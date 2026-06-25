#ifndef WORKER_H
#define WORKER_H

#include "common.h"

// --- Wątki robocze ---
#ifdef _WIN32
unsigned __stdcall worker_fn(void* arg);
#else
void* worker_fn(void* arg);
#endif

#endif // WORKER_H