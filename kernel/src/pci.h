#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} pci_device_t;

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);
uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);

// Scans every bus/device/function for a device whose class/subclass/prog-if
// registers match. xHCI is class 0x0C (Serial Bus Controller), subclass
// 0x03 (USB), prog-if 0x30 (xHCI). Returns 1 and fills *out on success.
int pci_find_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out);

// Reads BAR `bar_index` (0-5), masks off the flag bits, and if it's a
// 64-bit memory BAR, folds in the upper 32 bits from the next BAR slot.
// Returns 0 if the BAR is an I/O-space BAR (not usable here).
uint64_t pci_read_bar(pci_device_t dev, int bar_index);

// Determines the size (in bytes) of a memory BAR using the standard
// PCI "write all-1s, read back, restore" technique. Needed before
// mapping a BAR into the page tables (mm_map_mmio) so the whole
// register window actually gets mapped, not just an assumed size.
// Returns 0 for an I/O-space BAR.
uint64_t pci_bar_size(pci_device_t dev, int bar_index);

// Sets Memory Space Enable + Bus Master Enable in the command register.
// Without this, the controller can't respond to MMIO reads/writes or
// perform DMA -- easy to forget and then wonder why every register reads
// back as all-1s.
void pci_enable_device(pci_device_t dev);

#endif