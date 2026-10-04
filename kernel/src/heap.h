#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

void *kalloc(size_t size);
void *kalloc_aligned(size_t size, size_t alignment);

#endif
