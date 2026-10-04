#include "iwlwifi.h"
#include "iwl_firmware.h"
#include "iwl_transport.h"
#include "../../pci.h"
#include "../../mm.h"
#include "../../delay.h"
#include "../../heap.h"
#include "../../memory.h"
#include "../../serial.h"
#include "../../net/wifi.h"

#define INTEL_VENDOR_ID 0x8086u

/* Common CSR window shared by Intel PCIe/CNVio wireless transports. Only
   read-only identification is used in this first bring-up stage. */
#define CSR_HW_IF_CONFIG_REG 0x000u
#define CSR_INT              0x008u
#define CSR_INT_MASK         0x00cu
#define CSR_FH_INT_STATUS    0x010u
#define CSR_RESET            0x020u
#define CSR_GP_CNTRL         0x024u
#define CSR_HW_REV           0x028u
#define CSR_MBOX_SET         0x088u
#define CSR_HW_RF_ID         0x09cu
#define CSR_MAC_OTP0         0x380u
#define CSR_MAC_OTP1         0x384u
#define CSR_MAC_STRAP0       0x388u
#define CSR_MAC_STRAP1       0x38cu

#define CSR_HW_IF_NIC_READY  0x00400000u
#define CSR_MBOX_OS_ALIVE    0x00000020u
#define CPU_SEPARATOR        0xffffccccu
#define PAGING_SEPARATOR     0xaaaabbbbu
#define IWL_DMA_SECTION_MAX  32768u

typedef struct {
    uint16_t device_id;
    uint16_t subsystem_id;
    const char *name;
    const char *firmware;
    uint8_t integrated;
    uint8_t generation;
    uint8_t firmware_api;
} iwl_config_t;

/* Keep PCI identity selection data-driven. More 22000-family variants can be
   added after their RF identity and firmware tuple has been verified. */
static const iwl_config_t supported[] = {
    {0xa0f0, 0x0074, "Intel Wi-Fi 6 AX201 (QuZ HR)",
     "iwlwifi-QuZ-a0-hr-b0-77.ucode", 1, 2, 77}
};

typedef struct {
    pci_device_t pci;
    pci_bar_t bar;
    const iwl_config_t *config;
    volatile uint8_t *csr;
    uint32_t hw_if_config;
    uint32_t hw_revision;
    uint32_t gp_control;
    uint32_t reset;
    uint32_t interrupt_status;
    uint32_t fh_interrupt_status;
    uint32_t rf_id;
    uint32_t mac_otp0, mac_otp1, mac_strap0, mac_strap1;
    iwl_fw_info_t firmware;
    size_t firmware_size;
    iwl_context_info_t *context;
    uint64_t context_phys;
    size_t dma_bytes;
    unsigned dma_lmac, dma_umac, dma_paging;
    uint64_t *rx_free_ring;
    uint32_t *rx_used_ring;
    iwl_rx_status_t *rx_status;
    iwl_tfh_tfd_t *command_ring;
    uint8_t *command_first_tb;
    uint32_t *invalid_command;
    uint64_t rx_free_phys, rx_used_phys, rx_status_phys;
    uint64_t command_phys, command_first_tb_phys, invalid_command_phys;
    void *rx_buffers[IWL_RX_BOOTSTRAP_BUFFERS];
    uint64_t rx_buffer_phys[IWL_RX_BOOTSTRAP_BUFFERS];
    size_t queue_dma_bytes;
    unsigned rx_buffers_prepared;
    uint8_t card_ready;
    uint8_t dma_ready;
    uint8_t queues_ready;
    wifi_device_t wifi;
} iwl_state_t;

static iwl_state_t iwl;

static uint32_t csr_read32(uint16_t offset)
{
    return *(volatile uint32_t *)(iwl.csr + offset);
}

static void csr_write32(uint16_t offset, uint32_t value)
{
    *(volatile uint32_t *)(iwl.csr + offset) = value;
    asm volatile("" ::: "memory");
}

static int dma_range_valid(uint64_t physical, size_t size, size_t alignment)
{
    if (physical == UINT64_MAX || !size || physical > UINT64_MAX - size)
        return 0;
    if (alignment && (physical & (alignment - 1u)))
        return 0;
    /* AX201 advertises a 36-bit DMA mask. Its transport also cannot tolerate
       a single allocation crossing a 4 GiB boundary. */
    if ((physical + size - 1u) >= (1ull << 36) ||
        (physical >> 32) != ((physical + size - 1u) >> 32))
        return 0;
    return 1;
}

static int dma_alloc_zero(size_t size, size_t alignment, void **cpu,
                          uint64_t *physical)
{
    void *allocation = kalloc_aligned(size, alignment);
    if (!allocation)
        return -12;
    memset(allocation, 0, size);
    uint64_t address = virt_to_phys(allocation);
    if (!dma_range_valid(address, size, alignment))
        return -13;
    *cpu = allocation;
    *physical = address;
    return 0;
}

static int prepare_card_hardware(void)
{
    csr_write32(CSR_HW_IF_CONFIG_REG,
                csr_read32(CSR_HW_IF_CONFIG_REG) | CSR_HW_IF_NIC_READY);
    for (unsigned elapsed = 0; elapsed < 50u; elapsed++) {
        if (csr_read32(CSR_HW_IF_CONFIG_REG) & CSR_HW_IF_NIC_READY) {
            csr_write32(CSR_MBOX_SET, CSR_MBOX_OS_ALIVE);
            iwl.card_ready = 1;
            return 0;
        }
        delay_ms(1);
    }
    return -2;
}

static int dma_copy_section(const iwl_fw_section_t *section, uint64_t *entry)
{
    if (!section->size || section->size > IWL_DMA_SECTION_MAX)
        return -5;
    void *copy = kalloc_aligned(section->size, 4096u);
    if (!copy)
        return -6;
    memcpy(copy, section->data, section->size);
    uint64_t physical = virt_to_phys(copy);
    if (!dma_range_valid(physical, section->size, 4096u))
        return -7;
    *entry = physical;
    iwl.dma_bytes += section->size;
    return 0;
}

static int build_firmware_dma_map(void)
{
    iwl.context = kalloc_aligned(sizeof(*iwl.context), 4096u);
    if (!iwl.context)
        return -3;
    memset(iwl.context, 0, sizeof(*iwl.context));
    iwl.context_phys = virt_to_phys(iwl.context);
    if (!dma_range_valid(iwl.context_phys, sizeof(*iwl.context), 4096u))
        return -4;

    unsigned phase = 0;
    for (size_t i = 0; i < iwl.firmware.runtime.section_count; i++) {
        const iwl_fw_section_t *section = &iwl.firmware.runtime.sections[i];
        if (section->offset == CPU_SEPARATOR) {
            if (phase != 0)
                return -8;
            phase = 1;
            continue;
        }
        if (section->offset == PAGING_SEPARATOR) {
            if (phase != 1)
                return -9;
            phase = 2;
            continue;
        }

        uint64_t *entry;
        unsigned *count;
        if (phase == 0) {
            entry = iwl.context->dram.lmac;
            count = &iwl.dma_lmac;
        } else if (phase == 1) {
            entry = iwl.context->dram.umac;
            count = &iwl.dma_umac;
        } else {
            entry = iwl.context->dram.paging;
            count = &iwl.dma_paging;
        }
        if (*count == 64u)
            return -10;
        int result = dma_copy_section(section, &entry[*count]);
        if (result)
            return result;
        (*count)++;
    }
    if (phase != 2 || !iwl.dma_lmac || !iwl.dma_umac || !iwl.dma_paging)
        return -11;

    iwl.context->version.mac_id = (uint16_t)iwl.hw_revision;
    iwl.context->version.size = (uint16_t)(sizeof(*iwl.context) / 4u);
    /* AX201/HR: 2048-entry RX cyclic buffer, long TFDs, 4 KiB buffers. */
    iwl.context->control.flags = iwl_context_control_flags();
    iwl.dma_ready = 1;
    return 0;
}

static int build_bootstrap_queues(void)
{
    int result;
    result = dma_alloc_zero(sizeof(*iwl.rx_free_ring) * IWL_RX_RING_ENTRIES,
                            4096u, (void **)&iwl.rx_free_ring,
                            &iwl.rx_free_phys);
    if (result) return result;
    result = dma_alloc_zero(sizeof(*iwl.rx_used_ring) * IWL_RX_RING_ENTRIES,
                            4096u, (void **)&iwl.rx_used_ring,
                            &iwl.rx_used_phys);
    if (result) return result;
    result = dma_alloc_zero(sizeof(*iwl.rx_status), 4096u,
                            (void **)&iwl.rx_status, &iwl.rx_status_phys);
    if (result) return result;
    result = dma_alloc_zero(sizeof(*iwl.command_ring) * IWL_CMD_RING_ENTRIES,
                            4096u, (void **)&iwl.command_ring,
                            &iwl.command_phys);
    if (result) return result;
    result = dma_alloc_zero(IWL_FIRST_TB_STRIDE * IWL_CMD_RING_ENTRIES,
                            4096u, (void **)&iwl.command_first_tb,
                            &iwl.command_first_tb_phys);
    if (result) return result;
    result = dma_alloc_zero(sizeof(*iwl.invalid_command), 4096u,
                            (void **)&iwl.invalid_command,
                            &iwl.invalid_command_phys);
    if (result) return result;

    /* Empty gen2 command TFDs point at a harmless shared invalid buffer.
       The queue write pointer stays zero, so none are submitted. */
    for (unsigned i = 0; i < IWL_CMD_RING_ENTRIES; i++) {
        iwl.command_ring[i].num_tbs = 1u;
        iwl.command_ring[i].tbs[0].length =
            (uint16_t)sizeof(*iwl.invalid_command);
        iwl.command_ring[i].tbs[0].address = iwl.invalid_command_phys;
    }

    /* Prepare a conservative initial pool. The producer doorbell will not be
       written until the firmware has raised its ALIVE interrupt and configured
       the receive flow handler. The low 12 bits carry the nonzero buffer ID. */
    for (unsigned i = 0; i < IWL_RX_BOOTSTRAP_BUFFERS; i++) {
        void *buffer;
        uint64_t physical;
        result = dma_alloc_zero(IWL_RX_BUFFER_SIZE, 4096u, &buffer, &physical);
        if (result) return result;
        iwl.rx_buffers[i] = buffer;
        iwl.rx_buffer_phys[i] = physical;
        iwl.rx_free_ring[i] = physical | (uint64_t)(i + 1u);
        iwl.rx_buffers_prepared++;
    }

    iwl.context->rbd.free_rbd = iwl.rx_free_phys;
    iwl.context->rbd.used_rbd = iwl.rx_used_phys;
    iwl.context->rbd.status_write = iwl.rx_status_phys;
    iwl.context->command.address = iwl.command_phys;
    iwl.context->command.size = iwl_command_ring_size_code();

    iwl.queue_dma_bytes =
        sizeof(*iwl.rx_free_ring) * IWL_RX_RING_ENTRIES +
        sizeof(*iwl.rx_used_ring) * IWL_RX_RING_ENTRIES +
        sizeof(*iwl.rx_status) +
        sizeof(*iwl.command_ring) * IWL_CMD_RING_ENTRIES +
        IWL_FIRST_TB_STRIDE * IWL_CMD_RING_ENTRIES +
        sizeof(*iwl.invalid_command) +
        IWL_RX_BOOTSTRAP_BUFFERS * IWL_RX_BUFFER_SIZE;
    iwl.queues_ready = 1;
    return 0;
}

static int match_device(pci_device_t dev, uint16_t vendor, uint16_t device,
                        void *context)
{
    (void)context;
    if (vendor != INTEL_VENDOR_ID)
        return 0;
    uint16_t subsystem = (uint16_t)(pci_config_read32(dev.bus, dev.device,
                                                      dev.function, 0x2c) >> 16);
    for (unsigned i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (supported[i].device_id == device &&
            (supported[i].subsystem_id == 0xffffu ||
             supported[i].subsystem_id == subsystem)) {
            iwl.pci = dev;
            iwl.config = &supported[i];
            return 1;
        }
    }
    return 0;
}

int iwlwifi_init(void)
{
    memset(&iwl, 0, sizeof(iwl));
    serial_print("[iwlwifi] searching for supported Intel 22000-family device\n");
    if (!pci_enumerate(match_device, 0)) {
        serial_print("[iwlwifi] no supported device found\n");
        return -1;
    }

    serial_print("[iwlwifi] PCI location "); serial_print_hex32(iwl.pci.bus);
    serial_putc(':'); serial_print_hex32(iwl.pci.device);
    serial_putc('.'); serial_print_hex32(iwl.pci.function); serial_print("\n");

    int previous_power = pci_set_power_d0(iwl.pci);
    if (previous_power) delay_ms(20);
    pci_disable_link_power_management(iwl.pci);
    pci_enable_bridge_path(iwl.pci);
    if (!pci_get_bar(iwl.pci, 0, &iwl.bar) ||
        iwl.bar.kind != PCI_BAR_MMIO || iwl.bar.size < 0x1000u) {
        serial_print("[iwlwifi] ERROR: BAR0 is not a usable MMIO window\n");
        return -2;
    }
    iwl.csr = (volatile uint8_t *)mm_map_mmio(iwl.bar.base, iwl.bar.size);
    pci_enable_bus_master(iwl.pci, 0, 1);
    pci_config_write16(iwl.pci.bus, iwl.pci.device, iwl.pci.function,
                       0x06, 0xffffu);

    iwl.hw_if_config = csr_read32(CSR_HW_IF_CONFIG_REG);
    iwl.hw_revision = csr_read32(CSR_HW_REV);
    iwl.gp_control = csr_read32(CSR_GP_CNTRL);
    iwl.reset = csr_read32(CSR_RESET);
    iwl.interrupt_status = csr_read32(CSR_INT);
    iwl.fh_interrupt_status = csr_read32(CSR_FH_INT_STATUS);
    if (iwl.hw_revision == 0xffffffffu || iwl.gp_control == 0xffffffffu) {
        serial_print("[iwlwifi] ERROR: CSR window does not respond\n");
        return -3;
    }

    iwl.firmware_size = (size_t)(iwlwifi_quz_hr_77_fw_end -
                                 iwlwifi_quz_hr_77_fw_start);
    int firmware_result = iwl_fw_parse(iwlwifi_quz_hr_77_fw_start,
                                       iwl.firmware_size,
                                       iwl.config->firmware_api,
                                       &iwl.firmware);

    iwl.wifi.name = "wlan0";
    iwl.wifi.hardware_name = iwl.config->name;
    iwl.wifi.firmware_name = iwl.config->firmware;
    iwl.wifi.state = firmware_result == IWL_FW_OK
        ? WIFI_STATE_FIRMWARE_READY : WIFI_STATE_ERROR;
    iwl.wifi.driver_data = &iwl;
    if (wifi_register(&iwl.wifi) != 0)
        return -4;

    serial_print("[iwlwifi] BAR0="); serial_print_hex64(iwl.bar.base);
    serial_print(" size="); serial_print_hex64(iwl.bar.size);
    serial_print(" power-was-D"); serial_print_uint((unsigned)previous_power);
    serial_print(" hw-rev="); serial_print_hex32(iwl.hw_revision);
    serial_print("\n");
    if (firmware_result != IWL_FW_OK) {
        serial_print("[iwlwifi] ERROR: firmware parse failed: ");
        serial_print(iwl_fw_error_string(firmware_result)); serial_print("\n");
        return -5;
    }
    serial_print("[iwlwifi] firmware ");
    serial_print(iwl.firmware.human_readable);
    serial_print(" api="); serial_print_uint(iwl.firmware.api_version);
    serial_print(" sections=");
    serial_print_uint((unsigned)iwl.firmware.runtime.section_count);
    serial_print(" cpus="); serial_print_uint(iwl.firmware.num_cpus);
    serial_print(" paging="); serial_print_uint(iwl.firmware.paging_size);
    serial_print(" bytes\n");
    serial_print("[iwlwifi] firmware validated; hardware upload not enabled yet\n");
    return 0;
}

int iwlwifi_prepare_transport(void)
{
    if (!iwl.config || !iwl.csr)
        return -1;
    if (iwl.wifi.state != WIFI_STATE_FIRMWARE_READY &&
        iwl.wifi.state != WIFI_STATE_DMA_READY &&
        iwl.wifi.state != WIFI_STATE_QUEUES_READY)
        return -2;
    if (iwl.dma_ready) {
        serial_print("[iwlwifi] DMA firmware map is already prepared\n");
        return 0;
    }

    serial_print("[iwlwifi] requesting NIC-ready state (50 ms timeout)\n");
    int result = prepare_card_hardware();
    if (result != 0) {
        serial_print("[iwlwifi] ERROR: controller did not become ready\n");
        return result;
    }

    iwl.rf_id = csr_read32(CSR_HW_RF_ID);
    iwl.mac_otp0 = csr_read32(CSR_MAC_OTP0);
    iwl.mac_otp1 = csr_read32(CSR_MAC_OTP1);
    iwl.mac_strap0 = csr_read32(CSR_MAC_STRAP0);
    iwl.mac_strap1 = csr_read32(CSR_MAC_STRAP1);
    iwl.wifi.mac[0] = (uint8_t)(iwl.mac_otp0 >> 24);
    iwl.wifi.mac[1] = (uint8_t)(iwl.mac_otp0 >> 16);
    iwl.wifi.mac[2] = (uint8_t)(iwl.mac_otp0 >> 8);
    iwl.wifi.mac[3] = (uint8_t)iwl.mac_otp0;
    iwl.wifi.mac[4] = (uint8_t)(iwl.mac_otp1 >> 8);
    iwl.wifi.mac[5] = (uint8_t)iwl.mac_otp1;
    serial_print("[iwlwifi] controller ready; building firmware DMA map\n");
    result = build_firmware_dma_map();
    if (result != 0) {
        serial_print("[iwlwifi] ERROR: firmware DMA map failed, code=");
        serial_print_uint((unsigned)(-result)); serial_print("\n");
        return result;
    }

    iwl.wifi.state = WIFI_STATE_DMA_READY;
    serial_print("[iwlwifi] DMA map ready: context=");
    serial_print_hex64(iwl.context_phys);
    serial_print(" lmac="); serial_print_uint(iwl.dma_lmac);
    serial_print(" umac="); serial_print_uint(iwl.dma_umac);
    serial_print(" paging="); serial_print_uint(iwl.dma_paging);
    serial_print(" copied="); serial_print_uint((unsigned)iwl.dma_bytes);
    serial_print(" bytes\n");
    serial_print("[iwlwifi] first DMA addresses: lmac=");
    serial_print_hex64(iwl.context->dram.lmac[0]);
    serial_print(" umac="); serial_print_hex64(iwl.context->dram.umac[0]);
    serial_print(" paging="); serial_print_hex64(iwl.context->dram.paging[0]);
    serial_print("\n");
    serial_print("[iwlwifi] controller CPU remains stopped; firmware was not kicked\n");
    return 0;
}

int iwlwifi_prepare_queues(void)
{
    if (!iwl.config || !iwl.csr)
        return -1;
    if (!iwl.dma_ready) {
        int result = iwlwifi_prepare_transport();
        if (result)
            return result;
    }
    if (iwl.queues_ready) {
        serial_print("[iwlwifi] bootstrap queues are already prepared\n");
        return 0;
    }

    serial_print("[iwlwifi] allocating AX201 RX and command queues\n");
    int result = build_bootstrap_queues();
    if (result) {
        serial_print("[iwlwifi] ERROR: bootstrap queue allocation failed, code=");
        serial_print_uint((unsigned)(-result)); serial_print("\n");
        return result;
    }
    iwl.wifi.state = WIFI_STATE_QUEUES_READY;
    serial_print("[iwlwifi] queues ready: rx-free=");
    serial_print_hex64(iwl.rx_free_phys);
    serial_print(" rx-used="); serial_print_hex64(iwl.rx_used_phys);
    serial_print(" rx-status="); serial_print_hex64(iwl.rx_status_phys);
    serial_print("\n");
    serial_print("[iwlwifi] command-ring="); serial_print_hex64(iwl.command_phys);
    serial_print(" entries="); serial_print_uint(IWL_CMD_RING_ENTRIES);
    serial_print(" rx-ring="); serial_print_uint(IWL_RX_RING_ENTRIES);
    serial_print(" prepared-buffers="); serial_print_uint(iwl.rx_buffers_prepared);
    serial_print(" context-flags=");
    serial_print_hex32(iwl.context->control.flags); serial_print("\n");
    serial_print("[iwlwifi] context pointer is still not installed; CPUs remain stopped\n");
    return 0;
}

void iwlwifi_print_diagnostics(void)
{
    if (!iwl.config)
        return;
    serial_print("iwlwifi: hw-if="); serial_print_hex32(csr_read32(CSR_HW_IF_CONFIG_REG));
    serial_print(" hw-rev="); serial_print_hex32(csr_read32(CSR_HW_REV));
    serial_print(" gp-control="); serial_print_hex32(csr_read32(CSR_GP_CNTRL));
    serial_print(" reset="); serial_print_hex32(csr_read32(CSR_RESET));
    serial_print("\n");
    serial_print("iwlwifi: intr="); serial_print_hex32(csr_read32(CSR_INT));
    serial_print(" mask="); serial_print_hex32(csr_read32(CSR_INT_MASK));
    serial_print(" fh-intr="); serial_print_hex32(csr_read32(CSR_FH_INT_STATUS));
    serial_print(" pci-status="); serial_print_hex32(pci_config_read16(iwl.pci.bus,
        iwl.pci.device, iwl.pci.function, 0x06)); serial_print("\n");
    serial_print("iwlwifi: firmware-api=");
    serial_print_uint(iwl.firmware.api_version);
    serial_print(" tlvs="); serial_print_uint(iwl.firmware.tlv_count);
    serial_print(" runtime-sections=");
    serial_print_uint((unsigned)iwl.firmware.runtime.section_count);
    serial_print(" cpus="); serial_print_uint(iwl.firmware.num_cpus);
    serial_print(" paging="); serial_print_uint(iwl.firmware.paging_size);
    serial_print(" bytes\n");
    serial_print("iwlwifi: card-ready="); serial_print_uint(iwl.card_ready);
    serial_print(" dma-ready="); serial_print_uint(iwl.dma_ready);
    serial_print(" queues-ready="); serial_print_uint(iwl.queues_ready);
    serial_print(" context="); serial_print_hex64(iwl.context_phys);
    serial_print(" dma-bytes="); serial_print_uint((unsigned)iwl.dma_bytes);
    serial_print(" lmac/umac/paging="); serial_print_uint(iwl.dma_lmac);
    serial_putc('/'); serial_print_uint(iwl.dma_umac);
    serial_putc('/'); serial_print_uint(iwl.dma_paging); serial_print("\n");
    serial_print("iwlwifi: rf-id="); serial_print_hex32(iwl.rf_id);
    serial_print(" mac-otp="); serial_print_hex32(iwl.mac_otp0);
    serial_putc('/'); serial_print_hex32(iwl.mac_otp1);
    serial_print(" mac-strap="); serial_print_hex32(iwl.mac_strap0);
    serial_putc('/'); serial_print_hex32(iwl.mac_strap1); serial_print("\n");
    serial_print("iwlwifi: mac=");
    for (unsigned i = 0; i < 6; i++) {
        if (i) serial_putc(':');
        uint8_t byte = iwl.wifi.mac[i];
        const char hex[] = "0123456789abcdef";
        serial_putc(hex[byte >> 4]); serial_putc(hex[byte & 0x0f]);
    }
    serial_print(" context-flags=");
    serial_print_hex32(iwl.context ? iwl.context->control.flags : 0u);
    serial_print("\n");
    serial_print("iwlwifi: rx-free="); serial_print_hex64(iwl.rx_free_phys);
    serial_print(" rx-used="); serial_print_hex64(iwl.rx_used_phys);
    serial_print(" rx-status="); serial_print_hex64(iwl.rx_status_phys);
    serial_print(" command="); serial_print_hex64(iwl.command_phys);
    serial_print("\n");
    serial_print("iwlwifi: queue-dma-bytes=");
    serial_print_uint((unsigned)iwl.queue_dma_bytes);
    serial_print(" rx-buffers="); serial_print_uint(iwl.rx_buffers_prepared);
    serial_print(" command-size-code=");
    serial_print_uint(iwl.context ? iwl.context->command.size : 0u);
    if (iwl.command_ring) {
        serial_print(" tfd0-tbs="); serial_print_uint(iwl.command_ring[0].num_tbs);
        serial_print(" tfd0-address=");
        serial_print_hex64(iwl.command_ring[0].tbs[0].address);
    }
    serial_print("\n");
}
