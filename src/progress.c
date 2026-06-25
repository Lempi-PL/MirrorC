
#include "progress.h"
#include "globals.h"

void print_progress(int threads_count)
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
