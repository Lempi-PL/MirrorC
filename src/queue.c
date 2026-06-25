#include "queue.h"
#include "globals.h"

void queue_push_batch(TaskQueue* q, TaskBatch* b) 
{
    mutex_lock(&q->ctrl.lock);
    while (q->ctrl.count + b->count > QUEUE_CAPACITY && !atomic_load_explicit(&g_interrupt, memory_order_relaxed) && !atomic_load(&g_fatal_error))
    {
        cond_wait(&q->ctrl.not_full, &q->ctrl.lock);
    }

    if (!atomic_load_explicit(&g_interrupt, memory_order_relaxed) && !atomic_load(&g_fatal_error))
    {
        for (int i = 0; i < b->count; i++)
        {
            q->buffer[q->ctrl.tail] = b->tasks[i];
            q->ctrl.tail = (q->ctrl.tail + 1) % QUEUE_CAPACITY;
        }
        q->ctrl.count += b->count;
        cond_broadcast(&q->ctrl.not_empty);
    }
    else
    {
        for (int i = 0; i < b->count; i++)
        {
            free(b->tasks[i].src);
            free(b->tasks[i].dst);
            // Zamykanie deskryptorów z odrzuconego batcha
#ifndef _WIN32
            if (b->tasks[i].src_dir_fd >= 0) close(b->tasks[i].src_dir_fd);
            if (b->tasks[i].dst_dir_fd >= 0) close(b->tasks[i].dst_dir_fd);
#endif
        }
        // Obudź główny wątek, by sprawdził g_fatal_error
        cond_signal(&q->ctrl.wake_main);
    }
    mutex_unlock(&q->ctrl.lock);
    b->count = 0;
}

void queue_cleanup(TaskQueue* q) {
    mutex_lock(&q->ctrl.lock);
    while (q->ctrl.count > 0) { 
        CopyTask t = q->buffer[q->head_struct.head];
        q->head_struct.head = (q->head_struct.head + 1) % QUEUE_CAPACITY;
        q->ctrl.count--;
        free(t.src);
        free(t.dst);

        // Zamykanie osieroconych deskryptorów w kolejce
#ifndef _WIN32
        if (t.src_dir_fd >= 0) close(t.src_dir_fd);
        if (t.dst_dir_fd >= 0) close(t.dst_dir_fd);
#endif
    }
    mutex_unlock(&q->ctrl.lock);
    
    // DESTRUKCJA ZASOBÓW JĄDRA
    cond_destroy(&q->ctrl.not_empty);
    cond_destroy(&q->ctrl.not_full);
    cond_destroy(&q->ctrl.wake_main);
    mutex_destroy(&q->ctrl.lock);
}