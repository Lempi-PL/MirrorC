#include "common.h"
#include "globals.h"
#include "queue.h"
#include "progress.h"
#include "worker.h"
#include "traverse.h"

volatile sig_atomic_t g_interrupt_sig = 0;

#ifndef _WIN32
static void normalize_path(char* path) {
    char* p = path;
    char* out = path;
    char* start = path;
    
    while (*p) {
        if (p[0] == '.' && (p[1] == '/' || p[1] == '\0')) {
            p += (p[1] == '/') ? 2 : 1;
        } else if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            if (out > start + 1) {
                out--;
                while (out > start && *(out - 1) != '/') out--;
            }
            p += (p[2] == '/') ? 3 : 2;
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
    if (out > start + 1 && *(out - 1) == '/') *(out - 1) = '\0';
}
#endif

static void handle_sigint(int sig) { 
    (void)sig;
    g_interrupt_sig = 1;
}

int main(int argc, char** argv) 
{
#ifndef _WIN32
    // Zwiększenie limitu otwartych deskryptorów do maksimum
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
#endif

    signal(SIGINT, handle_sigint);

    // 1. Detekcja topologii procesora
    int threads_count;
#ifdef _WIN32
    SYSTEM_INFO si; 
    GetSystemInfo(&si); 
    threads_count = (int)si.dwNumberOfProcessors;
#else
    threads_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (threads_count < 1) threads_count = 4;

    // 2. Inicjalizacja kolejki
    mutex_init(&g_q.ctrl.lock); 
    cond_init(&g_q.ctrl.not_empty); 
    cond_init(&g_q.ctrl.not_full);
    
#ifdef _WIN32
    cond_init(&g_q.ctrl.wake_main);
#else
    // Ustawienie CLOCK_MONOTONIC, aby uniknąć problemów przy zmianie czasu systemowego (NTP)
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_q.ctrl.wake_main, &attr);
    pthread_condattr_destroy(&attr);
#endif
    atomic_init(&g_q.ctrl.in_flight, 0);
    atomic_store(&g_traverse_done, false);

    // 3. Alokacja statystyk (Alignment 64B zapobiega False Sharing)
#ifdef _WIN32
    g_worker_stats = (WorkerStats*)_aligned_malloc(threads_count * sizeof(WorkerStats), 64);
#else
    size_t stats_size = (threads_count * sizeof(WorkerStats) + 63) & ~(size_t)63;
    g_worker_stats = (WorkerStats*)aligned_alloc(64, stats_size);
#endif
    if (!g_worker_stats) abort();
    memset(g_worker_stats, 0, threads_count * sizeof(WorkerStats));

    // 4. Parsowanie argumentów
    const path_char_t *src_p = NULL, *dst_p = NULL;
#ifdef _WIN32
    int w_argc; 
    wchar_t** w_argv = CommandLineToArgvW(GetCommandLineW(), &w_argc);
    if (!w_argv || w_argc < 3) {
        if (w_argv) LocalFree(w_argv);
        return 1;
    }
    src_p = w_argv[1]; 
    dst_p = w_argv[2];
    
    wchar_t full_src[MAX_PATH_SAFE];
    wchar_t full_dst[MAX_PATH_SAFE];
        
    DWORD len_src = GetFullPathNameW(src_p, MAX_PATH_SAFE, full_src, NULL);
    DWORD len_dst = GetFullPathNameW(dst_p, MAX_PATH_SAFE, full_dst, NULL);
        
    if (len_src == 0 || len_src >= MAX_PATH_SAFE || len_dst == 0 || len_dst >= MAX_PATH_SAFE) {
        fprintf(stderr, "Fatal: Path resolution failed or exceeds limits.\n");
        if (w_argv) LocalFree(w_argv);
        return 1;
    }

    size_t src_len = wcslen(full_src);
    // Porównanie bez uwzględniania wielkości liter (_wcsnicmp)
    if (_wcsnicmp(full_src, full_dst, src_len) == 0 && 
        (full_dst[src_len] == L'\\' || full_dst[src_len] == L'\0')) {
        fprintf(stderr, "Fatal: Destination is inside source directory.\n");
        if (w_argv) LocalFree(w_argv);
        return 1;
    }

    HANDLE* h = (HANDLE*)malloc(sizeof(HANDLE) * threads_count);
    
    if (!h) {
        fprintf(stderr, "Fatal: Out of memory for thread handles.\n");
        // Destrukcja zasobów jądra 
        mutex_destroy(&g_q.ctrl.lock);
        cond_destroy(&g_q.ctrl.not_empty);
        cond_destroy(&g_q.ctrl.not_full);
        cond_destroy(&g_q.ctrl.wake_main);
        // Zwolnienie pamięci 
        _aligned_free(g_worker_stats);
        if (w_argv) LocalFree(w_argv);
        return 1;
    }
    HANDLE h_traverse;
#else
    if (argc < 3) return 1;
    src_p = argv[1]; 
    dst_p = argv[2];
    
    {
        char* real_src = realpath(src_p, NULL);
        if (!real_src) {
            fprintf(stderr, "Fatal: Source path does not exist or is inaccessible.\n");
            return 1;
        }

        char* real_dst = NULL;
        char* dst_work = strdup(dst_p);
        
        if (!dst_work) {
            fprintf(stderr, "Fatal: Out of memory for path validation.\n");
            free(real_src);
            return 1;
        }
        
        size_t dst_len = strlen(dst_p);
        char* suffix = (char*)malloc(dst_len + 2);
        if (!suffix) {
            fprintf(stderr, "Fatal: Out of memory for path validation.\n");
            free(real_src);
            free(dst_work);
            return 1;
        }
        suffix[0] = '\0';
        
        // Iteracyjne wspinanie się po drzewie katalogów
        while (1) {
            real_dst = realpath(dst_work, NULL);
            
            if (real_dst) {
                // Znaleziono istniejący przodek - odtwórz pełną ścieżkę
                if (suffix[0] != '\0') {
                    size_t real_len = strlen(real_dst);
                    size_t suffix_len = strlen(suffix);
                    char* full_dst = (char*)malloc(real_len + suffix_len + 2);
                    
                    // SPRAWDZENIE MALLOC
                    if (!full_dst) {
                        fprintf(stderr, "Fatal: Out of memory for path validation.\n");
                        free(real_dst);
                        free(real_src);
                        free(dst_work);
                        free(suffix);
                        return 1;
                    }
                    
                    // OBSŁUGA KATALOGU GŁÓWNEGO
                    if (strcmp(real_dst, "/") == 0) {
                        snprintf(full_dst, real_len + suffix_len + 2, "/%s", suffix);
                    } else {
                        snprintf(full_dst, real_len + suffix_len + 2, "%s/%s", real_dst, suffix);
                    }
                    
                    // KRYTYCZNE: Normalizacja ścieżki
                    normalize_path(full_dst);
                    free(real_dst);
                    real_dst = full_dst;
                }
                break;
            }
            
            // OBSŁUGA BŁĘDÓW INNYCH NIŻ ENOENT
            if (errno != ENOENT) {
                fprintf(stderr, "Fatal: Cannot resolve destination path (errno: %d).\n", errno);
                free(real_src);
                free(dst_work);
                free(suffix);
                return 1;
            }
            
            char* last_sep = strrchr(dst_work, '/');
            
            if (last_sep == NULL) {
                // Ścieżka relatywna bez katalogu (np. "backup")
                // KRYTYCZNE: Dodaj bieżącą nazwę na początek suffix
                size_t work_len = strlen(dst_work);
                size_t old_suffix_len = strlen(suffix);
                
                if (old_suffix_len > 0) {
                    // Przesuń stary suffix w prawo
                    memmove(suffix + work_len + 1, suffix, old_suffix_len + 1);
                    memcpy(suffix, dst_work, work_len);
                    suffix[work_len] = '/';
                } else {
                    strcpy(suffix, dst_work);
                }
                
                strcpy(dst_work, ".");
                real_dst = realpath(dst_work, NULL);
                
                if (real_dst) {
                    size_t real_len = strlen(real_dst);
                    size_t suffix_len = strlen(suffix);
                    char* full_dst = (char*)malloc(real_len + suffix_len + 2);
                    
                    // SPRAWDZENIE MALLOC
                    if (!full_dst) {
                        fprintf(stderr, "Fatal: Out of memory for path validation.\n");
                        free(real_dst);
                        free(real_src);
                        free(dst_work);
                        free(suffix);
                        return 1;
                    }
                    
                    if (strcmp(real_dst, "/") == 0) {
                        snprintf(full_dst, real_len + suffix_len + 2, "/%s", suffix);
                    } else {
                        snprintf(full_dst, real_len + suffix_len + 2, "%s/%s", real_dst, suffix);
                    }
                    
                    normalize_path(full_dst);
                    free(real_dst);
                    real_dst = full_dst;
                }
                break;
            }
            
            if (last_sep == dst_work) {
                // Osiągnięto korzeń "/"
                real_dst = realpath("/", NULL);
                
                if (real_dst && suffix[0] != '\0') {
                    size_t real_len = strlen(real_dst);
                    size_t suffix_len = strlen(suffix);
                    char* full_dst = (char*)malloc(real_len + suffix_len + 2);
                    
                    // SPRAWDZENIE MALLOC
                    if (!full_dst) {
                        fprintf(stderr, "Fatal: Out of memory for path validation.\n");
                        free(real_dst);
                        free(real_src);
                        free(dst_work);
                        free(suffix);
                        return 1;
                    }
                    
                    // real_dst to "/", więc używamy "/%s"
                    snprintf(full_dst, real_len + suffix_len + 2, "/%s", suffix);
                    normalize_path(full_dst);
                    free(real_dst);
                    real_dst = full_dst;
                }
                break;
            }
            
            // Skróć ścieżkę do rodzica i zapamiętaj obciętą część jako suffix
            size_t cut_len = strlen(last_sep + 1);
            size_t old_suffix_len = strlen(suffix);
            
            if (old_suffix_len > 0) {
                memmove(suffix + cut_len + 1, suffix, old_suffix_len + 1);
            }
            
            memcpy(suffix, last_sep + 1, cut_len);
            if (old_suffix_len > 0) {
                suffix[cut_len] = '/';
            }
            
            *last_sep = '\0';
            
            if (dst_work[0] == '\0') {
                strcpy(dst_work, ".");
            }
        }
        
        free(dst_work);
        free(suffix);
        
        // Walidacja: sprawdź czy dst jest wewnątrz src
        if (real_src && real_dst) {
            size_t src_len = strlen(real_src);
            if (strncmp(real_src, real_dst, src_len) == 0 && 
                (real_dst[src_len] == '/' || real_dst[src_len] == '\0')) {
                fprintf(stderr, "Fatal: Destination is inside source directory.\n");
                free(real_src); 
                free(real_dst);
                return 1;
            }
        }
        
        free(real_src); 
        if (real_dst) free(real_dst);
    }
    
    pthread_t* h = (pthread_t*)malloc(sizeof(pthread_t) * threads_count);
    
    if (!h) {
        fprintf(stderr, "Fatal: Out of memory for thread handles.\n");
        // Destrukcja zasobów jądra 
        cond_destroy(&g_q.ctrl.not_empty);
        cond_destroy(&g_q.ctrl.not_full);
        cond_destroy(&g_q.ctrl.wake_main);
        // Zwolnienie pamięci 
        free(g_worker_stats);
        return 1;
    }
    pthread_t h_traverse;
#endif

    // 5. Uruchomienie workerów
    int created_threads = 0;
    for (int i = 0; i < threads_count; i++) {
#ifdef _WIN32
        uintptr_t thread = _beginthreadex(NULL, 0, worker_fn, (void*)(intptr_t)i, 0, NULL);
        if (thread == 0) {
            fprintf(stderr, "Fatal: Failed to create worker thread %d.\n", i);
            break;
        }
        h[i] = (HANDLE)thread;
        created_threads++;
#else
        int err = pthread_create(&h[i], NULL, worker_fn, (void*)(intptr_t)i);
        if (err != 0) {
            fprintf(stderr, "Fatal: Failed to create worker thread %d (error: %d).\n", i, err);
            break;
        }
        created_threads++;
#endif
    }

    if (created_threads == 0) {
        atomic_store(&g_fatal_error, true);
        free(h);
#ifdef _WIN32
        _aligned_free(g_worker_stats);
        if (w_argv) LocalFree(w_argv);
#else
        free(g_worker_stats);
#endif
        // Destrukcja zasobów jądra (ETAP 3)
        mutex_destroy(&g_q.ctrl.lock);
        cond_destroy(&g_q.ctrl.not_empty);
        cond_destroy(&g_q.ctrl.not_full);
        cond_destroy(&g_q.ctrl.wake_main);
        return 1;
    }

    if (created_threads < threads_count) {
            atomic_store(&g_fatal_error, true);
            atomic_store(&g_shutdown, true); // KRYTYCZNE: Przerwanie pętli while w workerach
            mutex_lock(&g_q.ctrl.lock);
            cond_broadcast(&g_q.ctrl.not_empty); // Obudzenie uśpionych wątków
            mutex_unlock(&g_q.ctrl.lock);
    }
    

    // 6. URUCHOMIENIE TRAVERSAL W OSOBNYM WĄTKU
    TraverseArgs t_args; 
    t_args.src = src_p;
    t_args.dst = dst_p;

    printf("Status: Initializing parallel traversal...\n\n");
#ifdef _WIN32
    h_traverse = (HANDLE)_beginthreadex(NULL, 0, traverse_worker, &t_args, 0, NULL);
#else
    pthread_create(&h_traverse, NULL, traverse_worker, &t_args);
#endif

    // 7. Pętla monitorująca UI (działa RÓWNOLEGLE ze skanowaniem)
    while (true) 
    {
        print_progress(threads_count);

        if (g_interrupt_sig) atomic_store(&g_interrupt, 1);

        if (atomic_load_explicit(&g_interrupt, memory_order_acquire) || atomic_load(&g_fatal_error)) break;

        mutex_lock(&g_q.ctrl.lock);
        int pending_count = g_q.ctrl.count;
        int in_flight_count = atomic_load_explicit(&g_q.ctrl.in_flight, memory_order_acquire);
        bool done = atomic_load(&g_traverse_done);

        if (done && pending_count == 0 && in_flight_count == 0) 
        {
            mutex_unlock(&g_q.ctrl.lock);
            break;
        }

#ifdef _WIN32
        SleepConditionVariableCS(&g_q.ctrl.wake_main, &g_q.ctrl.lock, 200);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += 200000000;
        if (ts.tv_nsec >= 1000000000) 
        {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&g_q.ctrl.wake_main, &g_q.ctrl.lock, &ts);
#endif
        mutex_unlock(&g_q.ctrl.lock);
    }

    print_progress(threads_count);
    printf("\nStatus: Finalizing workers...\n");
    
    // 8. Sygnał zakończenia dla workerów
    atomic_store(&g_shutdown, true);

    mutex_lock(&g_q.ctrl.lock); 
    cond_broadcast(&g_q.ctrl.not_empty); 
    cond_broadcast(&g_q.ctrl.not_full);
    mutex_unlock(&g_q.ctrl.lock);

    // 9. Join wątku skanującego i workerów
#ifdef _WIN32
    WaitForSingleObject(h_traverse, INFINITE);
    CloseHandle(h_traverse);
    for (int i = 0; i < created_threads; i++) {
        WaitForSingleObject(h[i], INFINITE);
        CloseHandle(h[i]);
    }
#else
    pthread_join(h_traverse, NULL);
    for (int i = 0; i < created_threads; i++) {
        pthread_join(h[i], NULL);
    }
#endif

    // 10. Czyszczenie zasobów
    queue_cleanup(&g_q);
    
    free(h);

#ifdef _WIN32
    _aligned_free(g_worker_stats);
    if (w_argv) LocalFree(w_argv);
#else
    free(g_worker_stats);
#endif

    // 11. Raport końcowy
    if (atomic_load(&g_fatal_error))
    {
        fprintf(stderr, "Fatal: Operation aborted (No space left on device or I/O error).\n");
        return 1;
    }
    if (atomic_load_explicit(&g_interrupt, memory_order_acquire))
    {
        printf("Status: Interrupted by user.\n");
        return 130;
    }

    int err_cnt = atomic_load_explicit(&g_errors_count, memory_order_relaxed);
    if (err_cnt > 0)
    {
        printf("Status: Completed with %d non-fatal errors.\n", err_cnt);
        return 2;
    }

    printf("Status: Success. Operation completed cleanly.\n");
    return 0;
}