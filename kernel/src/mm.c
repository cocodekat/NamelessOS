#include <limine.h>
#include "mm.h"

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request kaddr_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0
};

static uint64_t hhdm_offset      = 0;
static uint64_t kernel_phys_base = 0;
static uint64_t kernel_virt_base = 0;

void mm_init(void) {
    if (hhdm_request.response != NULL) {
        hhdm_offset = hhdm_request.response->offset;
    }
    if (kaddr_request.response != NULL) {
        kernel_phys_base = kaddr_request.response->physical_base;
        kernel_virt_base = kaddr_request.response->virtual_base;
    }
}

void *phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)(phys + hhdm_offset);
}

uint64_t virt_to_phys(const void *virt) {
    return (uint64_t)(uintptr_t)virt - kernel_virt_base + kernel_phys_base;
}

// ---------------------------------------------------------------------
// On-demand page mapping, for physical addresses the bootloader's HHDM
// doesn't already cover (MMIO windows, mainly). We never unmap/free
// these -- everything we map here needs to live for the rest of boot.
// ---------------------------------------------------------------------

#define PTE_PRESENT  (1ull << 0)
#define PTE_WRITABLE (1ull << 1)
#define PTE_PCD      (1ull << 4)  // cache disable -- required for MMIO correctness
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

// Small fixed pool of pages to use as new page-table levels (PDPT/PD/PT)
// when the existing tables don't reach far enough. Kept as a static
// array rather than going through kalloc() so this code has no
// dependency on the heap being initialized yet -- mm_init() may need to
// run very early.
#define PT_POOL_PAGES 32
static uint8_t pt_pool[PT_POOL_PAGES][4096] __attribute__((aligned(4096)));
static int pt_pool_used = 0;

static void *alloc_table_page(void) {
    if (pt_pool_used >= PT_POOL_PAGES) return NULL; // out of emergency page-table pages
    uint8_t *page = pt_pool[pt_pool_used++];
    for (int i = 0; i < 4096; i++) page[i] = 0;
    return page;
}

static inline uint64_t read_cr3(void) {
    uint64_t v;
    asm volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void invlpg(uint64_t addr) {
    asm volatile ("invlpg (%0)" :: "r"(addr) : "memory");
}

// Walks (creating any missing levels along the way) the 4-level page
// table rooted at CR3 and maps one 4KiB page: virt -> phys, with the
// given extra PTE flags (PRESENT is always added).
static void map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(read_cr3() & PTE_ADDR_MASK);

    uint64_t i4 = (virt >> 39) & 0x1FF;
    uint64_t i3 = (virt >> 30) & 0x1FF;
    uint64_t i2 = (virt >> 21) & 0x1FF;
    uint64_t i1 = (virt >> 12) & 0x1FF;

    if (!(pml4[i4] & PTE_PRESENT)) {
        void *page = alloc_table_page();
        pml4[i4] = virt_to_phys(page) | PTE_PRESENT | PTE_WRITABLE;
    }
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[i4] & PTE_ADDR_MASK);

    if (!(pdpt[i3] & PTE_PRESENT)) {
        void *page = alloc_table_page();
        pdpt[i3] = virt_to_phys(page) | PTE_PRESENT | PTE_WRITABLE;
    }
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[i3] & PTE_ADDR_MASK);

    if (!(pd[i2] & PTE_PRESENT)) {
        void *page = alloc_table_page();
        pd[i2] = virt_to_phys(page) | PTE_PRESENT | PTE_WRITABLE;
    }
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[i2] & PTE_ADDR_MASK);

    pt[i1] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    invlpg(virt);
}

void *mm_map_mmio(uint64_t phys, uint64_t size) {
    uint64_t start = phys & ~0xFFFull;
    uint64_t end   = (phys + size + 0xFFF) & ~0xFFFull;

    for (uint64_t p = start; p < end; p += 0x1000) {
        uint64_t virt = (uint64_t)(uintptr_t)phys_to_virt(p);
        map_page(virt, p, PTE_WRITABLE | PTE_PCD);
    }

    return phys_to_virt(phys);
}