#include "worker.h"
#include "vfs_copy.h"
#include "globals.h"

#ifdef _WIN32
unsigned __stdcall worker_fn(void* arg) 
#else
void* worker_fn(void* arg) 
#endif
{
    int id = (int)(intptr_t)arg;
#ifdef _WIN32
    uint8_t* io_buf = (uint8_t*)_aligned_malloc(IO_CHUNK_SIZE, 64);
    size_t path_buf_elements = MAX_PATH_SAFE + 32; 
    path_char_t* path_buf = (path_char_t*)_aligned_malloc(path_buf_elements * sizeof(path_char_t), 64);
    path_char_t* dir_buf = (path_char_t*)_aligned_malloc(path_buf_elements * sizeof(path_char_t), 64);
    
    if (!io_buf || !path_buf || !dir_buf) {
        if (io_buf) _aligned_free(io_buf);
        if (path_buf) _aligned_free(path_buf);
        if (dir_buf) _aligned_free(dir_buf);
        return 0;
    }   

#else
    uint8_t* io_buf = (uint8_t*)aligned_alloc(64, IO_CHUNK_SIZE);
    size_t path_buf_elements = MAX_PATH_SAFE + 32;
    size_t path_buf_size = (path_buf_elements * sizeof(path_char_t) + 63) & ~(size_t)63;
    path_char_t* path_buf = (path_char_t*)aligned_alloc(64, path_buf_size);
    

    if (!io_buf || !path_buf) {
        if (io_buf) free(io_buf);
        if (path_buf) free(path_buf);
        return 0;
    }

#endif

    while (true)
    {
        CopyTask t = {0};
        mutex_lock(&g_q.ctrl.lock);
        while (g_q.ctrl.count == 0 && !atomic_load(&g_shutdown))
        {
            cond_wait(&g_q.ctrl.not_empty, &g_q.ctrl.lock);
        }
        if (g_q.ctrl.count == 0 && atomic_load(&g_shutdown))
        { 
            mutex_unlock(&g_q.ctrl.lock); 
            break; 
        }
        t = g_q.buffer[g_q.head_struct.head];
        g_q.head_struct.head = (g_q.head_struct.head + 1) % QUEUE_CAPACITY;
        g_q.ctrl.count--;
        atomic_fetch_add_explicit(&g_q.ctrl.in_flight, 1, memory_order_release); 
        cond_signal(&g_q.ctrl.not_full);
        mutex_unlock(&g_q.ctrl.lock);

        if (t.src)
        {
            if (!atomic_load(&g_fatal_error) && !atomic_load_explicit(&g_interrupt, memory_order_relaxed))
            {
#ifdef _WIN32
                int64_t result = vfs_copy_atomic(t.src, t.dst, true, io_buf, path_buf, dir_buf);
#else
                int64_t result = vfs_copy_atomic(t.src, t.dst, t.src_dir_fd, t.dst_dir_fd, true, io_buf);
#endif
                if (result < 0) {
                    int err = (int)(-result);
                    // Dodano obsługę kodów błędów Windows (ERROR_DISK_FULL = 112, ERROR_HANDLE_DISK_FULL = 39)
#ifdef _WIN32
                    if (err == 112 || err == 39 || err == 19) { // 19 = ERROR_WRITE_PROTECT
                        atomic_store(&g_fatal_error, true);
                    }
#else
                    if (err == ENOSPC || err == EIO) {
                        atomic_store(&g_fatal_error, true);
                    }
#endif
                    else {
                        fprintf(stderr, "\n[ERROR] Failed to copy: %" PRI_PATH " (code: %d)\n", t.src, err);
                        atomic_fetch_add_explicit(&g_errors_count, 1, memory_order_relaxed);
                    }
                } else {
                    // NAPRAWA: Aktualizacja statystyk (wcześniej martwy kod)
                    atomic_fetch_add_explicit(&g_worker_stats[id].bytes_copied, (uint64_t)result, memory_order_relaxed);
                    atomic_fetch_add_explicit(&g_worker_stats[id].files_completed, 1, memory_order_relaxed);
                }

            } else {
                // [KRYTYCZNE] Ścieżka błędu/przerwania. Skoro nie wywołujemy vfs_copy_atomic 
                // (które normalnie zamyka FDs), musimy posprzątać deskryptory ręcznie!
#ifndef _WIN32
                if (t.src_dir_fd >= 0) close(t.src_dir_fd);
                if (t.dst_dir_fd >= 0) close(t.dst_dir_fd);
#endif
            }
            free(t.src);
            free(t.dst);
        }
        if (atomic_fetch_sub_explicit(&g_q.ctrl.in_flight, 1, memory_order_acq_rel) == 1)
        {
            mutex_lock(&g_q.ctrl.lock);
            cond_signal(&g_q.ctrl.wake_main);
            mutex_unlock(&g_q.ctrl.lock);
        }
    }

#ifdef _WIN32
    _aligned_free(io_buf); 
    _aligned_free(path_buf);
    _aligned_free(dir_buf);
#else
    free(io_buf); 
    free(path_buf);
#endif
    return 0;
}