
#include "globals.h"

// --- Definicje zmiennych globalnych ---
WorkerStats* g_worker_stats = NULL;
atomic_uint_fast64_t g_total_size_bytes = 0;
atomic_uint_fast64_t g_total_files_count = 0;
atomic_bool g_traverse_done = false;
atomic_bool g_shutdown = false;
atomic_bool g_fatal_error = false;
atomic_int g_errors_count = 0;
atomic_int g_interrupt = 0;

TaskQueue g_q;

