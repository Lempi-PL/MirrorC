#ifndef TRAVERSE_H
#define TRAVERSE_H

#include "common.h"


#ifdef _WIN32
unsigned __stdcall traverse_worker(void* arg);
#else
void* traverse_worker(void* arg);
#endif

#endif // TRAVERSE_H

