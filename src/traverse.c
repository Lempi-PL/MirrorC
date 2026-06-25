
#include "traverse.h"
#include "queue.h"
#include "globals.h"

static void traverse(const path_char_t* r_src, const path_char_t* r_dst) 
{
    size_t s_cap = 2048;
    StackItem* stack = (StackItem*)malloc(sizeof(StackItem) * s_cap);
    TaskBatch batch = { .count = 0 };
    if (!stack)
    {
        atomic_store(&g_fatal_error, true);
        return;
    }

    size_t s_cnt = 0; // Inicjalizujemy na 0, ustawimy na 1 po poprawnej inicjalizacji stack[0]

#ifdef _WIN32
    // --- INICJALIZACJA KORZENIA (WINDOWS) ---
    stack[0].src_dfd = -1; // Nie używamy FD w Windows
    stack[0].dst_dfd = -1;
    stack[0].src_path = duplicate_path(r_src);
    stack[0].dst_path = duplicate_path(r_dst);
    
    if (!stack[0].src_path || !stack[0].dst_path)
    {
        free(stack[0].src_path); free(stack[0].dst_path); free(stack);
        atomic_store(&g_fatal_error, true);
        return;
    }
    s_cnt = 1;

    CreateDirectoryW(r_dst, NULL);
    wchar_t* search_buf = (wchar_t*)malloc((MAX_PATH_SAFE + 8) * sizeof(wchar_t));
    if (!search_buf) 
    {
        free(stack[0].src_path); free(stack[0].dst_path); free(stack);
        atomic_store(&g_fatal_error, true);
        return;
    }

    // --- PĘTLA GŁÓWNA (WINDOWS) ---
    while (s_cnt > 0 && !atomic_load_explicit(&g_interrupt, memory_order_relaxed) && !atomic_load(&g_fatal_error)) 
    {
        StackItem cur = stack[--s_cnt];

        int s_len = _snwprintf(search_buf, MAX_PATH_SAFE + 8, L"%ls\\*", cur.src_path);
        if (s_len > 0) 
        {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(search_buf, &fd);
            if (h != INVALID_HANDLE_VALUE) 
            {
                size_t b_src_len = wcslen(cur.src_path); // POPRAWIONE
                size_t b_dst_len = wcslen(cur.dst_path); // POPRAWIONE

                do
                {
                    if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
                        continue;
                    
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                        continue;

                    size_t name_len = wcslen(fd.cFileName);
                    size_t ls = b_src_len + name_len + 2; 
                    size_t ld = b_dst_len + name_len + 2;

                    if (ls >= MAX_PATH_SAFE || ld >= MAX_PATH_SAFE) continue;

                    path_char_t *ns = (path_char_t*)malloc(ls * sizeof(wchar_t));
                    path_char_t *nd = (path_char_t*)malloc(ld * sizeof(wchar_t));
                    
                    if (!ns || !nd)
                    { 
                        free(ns); free(nd); 
                        atomic_store(&g_fatal_error, true); 
                        break; 
                    }

                    memcpy(ns, cur.src_path, b_src_len * sizeof(wchar_t)); // POPRAWIONE
                    ns[b_src_len] = L'\\';
                    memcpy(ns + b_src_len + 1, fd.cFileName, (name_len + 1) * sizeof(wchar_t));

                    memcpy(nd, cur.dst_path, b_dst_len * sizeof(wchar_t)); // POPRAWIONE
                    nd[b_dst_len] = L'\\';
                    memcpy(nd + b_dst_len + 1, fd.cFileName, (name_len + 1) * sizeof(wchar_t));

                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) 
                    {
                        // Weryfikacja TOCTOU dla istniejących katalogów
                        bool dir_ok = false;
                        if (CreateDirectoryW(nd, NULL)) {
                            dir_ok = true;
                        } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
                            DWORD attrs = GetFileAttributesW(nd);
                            // Upewniamy się, że to nie jest złośliwy Reparse Point (np. Junction)
                            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                                dir_ok = true;
                            }
                        }

                        if (dir_ok) 
                        {
                            if (s_cnt >= s_cap) 
                            {
                                size_t new_cap = s_cap * 2;
                                StackItem* shadow = (StackItem*)realloc(stack, sizeof(StackItem) * new_cap);
                                if (!shadow) 
                                {
                                    free(ns); free(nd);
                                    atomic_store(&g_fatal_error, true);
                                    break; 
                                }
                                stack = shadow;
                                s_cap = new_cap;
                            }
                            // POPRAWIONE: Jawna inicjalizacja pól struktury
                            stack[s_cnt++] = (StackItem){ .src_dfd = -1, .dst_dfd = -1, .src_path = ns, .dst_path = nd };
                        }
                        else
                        {
                            free(ns); free(nd);
                        }
                    } 
                    else 
                    {
                        uint64_t f_size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                        atomic_fetch_add_explicit(&g_total_size_bytes, f_size, memory_order_relaxed);
                        atomic_fetch_add_explicit(&g_total_files_count, 1, memory_order_relaxed);

                        batch.tasks[batch.count++] = (CopyTask){ ns, nd };
                        if (batch.count == BATCH_SIZE) queue_push_batch(&g_q, &batch);
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        
        // Zwalniamy zasoby bieżącego elementu
        free(cur.src_path);
        free(cur.dst_path);
    }

#else
    // --- INICJALIZACJA KORZENIA (LINUX) ---
    int root_src_dfd = open(r_src, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root_src_dfd < 0) {
        free(stack);
        atomic_store(&g_fatal_error, true);
        return;
    }
    
    // Pobieramy uprawnienia katalogu źródłowego, aby zachować je w docelowym
    struct stat root_st;
    mode_t root_mode = 0755; // Fallback
    if (fstat(root_src_dfd, &root_st) == 0) {
        root_mode = root_st.st_mode & 0777;
    }
    
    mkdir(r_dst, root_mode);
    
    int root_dst_dfd = open(r_dst, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root_dst_dfd < 0) {
        close(root_src_dfd);
        free(stack);
        atomic_store(&g_fatal_error, true);
        return;
    }

    stack[0].src_dfd = root_src_dfd;
    stack[0].dst_dfd = root_dst_dfd;
    stack[0].src_path = duplicate_path(r_src);
    stack[0].dst_path = duplicate_path(r_dst);
    
    if (!stack[0].src_path || !stack[0].dst_path) {
        close(root_src_dfd); close(root_dst_dfd);
        free(stack[0].src_path); free(stack[0].dst_path); free(stack);
        atomic_store(&g_fatal_error, true);
        return;
    }
    s_cnt = 1;

    // --- PĘTLA GŁÓWNA (LINUX) ---
    while (s_cnt > 0 && !atomic_load_explicit(&g_interrupt, memory_order_relaxed) && !atomic_load(&g_fatal_error)) 
    {
        StackItem cur = stack[--s_cnt];

        // POPRAWIONE: O_PATH nie pozwala na fdopendir. Musimy otworzyć "." z O_RDONLY.
        int real_src_dfd = openat(cur.src_dfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (real_src_dfd < 0) {
            close(cur.src_dfd);
            close(cur.dst_dfd);
            free(cur.src_path);
            free(cur.dst_path);
            continue;
        }

        DIR* d = fdopendir(real_src_dfd);
        if (!d) {
            close(real_src_dfd);
            close(cur.src_dfd);
            close(cur.dst_dfd);
            free(cur.src_path);
            free(cur.dst_path);
            continue;
        }

        size_t b_src_len = strlen(cur.src_path);
        size_t b_dst_len = strlen(cur.dst_path);
        struct dirent* de;

        while ((de = readdir(d)) && !atomic_load_explicit(&g_interrupt, memory_order_relaxed) && !atomic_load(&g_fatal_error)) 
        {
            if (de->d_name[0] == '.' && (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0')))
                continue;

            size_t name_len = strlen(de->d_name);
            size_t ls = b_src_len + name_len + 2;
            size_t ld = b_dst_len + name_len + 2;

            if (ls >= MAX_PATH_SAFE || ld >= MAX_PATH_SAFE) continue;

            char *ns = (char*)malloc(ls);
            char *nd = (char*)malloc(ld);
            
            if (!ns || !nd) { 
                free(ns); free(nd); 
                atomic_store(&g_fatal_error, true); 
                break; 
            }

            snprintf(ns, ls, "%s/%s", cur.src_path, de->d_name);
            snprintf(nd, ld, "%s/%s", cur.dst_path, de->d_name);

            struct stat st_buf;
            bool is_dir = false;
            bool is_reg = false;
            
            if (fstatat(dirfd(d), de->d_name, &st_buf, AT_SYMLINK_NOFOLLOW) == 0) {
                is_dir = S_ISDIR(st_buf.st_mode);
                is_reg = S_ISREG(st_buf.st_mode);
            } else {
                free(ns); free(nd);
                continue; 
            }

            if (is_dir) 
            {
                // Zachowuje uprawnienia katalogu źródłowego (st_buf jest już pobrane wyżej przez fstatat)
                // Maska 0777 odcina bity SUID/SGID, które mogą powodować problemy przy tworzeniu.
                if (mkdirat(cur.dst_dfd, de->d_name, st_buf.st_mode & 0777) == 0 || errno == EEXIST) 
                {
                    int child_src_dfd = openat(dirfd(d), de->d_name, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                    
                    // O_NOFOLLOW dla ścieżki docelowej.
                    // Jeśli w czasie między mkdirat a openat atakujący podmieni katalog na symlink, 
                    // openat zwróci błąd ELOOP, zamiast otworzyć plik w innej lokalizacji.
                    int child_dst_dfd = openat(cur.dst_dfd, de->d_name, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

                    if (child_src_dfd >= 0 && child_dst_dfd >= 0) 
                    {
                        if (s_cnt >= s_cap) {
                            size_t new_cap = s_cap * 2;
                            StackItem* shadow = (StackItem*)realloc(stack, sizeof(StackItem) * new_cap);
                            if (!shadow) {
                                free(ns); free(nd);
                                close(child_src_dfd); close(child_dst_dfd);
                                atomic_store(&g_fatal_error, true);
                                break; 
                            }
                            stack = shadow;
                            s_cap = new_cap;
                        }
                        stack[s_cnt++] = (StackItem){ 
                            .src_dfd = child_src_dfd, 
                            .dst_dfd = child_dst_dfd, 
                            .src_path = ns, 
                            .dst_path = nd 
                        };
                    } else {
                        if (child_src_dfd >= 0) close(child_src_dfd);
                        if (child_dst_dfd >= 0) close(child_dst_dfd);
                        free(ns); free(nd);
                    }
                } else {
                    free(ns); free(nd);
                }
            } 
            else if (is_reg) 
            {
                atomic_fetch_add_explicit(&g_total_size_bytes, (uint64_t)st_buf.st_size, memory_order_relaxed);
                atomic_fetch_add_explicit(&g_total_files_count, 1, memory_order_relaxed);
                
                // Duplikujemy deskryptory
                int task_src_fd = dup(dirfd(d));
                int task_dst_fd = dup(cur.dst_dfd);

                // SPRAWDZENIE WYNIKU dup() - kluczowe dla uniknięcia EBADF w workerze
                if (task_src_fd < 0 || task_dst_fd < 0) {
                    // Jeśli dup() zawiodło (np. EMFILE - zbyt wiele otwartych FD),
                    // musimy poczekać, aż workery zwolnią zasoby.
                    // Najprostsze rozwiązanie: ustawiamy g_fatal_error i przerywamy.
                    // W pełnej implementacji można by dodać pętlę retry z sleep().
                    if (task_src_fd >= 0) close(task_src_fd);
                    if (task_dst_fd >= 0) close(task_dst_fd);
                    free(ns); free(nd);
                    
                    fprintf(stderr, "Fatal: Too many open files (EMFILE). Consider increasing ulimit -n.\n");
                    atomic_store(&g_fatal_error, true);
                    break;
                }

                batch.tasks[batch.count++] = (CopyTask){ ns, nd, task_src_fd, task_dst_fd };
                if (batch.count == BATCH_SIZE) queue_push_batch(&g_q, &batch);
            }
            else
            {
                free(ns); free(nd);
            }
        }
        
        // closedir zamyka real_src_dfd (przejęty przez fdopendir)
        closedir(d);
        // Zamykamy oryginalny O_PATH FD oraz FD docelowy
        close(cur.src_dfd);
        close(cur.dst_dfd);
#endif

        // POPRAWIONE: Wspólne zwalnianie pamięci dla obu platform (po #endif)
        free(cur.src_path);
        free(cur.dst_path);
    }

    if (batch.count > 0) queue_push_batch(&g_q, &batch);
    
    // Cleanup stosu w przypadku przerwania (Ctrl+C) lub błędu
    while (s_cnt > 0) {
        StackItem c = stack[--s_cnt];
#ifndef _WIN32
        close(c.src_dfd);
        close(c.dst_dfd);
#endif
        free(c.src_path);
        free(c.dst_path);
    }

#ifdef _WIN32
    free(search_buf);
#endif
    free(stack);
}

#ifdef _WIN32
unsigned __stdcall traverse_worker(void* arg) 
#else
void* traverse_worker(void* arg) 
#endif
{
    TraverseArgs* ta = (TraverseArgs*)arg;
    traverse(ta->src, ta->dst);
    atomic_store(&g_traverse_done, true);
    mutex_lock(&g_q.ctrl.lock);
    cond_signal(&g_q.ctrl.wake_main);
    mutex_unlock(&g_q.ctrl.lock);
    return 0;
}