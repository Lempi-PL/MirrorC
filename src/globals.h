#ifndef GLOBALS_H
#define GLOBALS_H

#include "common.h"

// --- Zmienne globalne (deklaracje extern) ---
extern WorkerStats* g_worker_stats;
extern atomic_uint_fast64_t g_total_size_bytes;
extern atomic_uint_fast64_t g_total_files_count;
extern atomic_bool g_traverse_done;
extern atomic_bool g_shutdown;
extern atomic_bool g_fatal_error;
extern atomic_int g_errors_count;
extern atomic_int g_interrupt;

extern TaskQueue g_q;

#endif // GLOBALS_H
