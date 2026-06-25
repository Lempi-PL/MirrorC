#ifndef QUEUE_H
#define QUEUE_H

#include "common.h"

// --- Operacje na kolejce ---
void queue_push_batch(TaskQueue* q, TaskBatch* b);
void queue_cleanup(TaskQueue* q);

#endif // QUEUE_H