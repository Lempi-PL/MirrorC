
#ifndef COMMON_H
#define COMMON_H

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

// --- Abstrakcja platformy ---
#if defined(_WIN32)
    #include <windows.h>
    #include <process.h>
    #include <wchar.h>
    
    typedef wchar_t path_char_t;
    #define PRI_PATH "ls"
    #define PATH_SEP L'\\'
    #define PATH_STR L"\\"
    
    // Mutex i Condition Variable dla Windows
    typedef CRITICAL_SECTION mutex_t;
    typedef CONDITION_VARIABLE cond_t;
    #define mutex_init(m) InitializeCriticalSection(m)
    #define mutex_lock(m) EnterCriticalSection(m)
    #define mutex_unlock(m) LeaveCriticalSection(m)
    #define mutex_destroy(m) DeleteCriticalSection(m)
    #define cond_init(c) InitializeConditionVariable(c)
    #define cond_wait(c, m) SleepConditionVariableCS(c, m, INFINITE)
    #define cond_signal(c) WakeConditionVariable(c)
    #define cond_broadcast(c) WakeAllConditionVariable(c)
    #define cond_destroy(c) (void)0
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/sendfile.h>
    #include <sys/resource.h>
    #include <sys/mman.h>
    #include <sys/ioctl.h>
    #include <fcntl.h>
    #include <dirent.h>
    #include <linux/fs.h>
    #include <pthread.h>
    #include <time.h>
    #include <liburing.h>
    #include <libgen.h>
    
    #ifndef FICLONE
        #define FICLONE _IOW(0x94, 9, int)
    #endif

    typedef char path_char_t;
    #define PRI_PATH "s"
    #define PATH_SEP '/'
    #define PATH_STR "/"
    
    // Mutex i Condition Variable dla POSIX
    typedef pthread_mutex_t mutex_t;
    typedef pthread_cond_t cond_t;
    #define mutex_init(m) pthread_mutex_init(m, NULL)
    #define mutex_lock(m) pthread_mutex_lock(m)
    #define mutex_unlock(m) pthread_mutex_unlock(m)
    #define mutex_destroy(m) pthread_mutex_destroy(m)
    #define cond_init(c) pthread_cond_init(c, NULL)
    #define cond_wait(c, m) pthread_cond_wait(c, m)
    #define cond_signal(c) pthread_cond_signal(c)
    #define cond_broadcast(c) pthread_cond_broadcast(c)
    #define cond_destroy(c) pthread_cond_destroy(c)
#endif

// --- Stałe konfiguracyjne ---
#define IO_CHUNK_SIZE (2 * 1024 * 1024)
#define BATCH_SIZE 32
#define QUEUE_CAPACITY 2048
#define MAX_PATH_SAFE 32768

// --- Struktury danych ---
typedef struct
{
    alignas(64) atomic_uint_fast64_t bytes_copied;
    atomic_uint_fast64_t files_completed;
} WorkerStats;

typedef struct 
{ 
    int src_dfd;      // Deskryptor OTWARTEGO katalogu źródłowego (O_PATH | O_DIRECTORY)
    int dst_dfd;      // Deskryptor OTWARTEGO katalogu docelowego (O_PATH | O_DIRECTORY)
    path_char_t *src_path; // Pełna ścieżka źródłowa (do budowania ścieżek dzieci i debugowania)
    path_char_t *dst_path; // Pełna ścieżka docelowa
} StackItem;

typedef struct 
{ 
    path_char_t *src;
    path_char_t *dst;
#ifndef _WIN32
    int src_dir_fd; // Deskryptor katalogu źródłowego
    int dst_dir_fd; // Deskryptor katalogu docelowego
#endif
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

// Kolejka tasków
typedef struct {
    alignas(64) mutex_t lock;
    cond_t not_empty;
    cond_t not_full;
    cond_t wake_main;
    int tail;
    int count;
    atomic_int in_flight; 
} QueueControl;

typedef struct {
    alignas(64) int head;
} QueueHead;

typedef struct {
    QueueControl ctrl;
    QueueHead head_struct;
    alignas(64) CopyTask buffer[QUEUE_CAPACITY];
} TaskQueue;

// --- Deklaracje funkcji pomocniczych ---
#ifdef _WIN32
void ensure_long_path(const wchar_t* src, wchar_t* dst, size_t dst_cap);
#endif

path_char_t* duplicate_path(const path_char_t* p);

#endif // COMMON_H