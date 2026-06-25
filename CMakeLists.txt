cmake_minimum_required(VERSION 3.10)
project(filecopy C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Opcje
option(USE_IO_URING "Use io_uring on Linux" ON)

# Źródła
set(SOURCES
    src/main.c
    src/globals.c
    src/queue.c
    src/progress.c
    src/vfs_copy.c
    src/traverse.c
    src/worker.c
)

# Executable
add_executable(filecopy ${SOURCES})

# Include directories
target_include_directories(filecopy PRIVATE src)

# Platform-specific settings
if(WIN32)
    # Windows
    target_compile_definitions(filecopy PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(filecopy PRIVATE ws2_32)
else()
    # Linux/Unix
    target_compile_definitions(filecopy PRIVATE _GNU_SOURCE)
    target_link_libraries(filecopy PRIVATE pthread)
    
    if(USE_IO_URING)
        target_link_libraries(filecopy PRIVATE uring)
    endif()
endif()

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(filecopy PRIVATE -Wall -Wextra -O2)
endif()

