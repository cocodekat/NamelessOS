#ifndef MM_H
#define MM_H

#include <stdint.h>
#include <stddef.h>

// Must be called once at boot, before anything touches phys_to_virt/virt_to_phys
// (pci/xhci init included).
void mm_init(void);

// Physical -> virtual. Use this to get a dereferenceable pointer to a
// physical address you didn't get handed by Limine already (e.g. a PCI
// BAR's MMIO physical address). Relies on the HHDM (Higher Half Direct
// Map) that Limine sets up, which covers all physical memory Limine
// knows about -- but NOT arbitrary MMIO windows (PCI BARs, etc). For
// those, call mm_map_mmio() first so the page tables actually back the
// address this function computes; otherwise the first access will page
// fault (and, with no IDT installed, triple-fault the machine).
void *phys_to_virt(uint64_t phys);

// Virtual -> physical. Use this ONLY on pointers that live inside your own
// kernel image/BSS (globals, static arrays, anything from kalloc() since
// heap_arena is a static BSS array) -- NOT on arbitrary HHDM pointers.
// This is what you need to hand a physical address to a DMA-capable
// device (xHCI rings/contexts) for a buffer your kernel owns.
uint64_t virt_to_phys(const void *virt);

// Maps `size` bytes starting at physical address `phys` into the page
// tables (rounded out to whole pages), uncacheable, so it's safe to
// dereference via phys_to_virt() afterward. Needed for any physical
// address that ISN'T guaranteed-present RAM Limine already mapped into
// the HHDM -- most importantly, PCI BAR / MMIO windows. Without this,
// touching a device's MMIO registers via phys_to_virt() alone can hit
// an unmapped page and fault (real hardware routinely places BARs in
// PCI-hole address ranges the HHDM doesn't cover, even though QEMU's
// default layout sometimes happens not to trigger it).
//
// Returns the same pointer phys_to_virt(phys) would -- callers can just
// use the return value directly, or keep calling phys_to_virt() on
// addresses within the mapped range afterward.
void *mm_map_mmio(uint64_t phys, uint64_t size);

#endif