#include <stdint.h>
#include <stdio.h>

#include "heap.h"

int main(void)
{
    void *small = kalloc(3u);
    void *page = kalloc_aligned(4096u, 4096u);
    void *cacheline = kalloc_aligned(64u, 64u);
    if (!small || !page || !cacheline ||
        ((uintptr_t)page & 4095u) != 0u ||
        ((uintptr_t)cacheline & 63u) != 0u) {
        fprintf(stderr, "aligned heap allocation failed\n");
        return 1;
    }
    if (kalloc_aligned(1u, 3u) != NULL ||
        kalloc_aligned(32u * 1024u * 1024u, 4096u) != NULL) {
        fprintf(stderr, "invalid allocation was accepted\n");
        return 1;
    }
    puts("heap alignment test passed");
    return 0;
}
