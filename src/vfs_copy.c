#include "vfs_copy.h"
#include "globals.h"

#ifdef _WIN32
void ensure_long_path(const wchar_t* src, wchar_t* dst, size_t dst_cap) {
    // thread_local eliminuje 64KB ze stosu i narzut alokacji, zachowując thread-safety
    static thread_local wchar_t abs_path[MAX_PATH_SAFE];
    
    DWORD len = GetFullPathNameW(src, MAX_PATH_SAFE, abs_path, NULL);
    if (len == 0 || len >= MAX_PATH_SAFE) {
        wcsncpy(dst, src, dst_cap - 1);
        dst[dst_cap - 1] = L'\0';
        return;
    }
    
   if (len >= 250) {
        if (wcsncmp(abs_path, L"\\\\?\\", 4) == 0) {
            wcsncpy(dst, abs_path, dst_cap - 1);
            dst[dst_cap - 1] = L'\0';
        } else if (wcsncmp(abs_path, L"\\\\", 2) == 0) {
            int w_len = _snwprintf(dst, dst_cap, L"\\\\?\\UNC\\%ls", abs_path + 2);
            if (w_len < 0 || (size_t)w_len >= dst_cap) dst[dst_cap - 1] = L'\0'; // ZABEZPIECZENIE
        } else {
            int w_len = _snwprintf(dst, dst_cap, L"\\\\?\\%ls", abs_path);
            if (w_len < 0 || (size_t)w_len >= dst_cap) dst[dst_cap - 1] = L'\0'; // ZABEZPIECZENIE
        }
    } else {
        wcsncpy(dst, abs_path, dst_cap - 1);
    }
    dst[dst_cap - 1] = L'\0';
}
#endif

path_char_t* duplicate_path(const path_char_t* p)
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


#ifdef _WIN32
// ============================================================================
// WERSJA WINDOWS (Bez zmian w sygnaturze)
// ============================================================================
int64_t vfs_copy_atomic(const path_char_t* src, const path_char_t* dst, bool meta, uint8_t* io_buf, path_char_t* path_buf, path_char_t* dir_buf)
{
    bool ok = true;
    int64_t f_size = 0;
    const size_t tmp_cap = MAX_PATH_SAFE + 16;

    wchar_t* long_src = (wchar_t*)malloc(MAX_PATH_SAFE * sizeof(wchar_t));
    wchar_t* long_dst = (wchar_t*)malloc(MAX_PATH_SAFE * sizeof(wchar_t));
    
    if (!long_src || !long_dst) 
    {
        DWORD err = GetLastError();
        free(long_src); 
        free(long_dst);
        return -(int64_t)(err > 0 ? err : 1);
    }

    ensure_long_path(src, path_buf, MAX_PATH_SAFE); // path_buf zawiera teraz ścieżkę źródłową
    HANDLE hIn = CreateFileW(path_buf, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 
                             FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hIn == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return -(int64_t)(err > 0 ? err : 1);
    }
    
    ensure_long_path(dst, dir_buf, MAX_PATH_SAFE); // dir_buf zawiera teraz ścieżkę docelową
    if (_snwprintf(path_buf, tmp_cap, L"%ls.tmp", dir_buf) < 0) { // Nadpisz path_buf ścieżką .tmp
        DWORD err = GetLastError();
        CloseHandle(hIn);
        return -(int64_t)(err > 0 ? err : 1);
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
        DWORD err = GetLastError(); 
        CloseHandle(hIn); 
        free(long_src); 
        free(long_dst);
        return -(int64_t)(err > 0 ? err : 1);
    }

    DWORD br, bw; 
    while (ReadFile(hIn, io_buf, IO_CHUNK_SIZE, &br, NULL) && br > 0) 
    {
        if (atomic_load_explicit(&g_interrupt, memory_order_acquire) || atomic_load(&g_fatal_error))
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

    return ok ? f_size : -(int64_t)(GetLastError() > 0 ? GetLastError() : 1);
}

#else
// ============================================================================
// WERSJA LINUX (Zabezpieczona przed TOCTOU, z deskryptorami katalogów)
// ============================================================================
int64_t vfs_copy_atomic(const path_char_t* src, const path_char_t* dst, int src_dir_fd, int dst_dir_fd, bool meta, uint8_t* io_buf) 
{
    bool ok = true;
    int64_t f_size = 0;
    int err_code = 0;

    // Ekstrakcja nazwy pliku ze ścieżki
    const char* src_name = strrchr(src, '/');
    src_name = src_name ? src_name + 1 : src;
    
    const char* dst_name = strrchr(dst, '/');
    dst_name = dst_name ? dst_name + 1 : dst;

    // 1. Otwarcie źródła z openat (bezpieczeństwo TOCTOU na katalogach pośrednich)
    int fd_in = openat(src_dir_fd, src_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd_in < 0) {
        err_code = errno;
        close(src_dir_fd); close(dst_dir_fd);
        return -err_code;
    }

    struct stat st;
    if (fstat(fd_in, &st) < 0) {
        err_code = errno;
        close(fd_in);
        close(src_dir_fd); close(dst_dir_fd);
        return -err_code;
    }
    f_size = (int64_t)st.st_size;

    // 2. Użycie O_TMPFILE dla atomowości (plik anonimowy, niewidoczny, automatyczne sprzątanie)
    int fd_out = openat(dst_dir_fd, ".", O_WRONLY | O_TMPFILE | O_CLOEXEC, 0600);
    if (fd_out < 0) {
        err_code = errno;
        close(fd_in);
        close(src_dir_fd); close(dst_dir_fd); 
        return -err_code;
    }

    posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);
    posix_fadvise(fd_out, 0, 0, POSIX_FADV_SEQUENTIAL);
    
    bool use_fallback = false;
    off_t total = 0;
    
    // Próba reflinku (Btrfs/XFS)
    if (ioctl(fd_out, FICLONE, fd_in) != 0)
    {
        while (total < st.st_size) {
            if (atomic_load_explicit(&g_interrupt, memory_order_acquire) || atomic_load(&g_fatal_error))
            { ok = false; break; }
            
            ssize_t ret = copy_file_range(fd_in, NULL, fd_out, NULL, st.st_size - total, 0);
            if (ret < 0) {
                if (errno == EINTR) continue;
                if (errno == EXDEV || errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL) {
                    use_fallback = true;
                    break;
                }
                ok = false; break;
            }
            if (ret == 0) break;
            total += ret;
        }

        if (ok && total < st.st_size) ok = false;

        if (use_fallback && ok) {
            off_t current_offset = total;
            while (current_offset < st.st_size) {
                if (atomic_load_explicit(&g_interrupt, memory_order_acquire) || atomic_load(&g_fatal_error)) {
                    ok = false; break;
                }
                size_t chunk = (st.st_size - current_offset > IO_CHUNK_SIZE) ? IO_CHUNK_SIZE : (size_t)(st.st_size - current_offset);
                ssize_t bytes_read = pread(fd_in, io_buf, chunk, current_offset);
                if (bytes_read < 0) { if (errno == EINTR) continue; ok = false; break; } 
                if (bytes_read == 0) { if (current_offset < st.st_size) ok = false; break; }

                size_t to_write = (size_t)bytes_read;
                size_t written_total = 0;
                while (written_total < to_write) {
                    ssize_t bytes_written = pwrite(fd_out, io_buf + written_total, to_write - written_total, current_offset + written_total);
                    if (bytes_written < 0) { if (errno == EINTR) continue; ok = false; break; }
                    if (bytes_written == 0) { ok = false; break; }
                    written_total += (size_t)bytes_written;
                }
                if (!ok) break;
                current_offset += bytes_read;
            }
            if (ok) total = current_offset;
        }
    }

    if (ok && meta) {
        struct timespec ts[2] = {st.st_atim, st.st_mtim};
        futimens(fd_out, ts);
        if (fchown(fd_out, st.st_uid, st.st_gid) < 0 && errno != EPERM) ok = false;
    }
    
    mode_t safe_mode = st.st_mode & ~(mode_t)(S_ISUID | S_ISGID);
    if (fchmod(fd_out, safe_mode) < 0) ok = false;

    if (ok) {
        fdatasync(fd_out);
        if (f_size > 0) {
            posix_fadvise(fd_in, 0, 0, POSIX_FADV_DONTNEED);
            posix_fadvise(fd_out, 0, 0, POSIX_FADV_DONTNEED);
        }
    }
    
    close(fd_in);

    if (ok) {
        // Materializacja anonimowego pliku O_TMPFILE do docelowej ścieżki przez /proc/self/fd/
        char proc_fd[32];
        snprintf(proc_fd, sizeof(proc_fd), "/proc/self/fd/%d", fd_out);
        
        // AT_SYMLINK_FOLLOW jest wymagane, aby linkat poprawnie zmaterializował plik z /proc/self/fd/
        if (linkat(AT_FDCWD, proc_fd, dst_dir_fd, dst_name, AT_SYMLINK_FOLLOW) == 0) {
            fsync(dst_dir_fd); // Zabezpieczenie trwałości metadanych katalogu
        } else {
            err_code = errno;
            ok = false;
        }
    }
    
    close(fd_out); // Jeśli ok=false, close(fd_out) automatycznie usunie anonimowy plik O_TMPFILE

    if (!ok && err_code == 0) err_code = errno;

    close(src_dir_fd);
    close(dst_dir_fd);

    return ok ? f_size : -err_code;
}
#endif