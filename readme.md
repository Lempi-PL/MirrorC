# Multi-Threaded File Transfer Utility (C11)

This utility is a high-performance, cross-platform file traversal and copying tool designed for maximum throughput and reliability. It utilizes a **Thread Pool** architecture combined with **Zero-Copy I/O** (on Linux) to saturate hardware capabilities.

---

## Key Features
* **Parallel Execution:** Distributes file I/O across 8 concurrent worker threads.
* **Atomic Transactions:** Files are written to a `.tmp` location and moved to the final destination only upon successful completion to prevent data corruption.
* **Custom Arena Allocation:** Uses a reference-counted memory arena to handle path strings, significantly reducing heap fragmentation and allocation overhead.
* **VFS Abstraction:** Unified logic for Windows (`Win32 API`) and Linux (`POSIX`).
* **Deep Path Support:** Bypasses the Windows 260-character limit using `\\?\` extended-length paths.

---

## Prerequisites

### Windows
* **Compiler:** Visual Studio (MSVC) or MinGW-w64.
* **Build Tool:** `nmake` or `gcc`.

### Linux
* **Compiler:** `gcc` or `clang`.
* **Libraries:** `pthread` (POSIX Threads).

---

## Compilation
The utility is written in standard C11. Use the following commands to compile with high-level optimizations:
**Linux:**
```bash
gcc -O3 -o file_util main.c -lpthread
```
**Windows (MinGW):**
``` bash
gcc -O3 -o file_util.exe main.c
```
**Windows (MSVC):**
``` bash
cl /O2 /Fe:file_util.exe main.c
```

---

## Usage
The utility requires a source path, a destination path, and an optional extension filter.
```bash
./file_util <source_directory> <destination_directory> [extension_filter]
```

### Examples
* **Full Directory Mirror:**
``` bash
./file_util /home/user/data /mnt/backup/data
```
* **Filter by Extension:** Only copy .log files from a server directory.
``` bash
./file_util C:\Logs D:\Backup\Logs .log
```
* **Source Code Backup:** Copy only .cpp files while ignoring other build artifacts.
``` bash
./file_util ./src ./backup .cpp
```

---

## Technical Architecture
The system operates using a Producer-Consumer model:
* **Producer (Main Thread):** Performs a breadth-first search (BFS) of the filesystem. It populates a synchronized task queue with file paths.
* **Consumers (8 Worker Threads):** Pull tasks from the queue and perform the actual I/O operations.
* **Arena Memory:** Shared memory blocks hold the path strings. A block is only freed once the producer and all workers associated with those paths release their references.

---

## Safety and Error Handling
* **Graceful Shutdown:** Responds to SIGINT (Ctrl+C). The producer stops immediately, and workers finish their current file before exiting.
* **Disk Space Guard:** If the destination disk returns an ENOSPC (Disk Full) error, the utility triggers a fatal error flag to stop all workers and prevent further write attempts.
* **Exclusions:** By default, the utility skips .git and node_modules to avoid unnecessary processing of dependency trees and version history.

---

## Performance Notes
Performance is primarily limited by your storage hardware:
* **SSD/NVMe:** Expect high throughput due to parallel I/O.
* **HDD:** Performance may vary; while multi-threading helps, mechanical seek times remain a bottleneck for small files.

