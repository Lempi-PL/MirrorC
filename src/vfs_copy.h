#ifndef VFS_COPY_H
#define VFS_COPY_H

#include "common.h"

#ifdef _WIN32
int64_t vfs_copy_atomic(const path_char_t* src, const path_char_t* dst, bool meta, uint8_t* io_buf, path_char_t* path_buf, path_char_t* dir_buf);

#else
// Nowa sygnatura dla Linuksa przyjmująca deskryptory katalogów
int64_t vfs_copy_atomic(const path_char_t* src, const path_char_t* dst, int src_dir_fd, int dst_dir_fd, bool meta, uint8_t* io_buf);
#endif

#endif // VFS_COPY_H