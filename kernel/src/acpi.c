#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

#include "acpi.h"
#include "serial.h"
#include "mm.h"

/* ------------------------------------------------------------------ */
/* NOTE on assumptions — adjust these two things to match your tree:
 *
 * 1. This assumes mm.h exposes:
 *        void *phys_to_virt(uint64_t phys);
 *    (matching the phys_to_virt/virt_to_phys helpers mentioned in your
 *    kmain.c comment). If your actual signature/name differs, just
 *    rename the calls below.
 *
 * 2. Limine's RSDP response address: depending on the base revision your
 *    limine.h models, this is sometimes already an HHDM-mapped virtual
 *    pointer, and sometimes a raw physical address you must convert
 *    yourself. This file treats it as physical and runs it through
 *    phys_to_virt() to be safe — if your limine.h already gives you a
 *    usable virtual pointer, just drop the phys_to_virt() call around
 *    rsdp_request.response->address below.
 * ------------------------------------------------------------------ */

extern volatile struct limine_rsdp_request rsdp_request;

/* ---- port I/O ---- */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" ::"a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" ::"a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port)
{
    uint16_t r;
    __asm__ volatile("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* ---- ACPI structures ---- */
struct RSDPDescriptor
{
    char signature[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_address;
} __attribute__((packed));

struct ACPISDTHeader
{
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

#define FADT_SMI_CMD 48
#define FADT_ACPI_ENABLE 52
#define FADT_PM1a_CNT_BLK 64
#define FADT_PM1b_CNT_BLK 68
#define FADT_DSDT 40

static uint32_t read_u32(void *base, int offset)
{
    return *(uint32_t *)((uint8_t *)base + offset);
}
static uint8_t read_u8(void *base, int offset)
{
    return *(uint8_t *)((uint8_t *)base + offset);
}

static struct ACPISDTHeader *find_table(struct ACPISDTHeader *rsdt, const char *sig)
{
    int entries = (rsdt->length - sizeof(struct ACPISDTHeader)) / 4;
    uint32_t *ptrs = (uint32_t *)((uint8_t *)rsdt + sizeof(struct ACPISDTHeader));
    for (int i = 0; i < entries; i++)
    {
        struct ACPISDTHeader *h = (struct ACPISDTHeader *)phys_to_virt(ptrs[i]);
        if (h->signature[0] == sig[0] && h->signature[1] == sig[1] &&
            h->signature[2] == sig[2] && h->signature[3] == sig[3])
        {
            return h;
        }
    }
    return NULL;
}

/* Minimal-parse trick: scan DSDT AML bytes for the "_S5_" package and pull
 * out SLP_TYPa/SLP_TYPb without a full AML interpreter. Works on the large
 * majority of real machines and VMs. */
static bool find_s5_values(struct ACPISDTHeader *dsdt, uint8_t *slp_typ_a, uint8_t *slp_typ_b)
{
    uint8_t *p = (uint8_t *)dsdt + sizeof(struct ACPISDTHeader);
    uint8_t *end = (uint8_t *)dsdt + dsdt->length;

    for (; p < end - 5; p++)
    {
        if (p[0] == '_' && p[1] == 'S' && p[2] == '5' && p[3] == '_')
        {
            p += 4;
            if (*p != 0x12)
                continue; /* expect PackageOp */
            p++;
            p += ((*p & 0xC0) ? 2 : 1); /* skip package length encoding */
            p++;                        /* skip element count byte */

            if (*p == 0x0A)
                p++; /* BytePrefix */
            *slp_typ_a = *p;
            p++;

            if (*p == 0x0A)
                p++; /* BytePrefix */
            *slp_typ_b = *p;
            return true;
        }
    }
    return false;
}

static uint32_t g_pm1a_cnt = 0;
static uint32_t g_pm1b_cnt = 0;
static uint32_t g_smi_cmd = 0;
static uint8_t g_acpi_enable = 0;
static uint8_t g_slp_typ_a = 0;
static uint8_t g_slp_typ_b = 0;
static bool g_acpi_ready = false;

bool acpi_init(void)
{
    if (rsdp_request.response == NULL)
    {
        serial_print("acpi: no RSDP from Limine\n");
        return false;
    }

    struct RSDPDescriptor *rsdp =
        (struct RSDPDescriptor *)rsdp_request.response->address;

    struct ACPISDTHeader *rsdt = (struct ACPISDTHeader *)phys_to_virt(rsdp->rsdt_address);

    struct ACPISDTHeader *fadt = find_table(rsdt, "FACP");
    if (!fadt)
    {
        serial_print("acpi: FADT not found\n");
        return false;
    }

    g_smi_cmd = read_u32(fadt, FADT_SMI_CMD);
    g_acpi_enable = read_u8(fadt, FADT_ACPI_ENABLE);
    g_pm1a_cnt = read_u32(fadt, FADT_PM1a_CNT_BLK);
    g_pm1b_cnt = read_u32(fadt, FADT_PM1b_CNT_BLK);
    uint32_t dsdt_phys = read_u32(fadt, FADT_DSDT);

    struct ACPISDTHeader *dsdt = (struct ACPISDTHeader *)phys_to_virt(dsdt_phys);

    if (!find_s5_values(dsdt, &g_slp_typ_a, &g_slp_typ_b))
    {
        serial_print("acpi: _S5 not found in DSDT\n");
        return false;
    }

    g_acpi_ready = true;
    serial_print("acpi: ready (S5 values found)\n");
    return true;
}

void acpi_poweroff(void)
{
    if (!g_acpi_ready)
    {
        serial_print("acpi: not initialized, cannot power off\n");
        return;
    }

    if (!(inw((uint16_t)g_pm1a_cnt) & 1) && g_smi_cmd && g_acpi_enable)
    {
        outb((uint16_t)g_smi_cmd, g_acpi_enable);
        while (!(inw((uint16_t)g_pm1a_cnt) & 1))
        { /* wait for SCI_EN */
        }
    }

    uint16_t slp_en = 1 << 13;
    outw((uint16_t)g_pm1a_cnt, (uint16_t)((g_slp_typ_a << 10) | slp_en));
    if (g_pm1b_cnt)
    {
        outw((uint16_t)g_pm1b_cnt, (uint16_t)((g_slp_typ_b << 10) | slp_en));
    }

    /* fallback for VMs if the above didn't take effect */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    serial_print("acpi: poweroff write sent but system still running — ACPI method failed on this machine\n");
}