#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include "iommu.h"
#include "mm.h"
#include "serial.h"

__attribute__((used, section(".limine_requests"))) volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0};

typedef struct
{
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_t;

typedef struct
{
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct
{
    uint16_t type;
    uint16_t length;
} __attribute__((packed)) dmar_entry_t;

typedef struct
{
    dmar_entry_t header;
    uint8_t flags;
    uint8_t reserved;
    uint16_t segment;
    uint64_t register_base;
} __attribute__((packed)) dmar_drhd_t;

#define VTD_GCMD 0x18u
#define VTD_GSTS 0x1cu
#define VTD_PMEN 0x64u
#define VTD_GSTS_TES (1u << 31)
#define VTD_GSTS_QIES (1u << 26)
#define VTD_GSTS_IRES (1u << 25)
#define VTD_PMEN_EPM (1u << 31)
#define VTD_PMEN_PRS (1u << 0)

static int signature_is(const char actual[4], const char expected[4])
{
    return actual[0] == expected[0] && actual[1] == expected[1] &&
           actual[2] == expected[2] && actual[3] == expected[3];
}

static int checksum_ok(const void *address, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)address;
    uint8_t sum = 0;
    if (length == 0 || length > 1024u * 1024u)
        return 0;
    for (uint32_t i = 0; i < length; i++)
        sum = (uint8_t)(sum + bytes[i]);
    return sum == 0;
}

static acpi_sdt_t *find_dmar(acpi_rsdp_t *rsdp)
{
    uint64_t root_phys;
    unsigned entry_size;
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0)
    {
        root_phys = rsdp->xsdt_address;
        entry_size = 8;
    }
    else
    {
        root_phys = rsdp->rsdt_address;
        entry_size = 4;
    }
    if (root_phys == 0)
        return NULL;
    acpi_sdt_t *root = (acpi_sdt_t *)phys_to_virt(root_phys);
    if (root->length < sizeof(*root) || !checksum_ok(root, root->length))
        return NULL;
    uint32_t count = (root->length - sizeof(*root)) / entry_size;
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    for (uint32_t i = 0; i < count; i++)
    {
        uint64_t table_phys = entry_size == 8
                                  ? ((const uint64_t *)entries)[i]
                                  : ((const uint32_t *)entries)[i];
        acpi_sdt_t *table = (acpi_sdt_t *)phys_to_virt(table_phys);
        if (signature_is(table->signature, "DMAR") &&
            table->length >= sizeof(*table) + 12 && checksum_ok(table, table->length))
            return table;
    }
    return NULL;
}

static volatile uint8_t *map_registers(uint64_t phys)
{
    void *candidate = phys_to_virt(phys);
    if (mm_virt_to_phys_page_table(candidate) == phys)
        return candidate;
    return (volatile uint8_t *)mm_map_mmio(phys, 0x1000);
}

static void disable_drhd(const dmar_drhd_t *drhd)
{
    volatile uint8_t *regs = map_registers(drhd->register_base);
    volatile uint32_t *gcmd = (volatile uint32_t *)(regs + VTD_GCMD);
    volatile uint32_t *gsts = (volatile uint32_t *)(regs + VTD_GSTS);
    volatile uint32_t *pmen = (volatile uint32_t *)(regs + VTD_PMEN);
    uint32_t before_gsts = *gsts;
    uint32_t before_pmen = *pmen;

    serial_print("[iommu] VT-d segment=");
    serial_print_uint(drhd->segment);
    serial_print(" base=");
    serial_print_hex64(drhd->register_base);
    serial_print(" gsts=");
    serial_print_hex32(before_gsts);
    serial_print(" pmen=");
    serial_print_hex32(before_pmen);
    serial_print("\n");

    /* myOS currently uses physical DMA addresses and owns no remapping
       tables. Disable translation, queued invalidation and interrupt
       remapping left active by firmware. */
    *gcmd = 0;
    (void)*gsts;
    for (uint32_t i = 0; i < 1000000u; i++)
    {
        if ((*gsts & (VTD_GSTS_TES | VTD_GSTS_QIES | VTD_GSTS_IRES)) == 0)
            break;
        asm volatile("pause");
    }

    /* Firmware DMA protection may use VT-d protected-memory regions even
       when normal address translation is off. Those also reject NIC DMA. */
    *pmen = before_pmen & ~VTD_PMEN_EPM;
    (void)*pmen;
    for (uint32_t i = 0; i < 1000000u; i++)
    {
        if ((*pmen & VTD_PMEN_PRS) == 0)
            break;
        asm volatile("pause");
    }

    serial_print("[iommu] after gsts=");
    serial_print_hex32(*gsts);
    serial_print(" pmen=");
    serial_print_hex32(*pmen);
    serial_print("\n");
}

void iommu_disable_firmware_dma_protection(void)
{
    if (rsdp_request.response == NULL || rsdp_request.response->address == NULL)
    {
        serial_print("[iommu] no ACPI RSDP; cannot inspect DMA remapping\n");
        return;
    }
    acpi_rsdp_t *rsdp = (acpi_rsdp_t *)rsdp_request.response->address;
    uint32_t rsdp_len = rsdp->revision >= 2 ? rsdp->length : 20u;
    if (rsdp_len < 20u || !checksum_ok(rsdp, rsdp_len))
    {
        serial_print("[iommu] invalid ACPI RSDP\n");
        return;
    }
    acpi_sdt_t *dmar = find_dmar(rsdp);
    if (dmar == NULL)
    {
        serial_print("[iommu] no DMAR table; VT-d not advertised\n");
        return;
    }

    unsigned units = 0;
    uint8_t *cursor = (uint8_t *)dmar + sizeof(*dmar) + 12u;
    uint8_t *end = (uint8_t *)dmar + dmar->length;
    while (cursor + sizeof(dmar_entry_t) <= end)
    {
        dmar_entry_t *entry = (dmar_entry_t *)cursor;
        if (entry->length < sizeof(*entry) || cursor + entry->length > end)
            break;
        if (entry->type == 0 && entry->length >= sizeof(dmar_drhd_t))
        {
            disable_drhd((const dmar_drhd_t *)entry);
            units++;
        }
        cursor += entry->length;
    }
    serial_print("[iommu] DMA-remapping units handled=");
    serial_print_uint(units);
    serial_print("\n");
}
