#include "heap.h"

// 16 MB static arena, reserved in the kernel's BSS section at compile time.
#define HEAP_SIZE (16 * 1024 * 1024)
static unsigned char heap_arena[HEAP_SIZE];
static size_t heap_offset = 0;

void *kalloc(size_t size)
{
    // Align every allocation to 8 bytes, since misaligned access can be
    // slow or even fault on some data types/architectures.
    size_t aligned_size = (size + 7) & ~((size_t)7);

    if (heap_offset + aligned_size > HEAP_SIZE)
    {
        return NULL; // out of memory
    }

    void *ptr = &heap_arena[heap_offset];
    heap_offset += aligned_size;
    return ptr;
}