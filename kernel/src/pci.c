#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline void outl(uint16_t port, uint32_t val)
{
    asm volatile("outl %0, %1" ::"a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint32_t pci_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)device << 11) | ((uint32_t)function << 8) | (offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t dword = pci_config_read32(bus, device, function, (uint8_t)(offset & 0xFC));
    return (uint16_t)(dword >> ((offset & 2) * 8));
}

void pci_config_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value)
{
    uint32_t dword = pci_config_read32(bus, device, function, (uint8_t)(offset & 0xFC));
    uint32_t shift = (offset & 2) * 8;
    dword = (dword & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    pci_config_write32(bus, device, function, (uint8_t)(offset & 0xFC), dword);
}

int pci_find_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out)
{
    for (uint32_t bus = 0; bus < 256; bus++)
    {
        for (uint32_t device = 0; device < 32; device++)
        {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)device, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF)
                continue; // vendor ID 0xFFFF = no device here

            uint8_t header_type = (uint8_t)(pci_config_read32((uint8_t)bus, (uint8_t)device, 0, 0x0C) >> 16);
            int max_function = (header_type & 0x80) ? 8 : 1; // bit7 set = multi-function device

            for (int function = 0; function < max_function; function++)
            {
                uint32_t fid = pci_config_read32((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x00);
                if ((fid & 0xFFFF) == 0xFFFF)
                    continue;

                uint32_t classreg = pci_config_read32((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x08);
                uint8_t f_prog_if = (uint8_t)(classreg >> 8);
                uint8_t f_subclass = (uint8_t)(classreg >> 16);
                uint8_t f_class = (uint8_t)(classreg >> 24);

                if (f_class == class_code && f_subclass == subclass && f_prog_if == prog_if)
                {
                    out->bus = (uint8_t)bus;
                    out->device = (uint8_t)device;
                    out->function = (uint8_t)function;
                    return 1;
                }
            }
        }
    }
    return 0;
}

uint64_t pci_read_bar(pci_device_t dev, int bar_index)
{
    uint8_t offset = (uint8_t)(0x10 + bar_index * 4);
    uint32_t low = pci_config_read32(dev.bus, dev.device, dev.function, offset);

    if (low & 0x1)
        return 0; // I/O-space BAR, not memory-mapped -- caller shouldn't ask for this one

    uint64_t addr = low & ~0xFull;
    uint8_t bar_type = (uint8_t)((low >> 1) & 0x3);
    if (bar_type == 0x2)
    {
        // 64-bit BAR: the next 32-bit slot holds the high half.
        uint32_t high = pci_config_read32(dev.bus, dev.device, dev.function, (uint8_t)(offset + 4));
        addr |= ((uint64_t)high << 32);
    }
    return addr;
}

void pci_enable_device(pci_device_t dev)
{
    uint16_t cmd = pci_config_read16(dev.bus, dev.device, dev.function, 0x04);
    cmd |= (1u << 1) | (1u << 2); // Memory Space Enable, Bus Master Enable
    pci_config_write16(dev.bus, dev.device, dev.function, 0x04, cmd);
}

uint64_t pci_bar_size(pci_device_t dev, int bar_index)
{
    uint8_t offset = (uint8_t)(0x10 + bar_index * 4);

    uint32_t orig_low = pci_config_read32(dev.bus, dev.device, dev.function, offset);
    if (orig_low & 0x1)
        return 0; // I/O-space BAR, not handled here

    uint8_t bar_type = (uint8_t)((orig_low >> 1) & 0x3);
    uint32_t orig_high = 0;
    if (bar_type == 0x2)
    {
        orig_high = pci_config_read32(dev.bus, dev.device, dev.function, (uint8_t)(offset + 4));
    }

    // Probe: write all 1s, see which bits the hardware actually lets us
    // set (those are the size bits), then restore the original value so
    // we don't leave the device's address decode scrambled.
    pci_config_write32(dev.bus, dev.device, dev.function, offset, 0xFFFFFFFFu);
    uint32_t size_low = pci_config_read32(dev.bus, dev.device, dev.function, offset);
    pci_config_write32(dev.bus, dev.device, dev.function, offset, orig_low);

    uint64_t size_mask = size_low & ~0xFu;

    if (bar_type == 0x2)
    {
        pci_config_write32(dev.bus, dev.device, dev.function, (uint8_t)(offset + 4), 0xFFFFFFFFu);
        uint32_t size_high = pci_config_read32(dev.bus, dev.device, dev.function, (uint8_t)(offset + 4));
        pci_config_write32(dev.bus, dev.device, dev.function, (uint8_t)(offset + 4), orig_high);
        size_mask |= ((uint64_t)size_high << 32);
    }

    if (size_mask == 0)
        return 0;
    return (~size_mask) + 1;
}