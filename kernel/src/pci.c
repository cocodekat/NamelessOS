#include "pci.h"
#include "serial.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline void outl(uint16_t port, uint32_t val)
{
    asm volatile("outl %0, %1" ::"a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t val)
{
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
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
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, offset));
    return inw((uint16_t)(PCI_CONFIG_DATA + (offset & 2u)));
}

void pci_config_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, offset));
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2u)), value);
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

int pci_enumerate(pci_visit_fn callback, void *context)
{
    if (callback == 0)
        return 0;

    for (uint32_t bus = 0; bus < 256; bus++)
    {
        for (uint32_t device = 0; device < 32; device++)
        {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)device, 0, 0x00);
            if ((id0 & 0xFFFFu) == 0xFFFFu)
                continue;
            uint8_t header = (uint8_t)(pci_config_read32((uint8_t)bus,
                (uint8_t)device, 0, 0x0C) >> 16);
            int functions = (header & 0x80u) ? 8 : 1;
            for (int function = 0; function < functions; function++)
            {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)device,
                                                (uint8_t)function, 0x00);
                uint16_t vendor = (uint16_t)id;
                if (vendor == 0xFFFFu)
                    continue;
                pci_device_t dev = {(uint8_t)bus, (uint8_t)device,
                                    (uint8_t)function};
                if (callback(dev, vendor, (uint16_t)(id >> 16), context))
                    return 1;
            }
        }
    }
    return 0;
}

uint8_t pci_find_capability(pci_device_t dev, uint8_t capability_id)
{
    uint16_t status = pci_config_read16(dev.bus, dev.device, dev.function, 0x06);
    if (!(status & (1u << 4)))
        return 0;

    uint8_t pointer = (uint8_t)pci_config_read32(dev.bus, dev.device,
                                                 dev.function, 0x34);
    for (unsigned visited = 0; pointer >= 0x40 && visited < 48; visited++)
    {
        pointer &= 0xfcu;
        uint32_t header = pci_config_read32(dev.bus, dev.device,
                                            dev.function, pointer);
        if ((uint8_t)header == capability_id)
            return pointer;
        uint8_t next = (uint8_t)(header >> 8);
        if (next == pointer)
            break;
        pointer = next;
    }
    return 0;
}

int pci_set_power_d0(pci_device_t dev)
{
    uint8_t pm = pci_find_capability(dev, 0x01);
    if (!pm)
        return 0;
    uint16_t pmcsr = pci_config_read16(dev.bus, dev.device, dev.function,
                                      (uint8_t)(pm + 4));
    int old_state = pmcsr & 3u;
    if (old_state != 0)
        pci_config_write16(dev.bus, dev.device, dev.function,
                           (uint8_t)(pm + 4), (uint16_t)(pmcsr & ~3u));
    return old_state;
}

void pci_disable_link_power_management(pci_device_t dev)
{
    uint8_t pcie = pci_find_capability(dev, 0x10);
    if (!pcie)
        return;
    uint8_t link_control_offset = (uint8_t)(pcie + 0x10);
    uint16_t control = pci_config_read16(dev.bus, dev.device, dev.function,
                                         link_control_offset);
    /* Disable ASPM L0s/L1 and Clock Power Management while myOS has no
       coordinated PCIe power-management subsystem. */
    control &= (uint16_t)~0x0103u;
    pci_config_write16(dev.bus, dev.device, dev.function,
                       link_control_offset, control);
}

void pci_enable_bridge_path(pci_device_t endpoint)
{
    for (uint32_t bus = 0; bus < 256; bus++)
    {
        for (uint32_t device = 0; device < 32; device++)
        {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)device, 0, 0);
            if ((uint16_t)id0 == 0xffffu)
                continue;
            uint8_t header = (uint8_t)(pci_config_read32((uint8_t)bus,
                (uint8_t)device, 0, 0x0c) >> 16);
            int functions = (header & 0x80u) ? 8 : 1;
            for (int function = 0; function < functions; function++)
            {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)device,
                                                (uint8_t)function, 0);
                if ((uint16_t)id == 0xffffu)
                    continue;
                uint32_t class_reg = pci_config_read32((uint8_t)bus,
                    (uint8_t)device, (uint8_t)function, 0x08);
                if ((class_reg >> 16) != 0x0604u)
                    continue;
                uint32_t buses = pci_config_read32((uint8_t)bus,
                    (uint8_t)device, (uint8_t)function, 0x18);
                uint8_t secondary = (uint8_t)(buses >> 8);
                uint8_t subordinate = (uint8_t)(buses >> 16);
                if (endpoint.bus < secondary || endpoint.bus > subordinate)
                    continue;

                pci_device_t bridge = {(uint8_t)bus, (uint8_t)device,
                                       (uint8_t)function};
                int power = pci_set_power_d0(bridge);
                pci_disable_link_power_management(bridge);
                uint16_t old_command = pci_config_read16(bridge.bus,
                    bridge.device, bridge.function, 0x04);
                /* Bridges in the endpoint's hierarchy must forward I/O and
                   memory traffic and be allowed to initiate transactions. */
                uint16_t command = old_command | 0x0007u;
                pci_config_write16(bridge.bus, bridge.device, bridge.function,
                                   0x04, command);
                serial_print("[pci] bridge "); serial_print_hex32(bridge.bus);
                serial_putc(':'); serial_print_hex32(bridge.device);
                serial_putc('.'); serial_print_hex32(bridge.function);
                serial_print(" buses="); serial_print_hex32(secondary);
                serial_putc('-'); serial_print_hex32(subordinate);
                serial_print(" command="); serial_print_hex32(old_command);
                serial_print("->"); serial_print_hex32(command);
                serial_print(" power-was-D"); serial_print_uint((unsigned)power);
                serial_print("\n");
            }
        }
    }
}

int pci_get_bar(pci_device_t dev, int bar_index, pci_bar_t *out)
{
    if (out == 0 || bar_index < 0 || bar_index > 5)
        return 0;
    *out = (pci_bar_t){0};

    uint8_t offset = (uint8_t)(0x10 + bar_index * 4);
    uint32_t low = pci_config_read32(dev.bus, dev.device, dev.function, offset);
    if (low == 0 || low == 0xFFFFFFFFu)
        return 0;

    // Disable address decoding while probing so all-ones is never decoded.
    uint16_t command = pci_config_read16(dev.bus, dev.device, dev.function, 0x04);
    pci_config_write16(dev.bus, dev.device, dev.function, 0x04,
                       (uint16_t)(command & ~0x3u));

    if (low & 1u)
    {
        pci_config_write32(dev.bus, dev.device, dev.function, offset, 0xFFFFFFFFu);
        uint32_t mask = pci_config_read32(dev.bus, dev.device, dev.function, offset);
        pci_config_write32(dev.bus, dev.device, dev.function, offset, low);
        pci_config_write16(dev.bus, dev.device, dev.function, 0x04, command);
        mask &= ~0x3u;
        out->kind = PCI_BAR_IO;
        out->base = low & ~0x3u;
        out->size = mask ? (uint64_t)(~mask + 1u) : 0;
        return out->base != 0;
    }

    uint8_t type = (uint8_t)((low >> 1) & 3u);
    if (type == 1u || (type == 2u && bar_index == 5))
    {
        pci_config_write16(dev.bus, dev.device, dev.function, 0x04, command);
        return 0;
    }
    uint32_t high = type == 2u
        ? pci_config_read32(dev.bus, dev.device, dev.function, (uint8_t)(offset + 4)) : 0;

    pci_config_write32(dev.bus, dev.device, dev.function, offset, 0xFFFFFFFFu);
    if (type == 2u)
        pci_config_write32(dev.bus, dev.device, dev.function, (uint8_t)(offset + 4), 0xFFFFFFFFu);
    uint32_t mask_low = pci_config_read32(dev.bus, dev.device, dev.function, offset);
    uint32_t mask_high = type == 2u
        ? pci_config_read32(dev.bus, dev.device, dev.function, (uint8_t)(offset + 4)) : 0;
    pci_config_write32(dev.bus, dev.device, dev.function, offset, low);
    if (type == 2u)
        pci_config_write32(dev.bus, dev.device, dev.function, (uint8_t)(offset + 4), high);
    pci_config_write16(dev.bus, dev.device, dev.function, 0x04, command);

    uint64_t mask = (uint64_t)(mask_low & ~0xFu);
    uint64_t base = (uint64_t)(low & ~0xFu);
    if (type == 2u)
    {
        mask |= (uint64_t)mask_high << 32;
        base |= (uint64_t)high << 32;
    }
    out->kind = PCI_BAR_MMIO;
    out->base = base;
    if (type == 2u)
        out->size = mask ? (~mask + 1u) : 0;
    else
        out->size = mask_low ? (uint64_t)(~(mask_low & ~0xFu) + 1u) : 0;
    out->is_64bit = type == 2u;
    out->prefetchable = (low & 8u) != 0;
    return out->base != 0;
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
    pci_enable_bus_master(dev, 0, 1);
}

void pci_enable_bus_master(pci_device_t dev, int enable_io, int enable_memory)
{
    uint16_t cmd = pci_config_read16(dev.bus, dev.device, dev.function, 0x04);
    if (enable_io)
        cmd |= 1u << 0;
    if (enable_memory)
        cmd |= 1u << 1;
    cmd |= 1u << 2;
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
