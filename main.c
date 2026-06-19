#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <errno.h>
#include <signal.h>
#include <inttypes.h>

#if defined(_WIN32)
    #include <windows.h>
    #include <process.h>
    #include <wchar.h>
    typedef wchar_t path_char_t;
    #define PRI_PATH "ls"
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/sendfile.h>
    #include <sys/mman.h>
    #include <sys/ioctl.h>
    #include <fcntl.h>
    #include <dirent.h>
    #include <linux/fs.h>
    #include <pthread.h>
    #include <time.h>
    
    #ifndef FICLONE
        #define FICLONE _IOW(0x94, 9, int)
    #endif

    typedef char path_char_t;
    #define PRI_PATH "s"
    #define PATH_SEP '/'
    #define PATH_STR "/"
#endif

#define IO_CHUNK_SIZE (2 * 1024 * 1024)
#define BATCH_SIZE 32
#define QUEUE_CAPACITY 2048
#define MAX_PATH_SAFE 32768

typedef struct
{
    alignas(64) atomic_uint_fast64_t bytes_copied;
    atomic_uint_fast64_t files_completed;
} WorkerStats;

typedef struct 
{ 
    path_char_t *src;
    path_char_t *dst;
} StackItem;

typedef struct 
{ 
    path_char_t *src;
    path_char_t *dst;
} CopyTask;

typedef struct 
{
    CopyTask tasks[BATCH_SIZE]; 
    int count; 
} TaskBatch;

typedef struct 
{
    const path_char_t* src;
    const path_char_t* dst;
} TraverseArgs;


static WorkerStats* g_worker_stats = NULL;
static atomic_uint_fast64_t g_total_size_bytes = 0;
static atomic_uint_fast64_t g_total_files_count = 0;
static atomic_bool g_traverse_done = false;
static atomic_bool g_shutdown = false;
static atomic_bool g_fatal_error = false;
static atomic_int g_errors_count = 0;
static atomic_int g_interrupt = 0;


// --- MONITORING & STATS ---
static void print_progress(int threads_count)
{
    uint64_t current_bytes = 0;
    uint64_t current_files = 0;
    uint64_t total_bytes = atomic_load(&g_total_size_bytes);
    uint64_t total_files = atomic_load(&g_total_files_count);

    for (int i = 0; i < threads_count; i++)
    {
        current_bytes += atomic_load(&g_worker_stats[i].bytes_copied);
        current_files += atomic_load(&g_worker_stats[i].files_completed);
    }

    double percent = 0.0;
    if (total_bytes > 0)
    {
        percent = (100.0 * (double)current_bytes / (double)total_bytes);
    } else if (atomic_load(&g_traverse_done) && total_files > 0)
    {
        percent = 100.0;
    } else if (atomic_load(&g_traverse_done) && total_files == 0)
    {
        percent = 100.0;
    }

    if (percent > 100.0) percent = 100.0;

    printf("\rProgress: [%-50.*s] %.1f%% (%" PRIu64 "/%" PRIu64 " files)", 
           (int)(percent / 2), "==================================================", 
           percent, current_files, total_files);
    fflush(stdout);
}

// --- QUEUE & THREADING ---
#ifdef _WIN32
    typedef CRITICAL_SECTION mutex_t;
    typedef CONDITION_VARIABLE cond_t;
    #define mutex_init(m) InitializeCriticalSection(m)
    #define mutex_lock(m) EnterCriticalSection(m)
    #define mutex_unlock(m) LeaveCriticalSection(m)
    #define cond_init(c) InitializeConditionVariable(c)
    #define cond_wait(c, m) SleepConditionVariableCS(c, m, INFINITE)
    #define cond_signal(c) WakeConditionVariable(c)
    #define cond_broadcast(c) WakeAllConditionVariable(c)
#else
    typedef pthread_mutex_t mutex_t;
    typedef pthread_cond_t cond_t;
    #define mutex_init(m) pthread_mutex_init(m, NULL)
    #define mutex_lock(m) pthread_mutex_lock(m)
    #define mutex_unlock(m) pthread_mutex_unlock(m)
    #define cond_init(c) pthread_cond_init(c, NULL)
    #define cond_wait(c, m) pthread_cond_wait(c, m)
    #define cond_signal(c) pthread_cond_signal(c)
    #define cond_broadcast(c) pthread_cond_broadcast(c)
#endif

typedef struct 
{
    // --- Sekcja chroniona Lockiem (Spatial Locality) ---
    // Te zmienne MUSZĄ być ciasno upakowane. Procesor wczyta je jednym strzałem do L1.
    mutex_t lock;
    cond_t not_empty;
    cond_t not_full;
    cond_t wake_main;

    int head; 
    int tail; 
    int count;

    // --- Sekcja Asynchroniczna (False Sharing Prevention) ---
    // Modyfikowana poza lockiem z wielu rdzeni. Izolujemy ją wymuszając nową linię Cache.
    alignas(64) atomic_int in_flight;

    // --- Pamięć Masowa ---
    alignas(64) CopyTask buffer[QUEUE_CAPACITY];
} TaskQueue;

static TaskQueue g_q;

#ifdef _WIN32
static void ensure_long_path(const wchar_t* src, wchar_t* dst, size_t dst_cap) 
{
    if (wcslen(src) >= 250 && wcsncmp(src, L"\\\\?\\", 4) != 0) 
        swprintf(dst, dst_cap, L"\\\\?\\%ls", src);
    else 
    {
        wcsncpy(dst, src, dst_cap);
        dst[dst_cap - 1] = L'\0';
    }
}
#endif

// --- ATOMIC VFS COPY ---

static int64_t vfs_copy_atomic(const path_char_t* src, const path_char_t* dst, bool meta, uint8_t* io_buf, path_char_t* path_buf) 
{
    bool ok = true;
    int64_t f_size = 0;
    const size_t tmp_cap = MAX_PATH_SAFE + 16;

#ifdef _WIN32
    wchar_t* long_src = (wchar_t*)malloc(MAX_PATH_SAFE * sizeof(wchar_t));
    wchar_t* long_dst = (wchar_t*)malloc(MAX_PATH_SAFE * sizeof(wchar_t));
    
    if (!long_src || !long_dst) 
    {
        free(long_src); 
        free(long_dst);
        return -1;
    }

    ensure_long_path(src, long_src, MAX_PATH_SAFE);
    ensure_long_path(dst, long_dst, MAX_PATH_SAFE);
    
    if (_snwprintf(path_buf, tmp_cap, L"%ls.tmp", long_dst) < 0) 
    {
        free(long_src); 
        free(long_dst);
        return -1;
    }
    
    HANDLE hIn = CreateFileW(long_src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hIn == INVALID_HANDLE_VALUE) 
    {
        free(long_src); 
        free(long_dst);
        return -1;
    }

    LARGE_INTEGER liSize;
    if (GetFileSizeEx(hIn, &liSize))
    {
        f_size = (int64_t)liSize.QuadPart;
    }

    HANDLE hOut = CreateFileW(path_buf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hOut == INVALID_HANDLE_VALUE)
    { 
        CloseHandle(hIn); 
        free(long_src); 
        free(long_dst);
        return -1; 
    }

    DWORD br, bw; 
    while (ReadFile(hIn, io_buf, IO_CHUNK_SIZE, &br, NULL) && br > 0) 
    {
        if (atomic_load_explicit(&g_interrupt, memory_order_relaxed) || atomic_load(&g_fatal_error))
        { 
            ok = false; 
            break;
        }
        if (!WriteFile(hOut, io_buf, br, &bw, NULL))
        { 
            ok = false; 
            break;
        }
    }
    
    if (ok && meta) 
    { 
        FILETIME c, a, w; 
        if (GetFileTime(hIn, &c, &a, &w)) SetFileTime(hOut, &c, &a, &w); 
    }
    
    CloseHandle(hIn); 
    CloseHandle(hOut);
    
    if (ok) 
    {
        if (!MoveFileExW(path_buf, long_dst, MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileW(path_buf);
            ok = false;
        }
    } 
    else 
    {
        DeleteFileW(path_buf);
    }
    
    free(long_src);
    free(long_dst);

#else
    if (snprintf(path_buf, tmp_cap, "%s.tmp", dst) >= (int)tmp_cap) return -1;

    int fd_in = open(src, O_RDONLY | O_CLOEXEC);
    if (fd_in < 0) return -1;

    struct stat st;
    if (fstat(fd_in, &st) < 0)
    { 
        close(fd_in); 
        return -1; 
    }
    f_size = (int64_t)st.st_size;

    int fd_out = open(path_buf, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode);
    if (fd_out < 0)
    {
        close(fd_in);
        return -1;
    }

    posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);
    posix_fadvise(fd_out, 0, 0, POSIX_FADV_SEQUENTIAL);

    bool use_fallback = false;
    off_t total = 0;

    if (ioctl(fd_out, FICLONE, fd_in) != 0)
    {
        while (total < st.st_size)
        {
            if (atomic_load_explicit(&g_interrupt, memory_order_relaxed) || atomic_load(&g_fatal_error))
            { 
                ok = false; 
                break; 
            }
            
            ssize_t ret = copy_file_range(fd_in, NULL, fd_out, NULL, st.st_size - total, 0);
            if (ret < 0)
            {
                if (errno == EINTR) continue;
                // Jeśli jądro nie wspiera zero-copy dla tego systemu plików, włączamy fallback
                if (errno == EXDEV || errno == ENOSYS || errno == EOPNOTSUPP)
                {
                    use_fallback = true;
                    break;
                }
                ok = false; 
                break;
            }
            if (ret == 0) break;
            total += ret;
        }

        if (use_fallback && ok)
        {
            lseek(fd_in, total, SEEK_SET);
            lseek(fd_out, total, SEEK_SET);
            
            while (total < st.st_size)
            {
                if (atomic_load_explicit(&g_interrupt, memory_order_relaxed) || atomic_load(&g_fatal_error))
                { 
                    ok = false; 
                    break; 
                }
                
                ssize_t br = read(fd_in, io_buf, IO_CHUNK_SIZE);
                if (br < 0)
                {
                    if (errno == EINTR) continue;
                    ok = false; 
                    break;
                }
                if (br == 0) break;

                ssize_t bw = 0;
                while (bw < br)
                {
                    ssize_t w = write(fd_out, io_buf + bw, br - bw);
                    if (w < 0)
                    {
                        if (errno == EINTR) continue;
                        ok = false; 
                        break;
                    }
                    bw += w;
                }
                
                if (!ok) break;
                total += br;
            }
        }
    }

    if (ok)
    {
        if (meta)
        {
            struct timespec ts[2] = {st.st_atim, st.st_mtim};
            futimens(fd_out, ts);
        }
    }

    if (ok)
    {
        // Prawidłowy zrzut metadanych do kroniki systemu plików. Wymagane ZAWSZE, nawet dla plików 0-bajtowych.
        fdatasync(fd_out);
        
        if (f_size > 0)
        {
            // Strony są czyste (clean). DONTNEED skutecznie zrzuci je z RAM-u.
            posix_fadvise(fd_in, 0, 0, POSIX_FADV_DONTNEED);
            posix_fadvise(fd_out, 0, 0, POSIX_FADV_DONTNEED);
        }
    }
    
    close(fd_out);
    close(fd_in);

    if (ok) 
    {
        if (rename(path_buf, dst) == 0) 
        {
            const char* last_slash = strrchr(dst, '/');
            if (last_slash) 
            {
                size_t dir_len = last_slash - dst;
                if (dir_len < MAX_PATH_SAFE) 
                {
                    char* parent_dir = (char*)malloc(MAX_PATH_SAFE);
                    if (parent_dir) 
                    {
                        snprintf(parent_dir, MAX_PATH_SAFE, "%.*s", (int)dir_len, dst);
                        int dfd = open(parent_dir, O_RDONLY | O_DIRECTORY);
                        if (dfd >= 0)
                        {
                            fsync(dfd);
                            close(dfd);
                        }
                        free(parent_dir);
                    }
                }
            }
        } 
        else 
        {
            unlink(path_buf);
            ok = false;
        }
    } 
    else 
    {
        unlink(path_buf);
    }
#endif

    return ok ? f_size : -1;
}


// --- Worker function ---
#ifdef _WIN32
static unsigned __stdcall worker_fn(void* arg) 
#else
static void* worker_fn(void* arg) 
#endif
{
    int id = (int)(intptr_t)arg;
#ifdef _WIN32
    uint8_t* io_buf = (uint8_t*)_aligned_malloc(IO_CHUNK_SIZE, 64);
    path_char_t* path_buf = (path_char_t*)_aligned_malloc(MAX_PATH_SAFE * sizeof(path_char_t), 64);
#else
    uint8_t* io_buf = (uint8_t*)aligned_alloc(64, IO_CHUNK_SIZE);
    path_char_t* path_buf = (path_char_t*)aligned_alloc(64, MAX_PATH_SAFE * sizeof(path_char_t) + 64);
#endif

    if (!io_buf || !path_buf) return 0;

    while (true)
    {
        CopyTask t = {0};
        mutex_lock(&g_q.lock);
        while (g_q.count == 0 && !atomic_load(&g_shutdown))
        {
            cond_wait(&g_q.not_empty, &g_q.lock);
        }
        if (g_q.count == 0 && atomic_load(&g_shutdown))
        { 
            mutex_unlock(&g_q.lock); 
            break; 
        }
        t = g_q.buffer[g_q.head];
        g_q.head = (g_q.head + 1) % QUEUE_CAPACITY;
        g_q.count--;
        atomic_fetch_add_explicit(&g_q.in_flight, 1, memory_order_release); 
        cond_signal(&g_q.not_full);
        mutex_unlock(&g_q.lock);

        if (t.src)
        {
            if (!atomic_load(&g_fatal_error) && !atomic_load_explicit(&g_interrupt, memory_order_relaxed))
            {
                int64_t result = vfs_copy_atomic(t.src, t.dst, true, io_buf, path_buf);
                
                if (result >= 0)
                {
                    if (result > 0)
                    {
                        atomic_fetch_add_explicit(&g_worker_stats[id].bytes_copied, (uint64_t)result, memory_order_relaxed);
                    }
                    atomic_fetch_add_explicit(&g_worker_stats[id].files_completed, 1, memory_order_relaxed);
                } 
                else
                {
                    if (errno == ENOSPC || errno == EIO)
                    {
                        atomic_store(&g_fatal_error, true);
                    }
                    else
                    {
                        fprintf(stderr, "\n[ERROR] Failed to copy: %" PRI_PATH " (errno: %d)\n", t.src, errno);
                        atomic_fetch_add_explicit(&g_errors_count, 1, memory_order_relaxed);
                    }
                }
                
            }
            free(t.src);
            free(t.dst);
        }
        if (atomic_fetch_sub_explicit(&g_q.in_flight, 1, memory_order_acq_rel) == 1)
        {
            mutex_lock(&g_q.lock);
            cond_broadcast(&g_q.wake_main);
            mutex_unlock(&g_q.lock);
        }
    }

#ifdef _WIN32
    _aligned_free(io_buf); 
    _aligned_free(path_buf);
#else
    free(io_buf); 
    free(path_buf);
#endif
    return 0;
}

// --- TRAVERSAL ---

static path_char_t* duplicate_path(const path_char_t* p)
{
    size_t len = 0;
#ifdef _WIN32
    len = wcslen(p);
#else
    len = strlen(p);
#endif
    path_char_t* dup = (path_char_t*)malloc((len + 1) * sizeof(path_char_t));
    if (dup) memcpy(dup, p, (len + 1) * sizeof(path_char_t));
    return dup;
}


static void queue_push_batch(TaskQueue* q, TaskBatch* b) 
{
    mutex_lock(&q->lock);
    while (q->count + b->count > QUEUE_CAPACITY && !atomic_load_explicit(&g_interrupt, memory_order_relaxed) && !atomic_load(&g_fatal_error))
    {
        cond_wait(&q->not_full, &q->lock);
    }

    if (!atomic_load_explicit(&g_interrupt, memory_order_relaxed) && !atomic_load(&g_fatal_error))
    {
        for (int i = 0; i < b->count; i++)
        {
            q->buffer[q->tail] = b->tasks[i];
            q->tail = (q->tail + 1) % QUEUE_CAPACITY;
        }
        q->count += b->count;
        cond_broadcast(&q->not_empty);
    }
    
    else
    {
        for (int i = 0; i < b->count; i++)
        {
            free(b->tasks[i].src);
            free(b->tasks[i].dst);
        }
    }
    mutex_unlock(&q->lock);
    b->count = 0;
}

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

    stack[0].src = duplicate_path(r_src);
    stack[0].dst = duplicate_path(r_dst);
    if (!stack[0].src || !stack[0].dst)
    {
        free(stack[0].src); free(stack[0].dst); free(stack);
        atomic_store(&g_fatal_error, true);
        return;
    }
    
    size_t s_cnt = 1;

#ifdef _WIN32
    CreateDirectoryW(r_dst, NULL);
    wchar_t* search_buf = (wchar_t*)malloc((MAX_PATH_SAFE + 8) * sizeof(wchar_t));
    if (!search_buf) 
    {
        free(stack[0].src); free(stack[0].dst); free(stack);
        atomic_store(&g_fatal_error, true);
        return;
    }
#else
    mkdir(r_dst, 0755);
#endif

    while (s_cnt > 0 && !atomic_load_explicit(&g_interrupt, memory_order_relaxed) && !atomic_load(&g_fatal_error)) 
    {
        StackItem cur = stack[--s_cnt];

#ifdef _WIN32
        int s_len = swprintf(search_buf, MAX_PATH_SAFE + 8, L"%ls\\*", cur.src);
        if (s_len > 0) 
        {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(search_buf, &fd);
            if (h != INVALID_HANDLE_VALUE) 
            {
                size_t b_src_len = wcslen(cur.src);
                size_t b_dst_len = wcslen(cur.dst);

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

                    memcpy(ns, cur.src, b_src_len * sizeof(wchar_t));
                    ns[b_src_len] = L'\\';
                    memcpy(ns + b_src_len + 1, fd.cFileName, (name_len + 1) * sizeof(wchar_t));

                    memcpy(nd, cur.dst, b_dst_len * sizeof(wchar_t));
                    nd[b_dst_len] = L'\\';
                    memcpy(nd + b_dst_len + 1, fd.cFileName, (name_len + 1) * sizeof(wchar_t));

                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) 
                    {
                        if (CreateDirectoryW(nd, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) 
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
                            stack[s_cnt++] = (StackItem){ ns, nd };
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
#else
        DIR* d = opendir(cur.src);
        if (d) 
        {
            size_t b_src_len = strlen(cur.src);
            size_t b_dst_len = strlen(cur.dst);
            struct dirent* de;

            while ((de = readdir(d)) && !atomic_load_explicit(&g_interrupt, memory_order_relaxed) && !atomic_load(&g_fatal_error)) 
            {
                if (de->d_name[0] == '.' && (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0')))
                    continue;

                size_t name_len = strlen(de->d_name);
                size_t ls = b_src_len + name_len + 2;
                size_t ld = b_dst_len + name_len + 2;

                if (ls >= MAX_PATH_SAFE || ld >= MAX_PATH_SAFE) continue;

                char *ns = (char*)malloc(ls * sizeof(char));
                char *nd = (char*)malloc(ld * sizeof(char));
                
                if (!ns || !nd)
                { 
                    free(ns); free(nd); 
                    atomic_store(&g_fatal_error, true); 
                    break; 
                }

                memcpy(ns, cur.src, b_src_len);
                ns[b_src_len] = '/';
                memcpy(ns + b_src_len + 1, de->d_name, name_len + 1);

                memcpy(nd, cur.dst, b_dst_len);
                nd[b_dst_len] = '/';
                memcpy(nd + b_dst_len + 1, de->d_name, name_len + 1);

                struct stat st_buf;
                bool is_dir = (de->d_type == DT_DIR);
                bool is_reg = (de->d_type == DT_REG);
                
                if (de->d_type == DT_UNKNOWN || de->d_type == DT_REG) 
                {
                    if (fstatat(dirfd(d), de->d_name, &st_buf, AT_SYMLINK_NOFOLLOW) == 0) 
                    {
                        is_dir = S_ISDIR(st_buf.st_mode);
                        is_reg = S_ISREG(st_buf.st_mode);
                    } 
                    else
                    {
                        free(ns); 
                        free(nd);
                        continue; 
                    }
                }

                if (is_dir) 
                {
                    if (mkdir(nd, 0755) == 0 || errno == EEXIST) 
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
                        stack[s_cnt++] = (StackItem){ ns, nd };
                    } 
                    else
                    {
                        free(ns); 
                        free(nd);
                    }
                } 
                else if (is_reg) 
                {
                    atomic_fetch_add_explicit(&g_total_size_bytes, (uint64_t)st_buf.st_size, memory_order_relaxed);
                    atomic_fetch_add_explicit(&g_total_files_count, 1, memory_order_relaxed);
                    
                    batch.tasks[batch.count++] = (CopyTask){ ns, nd };
                    if (batch.count == BATCH_SIZE) queue_push_batch(&g_q, &batch);
                }
                else
                {
                    free(ns); free(nd);
                }
            }
            closedir(d);
        }
#endif
        free(cur.src);
        free(cur.dst);
    }

    if (batch.count > 0) queue_push_batch(&g_q, &batch);
    
    while (s_cnt > 0)
    {
        StackItem c = stack[--s_cnt];
        free(c.src);
        free(c.dst);
    }
#ifdef _WIN32
    free(search_buf);
#endif
    free(stack);
}

// --- WĄTEK OPAKOWUJĄCY ---

#ifdef _WIN32
static unsigned __stdcall traverse_worker(void* arg) 
#else
static void* traverse_worker(void* arg) 
#endif
{
    TraverseArgs* ta = (TraverseArgs*)arg;
    traverse(ta->src, ta->dst);
    atomic_store(&g_traverse_done, true);
    mutex_lock(&g_q.lock);
    cond_broadcast(&g_q.wake_main);
    mutex_unlock(&g_q.lock);
    return 0;
}

static void handle_sigint(int sig) 
{ 
    (void)sig; 
    atomic_store_explicit(&g_interrupt, 1, memory_order_relaxed); 
}


// -- main ---
int main(int argc, char** argv) 
{
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

    mutex_init(&g_q.lock); 
    cond_init(&g_q.not_empty); 
    cond_init(&g_q.not_full);
    cond_init(&g_q.wake_main);

    atomic_init(&g_q.in_flight, 0);

    atomic_store(&g_traverse_done, false); // Reset flagi stanu

    // 3. Alokacja statystyk (Alignment 64B zapobiega False Sharing)
#ifdef _WIN32
    g_worker_stats = (WorkerStats*)_aligned_malloc(threads_count * sizeof(WorkerStats), 64);
#else
    g_worker_stats = (WorkerStats*)aligned_alloc(64, threads_count * sizeof(WorkerStats));
#endif
    if (!g_worker_stats) abort();
    memset(g_worker_stats, 0, threads_count * sizeof(WorkerStats));

    // 4. Parsowanie argumentów
    const path_char_t *src_p = NULL, *dst_p = NULL;
#ifdef _WIN32
    int w_argc; 
    wchar_t** w_argv = CommandLineToArgvW(GetCommandLineW(), &w_argc);
    if (!w_argv || w_argc < 3) return 1;
    src_p = w_argv[1]; 
    dst_p = w_argv[2];
    HANDLE* h = (HANDLE*)malloc(sizeof(HANDLE) * threads_count);
    HANDLE h_traverse;
#else
    if (argc < 3) return 1;
    src_p = argv[1]; 
    dst_p = argv[2];
    pthread_t* h = (pthread_t*)malloc(sizeof(pthread_t) * threads_count);
    pthread_t h_traverse;
#endif

    // 5. Uruchomienie workerów
    for (int i = 0; i < threads_count; i++) 
    {
#ifdef _WIN32
        h[i] = (HANDLE)_beginthreadex(NULL, 0, worker_fn, (void*)(intptr_t)i, 0, NULL);
#else
        pthread_create(&h[i], NULL, worker_fn, (void*)(intptr_t)i);
#endif
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

        if (atomic_load_explicit(&g_interrupt, memory_order_relaxed) || atomic_load(&g_fatal_error)) break;

        mutex_lock(&g_q.lock);
        int pending_count = g_q.count;
        int in_flight_count = atomic_load_explicit(&g_q.in_flight, memory_order_acquire);
        bool done = atomic_load(&g_traverse_done);

        if (done && pending_count == 0 && in_flight_count == 0) 
        {
            mutex_unlock(&g_q.lock);
            break;
        }

        // Czekaj na sygnał zakończenia, ale nie dłużej niż 200ms (dla odświeżenia UI)
#ifdef _WIN32
        SleepConditionVariableCS(&g_q.wake_main, &g_q.lock, 200);
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000;
        if (ts.tv_nsec >= 1000000000) 
        {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&g_q.wake_main, &g_q.lock, &ts);
#endif
        mutex_unlock(&g_q.lock);
    }

    print_progress(threads_count);
    printf("\nStatus: Finalizing workers...\n");
    
    // 8. Sygnał zakończenia dla workerów
    atomic_store(&g_shutdown, true);
    mutex_lock(&g_q.lock); 
    cond_broadcast(&g_q.not_empty); 
    cond_broadcast(&g_q.not_full);
    mutex_unlock(&g_q.lock);


    // 9. Join wątku skanującego i workerów
#ifdef _WIN32
    WaitForSingleObject(h_traverse, INFINITE);
    CloseHandle(h_traverse);
    for (int i = 0; i < threads_count; i++)
    {
        WaitForSingleObject(h[i], INFINITE); 
        CloseHandle(h[i]);
    }
#else
    pthread_join(h_traverse, NULL);
    for (int i = 0; i < threads_count; i++)
    {
        pthread_join(h[i], NULL);
    }
#endif

    // 10. Czyszczenie zasobów

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
    if (atomic_load_explicit(&g_interrupt, memory_order_relaxed))
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