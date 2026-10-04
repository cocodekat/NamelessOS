#include "rtl8168.h"
#include "rtl8168_firmware.h"
#include "../pci.h"
#include "../mm.h"
#include "../memory.h"
#include "../serial.h"
#include "../io.h"
#include "../delay.h"
#include "../net/netdev.h"

#define RTL_VENDOR_ID 0x10EC
#define RTL_DEVICE_ID 0x8168
#define REG_MAC0 0x00
#define REG_TX_DESC 0x20
#define REG_CHIP_CMD 0x37
#define REG_TX_POLL 0x38
#define REG_INTR_MASK 0x3C
#define REG_INTR_STATUS 0x3E
#define REG_TX_CONFIG 0x40
#define REG_RX_CONFIG 0x44
#define REG_CFG9346 0x50
#define REG_PHY_STATUS 0x6C
#define REG_ERI_DATA 0x70
#define REG_ERI_ADDRESS 0x74
#define REG_EPHY_ADDRESS 0x80
#define REG_MAC_OCP 0xB0
#define REG_GPHY_OCP 0xB8
#define REG_DLL_POWER 0xD0
#define REG_MISC 0xF0
#define REG_MISC_1 0xF2
#define REG_RX_MAX_SIZE 0xDA
#define REG_CPLUS_CMD 0xE0
#define REG_INTR_MITIGATE 0xE2
#define REG_RX_DESC 0xE4
#define REG_MAX_TX_SIZE 0xEC

#define CMD_RESET 0x10
#define CMD_RX_ENABLE 0x08
#define CMD_TX_ENABLE 0x04
#define DESC_OWN (1u << 31)
#define DESC_EOR (1u << 30)
#define DESC_FS (1u << 29)
#define DESC_LS (1u << 28)
#define DESC_LEN_MASK 0x3FFFu
#define RX_ERROR_MASK ((1u << 22) | (1u << 21) | (1u << 20) | (1u << 19))
#define RING_SIZE 64
#define BUFFER_SIZE 2048
#define HW_REVISION_MASK 0x7CFu
#define TX_PACKET_MAX_UNITS 0x27u
#define OCP_COMMAND_FLAG 0x80000000u
#define OCP_STD_PHY_BASE 0xA400u

/* RTL8168E-VL and newer receive/transmit configuration. */
#define RX128_INT_ENABLE (1u << 15)
#define RX_MULTI_ENABLE (1u << 14)
#define RX_EARLY_OFF (1u << 11)
#define RX_DMA_BURST (7u << 8)
#define TX_DMA_BURST (7u << 8)
#define TX_INTERFRAME_GAP (3u << 24)
#define TX_AUTO_FIFO (1u << 7)
#define CPLUS_NORMAL_MODE (1u << 13)
#define CPLUS_RX_VLAN (1u << 6)
#define CPLUS_RX_CHECKSUM (1u << 5)
#define CPLUS_INTERRUPT_TIMER_MASK 3u
#define CPLUS_PACKET_COUNTER_DISABLE (1u << 7)

typedef struct
{
    volatile uint32_t options1;
    volatile uint32_t options2;
    volatile uint64_t address;
} __attribute__((packed, aligned(16))) rtl_desc_t;

typedef struct
{
    pci_device_t pci;
    pci_bar_t bar;
    uint32_t tx_config;
    uint16_t hw_revision;
    uint16_t rx_index;
    uint16_t tx_index;
    uint16_t tx_clean_index;
    uint64_t rx_packets, tx_packets, tx_completed, rx_errors, tx_errors, tx_busy;
    uint16_t last_interrupt_status;
    uint16_t ocp_base;
    int firmware_status;
    char firmware_version[33];
    unsigned int diagnostic_events;
    int last_link;
    netdev_t netdev;
} rtl_state_t;

static rtl_state_t rtl;
static rtl_desc_t rx_ring[RING_SIZE] __attribute__((aligned(256)));
static rtl_desc_t tx_ring[RING_SIZE] __attribute__((aligned(256)));
static uint8_t rx_buffers[RING_SIZE][BUFFER_SIZE] __attribute__((aligned(64)));
static uint8_t tx_buffers[RING_SIZE][BUFFER_SIZE] __attribute__((aligned(64)));

static volatile uint8_t *mmio(void) { return (volatile uint8_t *)phys_to_virt(rtl.bar.base); }
static uint8_t r8(uint16_t r) { return rtl.bar.kind == PCI_BAR_IO ? inb((uint16_t)(rtl.bar.base + r)) : *(volatile uint8_t *)(mmio() + r); }
static uint16_t r16(uint16_t r) { return rtl.bar.kind == PCI_BAR_IO ? inw((uint16_t)(rtl.bar.base + r)) : *(volatile uint16_t *)(mmio() + r); }
static uint32_t r32(uint16_t r) { return rtl.bar.kind == PCI_BAR_IO ? inl((uint16_t)(rtl.bar.base + r)) : *(volatile uint32_t *)(mmio() + r); }
static void w8(uint16_t r, uint8_t v)
{
    if (rtl.bar.kind == PCI_BAR_IO)
        outb((uint16_t)(rtl.bar.base + r), v);
    else
        *(volatile uint8_t *)(mmio() + r) = v;
}
static void w16(uint16_t r, uint16_t v)
{
    if (rtl.bar.kind == PCI_BAR_IO)
        outw((uint16_t)(rtl.bar.base + r), v);
    else
        *(volatile uint16_t *)(mmio() + r) = v;
}
static void w32(uint16_t r, uint32_t v)
{
    if (rtl.bar.kind == PCI_BAR_IO)
        outl((uint16_t)(rtl.bar.base + r), v);
    else
        *(volatile uint32_t *)(mmio() + r) = v;
}

static void print_mac(const uint8_t mac[6])
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++)
    {
        serial_putc(hex[mac[i] >> 4]);
        serial_putc(hex[mac[i] & 15]);
        if (i != 5)
            serial_putc(':');
    }
}

static int match_device(pci_device_t dev, uint16_t vendor, uint16_t device, void *ctx)
{
    (void)ctx;
    if (vendor != RTL_VENDOR_ID || device != RTL_DEVICE_ID)
        return 0;
    rtl.pci = dev;
    return 1;
}

static int select_bar(void)
{
    pci_bar_t io_bar = {0}, mmio_bar = {0};
    int io_index = -1, mmio_index = -1;
    for (int i = 0; i < 6; i++)
    {
        pci_bar_t bar;
        if (!pci_get_bar(rtl.pci, i, &bar))
            continue;
        serial_print("[rtl8168] BAR");
        serial_print_uint((unsigned)i);
        serial_print(bar.kind == PCI_BAR_IO ? " I/O base=" : " MMIO base=");
        serial_print_hex64(bar.base);
        serial_print(" size=");
        serial_print_hex64(bar.size);
        serial_print("\n");
        if (bar.kind == PCI_BAR_MMIO && bar.size >= 0x100 && mmio_index < 0)
        {
            mmio_bar = bar;
            mmio_index = i;
        }
        if (bar.kind == PCI_BAR_IO && bar.size >= 0x100 &&
            bar.base + 0xFF <= 0xFFFF && io_index < 0)
        {
            io_bar = bar;
            io_index = i;
        }
        if (bar.is_64bit)
            i++;
    }
    /*
     * If this device exposes both aliases, prefer port I/O.  Port accesses
     * are strongly serialized on x86 and avoid depending on platform MMIO
     * write-posting behavior while the real-hardware DMA issue is diagnosed.
     */
    if (io_index >= 0)
    {
        rtl.bar = io_bar;
        return io_index;
    }
    if (mmio_index >= 0)
        rtl.bar = mmio_bar;
    return mmio_index;
}

static int reset_controller(void)
{
    w8(REG_CHIP_CMD, CMD_RESET);
    for (int i = 0; i < 100; i++)
    {
        if (!(r8(REG_CHIP_CMD) & CMD_RESET))
            return 0;
        delay_ms(1);
    }
    serial_print("[rtl8168] ERROR: reset timed out\n");
    return -1;
}

static int wait_reg32(uint16_t reg, uint32_t bit, int want_set)
{
    for (int i = 0; i < 10000; i++)
        if (((r32(reg) & bit) != 0) == want_set)
            return 0;
    return -1;
}

static int ephy_read(uint8_t reg, uint16_t *value)
{
    w32(REG_EPHY_ADDRESS, (uint32_t)(reg & 0x1Fu) << 16);
    if (wait_reg32(REG_EPHY_ADDRESS, 0x80000000u, 1) != 0)
        return -1;
    *value = (uint16_t)r32(REG_EPHY_ADDRESS);
    return 0;
}

static int ephy_write(uint8_t reg, uint16_t value)
{
    w32(REG_EPHY_ADDRESS, 0x80000000u |
                              ((uint32_t)(reg & 0x1Fu) << 16) | value);
    if (wait_reg32(REG_EPHY_ADDRESS, 0x80000000u, 0) != 0)
        return -1;
    delay_ms(1);
    return 0;
}

static int eri_read(uint16_t address, uint32_t *value)
{
    w32(REG_ERI_ADDRESS, 0x0000F000u | (address & 0x0FFFu));
    if (wait_reg32(REG_ERI_ADDRESS, 0x80000000u, 1) != 0)
        return -1;
    *value = r32(REG_ERI_DATA);
    return 0;
}

static int eri_write(uint16_t address, uint8_t byte_mask, uint32_t value)
{
    w32(REG_ERI_DATA, value);
    w32(REG_ERI_ADDRESS, 0x80000000u | ((uint32_t)(byte_mask & 0xFu) << 12) |
                             (address & 0x0FFFu));
    return wait_reg32(REG_ERI_ADDRESS, 0x80000000u, 0);
}

static int eri_modify(uint16_t address, uint32_t clear, uint32_t set)
{
    uint32_t value;
    if (eri_read(address, &value) != 0)
        return -1;
    return eri_write(address, 0xFu, (value & ~clear) | set);
}

static int ocp_reg_valid(uint32_t reg)
{
    return reg <= 0xffffu && !(reg & 1u);
}

static int gphy_wait(int want_set)
{
    for (int i = 0; i < 100000; i++)
    {
        if (((r32(REG_GPHY_OCP) & OCP_COMMAND_FLAG) != 0) == want_set)
            return 0;
        asm volatile("pause");
    }
    return -1;
}

static int phy_ocp_write(uint16_t reg, uint16_t value)
{
    if (!ocp_reg_valid(reg))
        return -1;
    w32(REG_GPHY_OCP, OCP_COMMAND_FLAG | ((uint32_t)reg << 15) | value);
    return gphy_wait(0);
}

static int phy_ocp_read(uint16_t reg, uint16_t *value)
{
    if (!ocp_reg_valid(reg))
        return -1;
    w32(REG_GPHY_OCP, (uint32_t)reg << 15);
    if (gphy_wait(1) != 0)
        return -1;
    *value = (uint16_t)r32(REG_GPHY_OCP);
    return 0;
}

static int firmware_phy_write(void *context, uint16_t reg, uint16_t value)
{
    rtl_state_t *state = context;
    if (reg == 0x1f)
    {
        state->ocp_base = value ? (uint16_t)(value << 4) : OCP_STD_PHY_BASE;
        return 0;
    }
    if (state->ocp_base != OCP_STD_PHY_BASE)
    {
        if (reg < 0x10)
            return -1;
        reg = (uint16_t)(reg - 0x10);
    }
    uint32_t target = (uint32_t)state->ocp_base + (uint32_t)reg * 2;
    if (!ocp_reg_valid(target))
        return -1;
    return phy_ocp_write((uint16_t)target, value);
}

static int firmware_phy_read(void *context, uint16_t reg, uint16_t *value)
{
    rtl_state_t *state = context;
    if (reg == 0x1f)
    {
        *value = state->ocp_base == OCP_STD_PHY_BASE ? 0 : (uint16_t)(state->ocp_base >> 4);
        return 0;
    }
    if (state->ocp_base != OCP_STD_PHY_BASE)
    {
        if (reg < 0x10)
            return -1;
        reg = (uint16_t)(reg - 0x10);
    }
    uint32_t target = (uint32_t)state->ocp_base + (uint32_t)reg * 2;
    if (!ocp_reg_valid(target))
        return -1;
    return phy_ocp_read((uint16_t)target, value);
}

static int firmware_mac_write(void *context, uint16_t reg, uint16_t value)
{
    rtl_state_t *state = context;
    if (reg == 0x1f)
    {
        state->ocp_base = (uint16_t)(value << 4);
        return 0;
    }
    uint32_t target = (uint32_t)state->ocp_base + reg;
    if (!ocp_reg_valid(target))
        return -1;
    w32(REG_MAC_OCP, OCP_COMMAND_FLAG | (target << 15) | value);
    return 0;
}

static int firmware_mac_read(void *context, uint16_t reg, uint16_t *value)
{
    rtl_state_t *state = context;
    uint32_t target = (uint32_t)state->ocp_base + reg;
    if (!ocp_reg_valid(target))
        return -1;
    w32(REG_MAC_OCP, target << 15);
    *value = (uint16_t)r32(REG_MAC_OCP);
    return 0;
}

static int mac_ocp_read(uint16_t reg, uint16_t *value)
{
    if (!ocp_reg_valid(reg))
        return -1;
    w32(REG_MAC_OCP, (uint32_t)reg << 15);
    *value = (uint16_t)r32(REG_MAC_OCP);
    return 0;
}

static int mac_ocp_write(uint16_t reg, uint16_t value)
{
    if (!ocp_reg_valid(reg))
        return -1;
    w32(REG_MAC_OCP, OCP_COMMAND_FLAG | ((uint32_t)reg << 15) | value);
    return 0;
}

static int mac_ocp_modify(uint16_t reg, uint16_t clear, uint16_t set)
{
    uint16_t value;
    if (mac_ocp_read(reg, &value) != 0)
        return -1;
    return mac_ocp_write(reg, (uint16_t)((value & ~clear) | set));
}

static int apply_8168h_firmware(void)
{
    const rtl8168_fw_bus_t phy = {
        &rtl, firmware_phy_read, firmware_phy_write};
    const rtl8168_fw_bus_t mac = {
        &rtl, firmware_mac_read, firmware_mac_write};
    size_t size = (size_t)(rtl8168h_2_fw_end - rtl8168h_2_fw_start);

    rtl.ocp_base = OCP_STD_PHY_BASE;
    int result = rtl8168_fw_apply(rtl8168h_2_fw_start, size, &phy, &mac,
                                  rtl.firmware_version);
    rtl.ocp_base = OCP_STD_PHY_BASE;
    rtl.firmware_status = result;
    if (result == 0)
    {
        serial_print("[rtl8168] firmware applied: ");
        serial_print(rtl.firmware_version);
        serial_print("\n");
    }
    else
    {
        serial_print("[rtl8168] ERROR: rtl8168h-2 firmware failed, code=");
        serial_print_uint((unsigned)(-result));
        serial_print("\n");
    }
    return result;
}

static int configure_8168h(void)
{
    static const struct
    {
        uint8_t reg;
        uint16_t clear, set;
    } ephy[] = {
        {0x1e, 0x0800, 0x0001}, {0x1d, 0x0000, 0x0800}, {0x05, 0xffff, 0x2089}, {0x06, 0xffff, 0x5881}, {0x04, 0xffff, 0x854a}, {0x01, 0xffff, 0x068b}};

    /* myOS does not manage PCIe ASPM yet, so keep the NIC-side controls off. */
    /* Config3 bit 1 says the device may remain in the problematic L2/L3
       ready state across a PCI reset.  Clear it before bringing DMA up. */
    w8(0x53, (uint8_t)(r8(0x53) & ~((1u << 7) | (1u << 1))));
    w8(0x56, (uint8_t)(r8(0x56) & ~1u));
    for (unsigned i = 0; i < sizeof(ephy) / sizeof(ephy[0]); i++)
    {
        uint16_t value;
        if (ephy_read(ephy[i].reg, &value) != 0 ||
            ephy_write(ephy[i].reg,
                       (uint16_t)((value & ~ephy[i].clear) | ephy[i].set)) != 0)
            return -1;
    }

    /* Establish the H-generation RX/TX FIFO split and pause thresholds. */
    if (eri_write(0x0c8, 0xFu, (0x08u << 16) | 0x02u) != 0 ||
        eri_write(0x0e8, 0xFu, (0x10u << 16) | 0x06u) != 0 ||
        eri_write(0x0cc, 0x1u, 0x38u) != 0 ||
        eri_write(0x0d0, 0x1u, 0x48u) != 0 ||
        eri_modify(0x0dc, 1u, 0) != 0 ||
        eri_modify(0x0dc, 0, 1u) != 0 ||
        eri_modify(0x0d4, 0, 0x1f00u) != 0 ||
        eri_modify(0x0dc, 0, 0x001cu) != 0 ||
        eri_write(0x5f0, 0x3u, 0x4f87u) != 0 ||
        eri_write(0x0c0, 0x3u, 0) != 0 || eri_write(0x0b8, 0x3u, 0) != 0 ||
        eri_modify(0x1b0, 1u << 12, 0) != 0)
        return -1;

    w32(REG_MISC, r32(REG_MISC) & ~(1u << 19));
    w8(REG_DLL_POWER, (uint8_t)(r8(REG_DLL_POWER) & ~((1u << 7) | (1u << 6))));
    w8(REG_MISC_1, (uint8_t)(r8(REG_MISC_1) & ~(1u << 6)));
    w8(0x54, (uint8_t)(r8(0x54) & ~(1u << 1)));
    serial_print("[rtl8168] RTL8168H EPHY/ERI datapath setup complete\n");
    return 0;
}

static int configure_8168h_mac_timing(void)
{
    /*
     * These are H-generation MAC timing and FIFO controls.  They are kept
     * isolated from the common descriptor setup because other RTL8168 XIDs
     * use different OCP layouts.
     */
    uint16_t saw_count;
    if (firmware_phy_write(&rtl, 0x1f, 0x0c42) != 0 ||
        firmware_phy_read(&rtl, 0x13, &saw_count) != 0)
        return -1;
    saw_count &= 0x3fffu;
    if (saw_count != 0 &&
        mac_ocp_modify(0xd412, 0x0fff,
                       (uint16_t)((16000000u / saw_count) & 0x0fffu)) != 0)
        return -1;

    if (mac_ocp_modify(0xe056, 0x00f0, 0) != 0 ||
        mac_ocp_modify(0xe052, 0x6000, 0x8008) != 0 ||
        mac_ocp_modify(0xe0d6, 0x01ff, 0x017f) != 0 ||
        mac_ocp_modify(0xd420, 0x0fff, 0x047f) != 0 ||
        mac_ocp_write(0xe63e, 0x0001) != 0 ||
        mac_ocp_write(0xe63e, 0x0000) != 0 ||
        mac_ocp_write(0xc094, 0x0000) != 0 ||
        mac_ocp_write(0xc09e, 0x0000) != 0)
        return -1;
    rtl.ocp_base = OCP_STD_PHY_BASE;
    serial_print("[rtl8168] RTL8168H MAC timing setup complete\n");
    return 0;
}

static void apply_revision_quirks(void)
{
    rtl.tx_config = r32(REG_TX_CONFIG);
    /* The upper TxConfig bits contain a revision code with reserved holes. */
    rtl.hw_revision = (uint16_t)((rtl.tx_config >> 20) & HW_REVISION_MASK);
    serial_print("[rtl8168] TxConfig=");
    serial_print_hex32(rtl.tx_config);
    serial_print(" revision field=");
    serial_print_hex32(rtl.hw_revision);
    if (rtl.hw_revision == 0x541)
        serial_print(" (RTL8168H family); conservative descriptor mode\n");
    else
        serial_print("; conservative descriptor mode\n");
}

static void init_rings(void)
{
    memset(rx_ring, 0, sizeof(rx_ring));
    memset(tx_ring, 0, sizeof(tx_ring));
    for (int i = 0; i < RING_SIZE; i++)
    {
        rx_ring[i].address = virt_to_phys(rx_buffers[i]);
        rx_ring[i].options1 = DESC_OWN | BUFFER_SIZE | (i == RING_SIZE - 1 ? DESC_EOR : 0);
        tx_ring[i].address = virt_to_phys(tx_buffers[i]);
        tx_ring[i].options1 = i == RING_SIZE - 1 ? DESC_EOR : 0;
    }
    rtl.rx_index = rtl.tx_index = rtl.tx_clean_index = 0;
}

static int get_link(netdev_t *dev)
{
    (void)dev;
    return (r8(REG_PHY_STATUS) & 2u) != 0;
}

static int transmit(netdev_t *dev, const void *frame, size_t length)
{
    (void)dev;
    if (length > BUFFER_SIZE)
        return -1;
    uint16_t i = rtl.tx_index;
    rtl_desc_t *desc = &tx_ring[i];
    if (desc->options1 & DESC_OWN)
    {
        rtl.tx_busy++;
        return -2;
    }
    size_t wire_len = length < 60 ? 60 : length;
    memcpy(tx_buffers[i], frame, length);
    if (wire_len > length)
        memset(tx_buffers[i] + length, 0, wire_len - length);
    desc->options2 = 0;
    asm volatile("sfence" ::: "memory");
    desc->options1 = DESC_OWN | DESC_FS | DESC_LS | (uint32_t)wire_len |
                     (i == RING_SIZE - 1 ? DESC_EOR : 0);
    asm volatile("sfence" ::: "memory");
    w8(REG_TX_POLL, 0x40);
    rtl.tx_index = (uint16_t)((i + 1) % RING_SIZE);
    rtl.tx_packets++;
    return 0;
}

static void poll_device(netdev_t *dev)
{
    uint16_t status = r16(REG_INTR_STATUS);
    if (status)
    {
        rtl.last_interrupt_status = status;
        w16(REG_INTR_STATUS, status);
        if (rtl.diagnostic_events++ < 12)
        {
            serial_print("[rtl8168] interrupt status=");
            serial_print_hex32(status);
            serial_print("\n");
        }
    }
    /* Reclaim every descriptor that hardware has returned to the CPU. */
    while (rtl.tx_clean_index != rtl.tx_index)
    {
        rtl_desc_t *tx = &tx_ring[rtl.tx_clean_index];
        uint32_t opts = tx->options1;
        if (opts & DESC_OWN)
            break;
        rtl.tx_completed++;
        if (opts & 0x00008000u)
            rtl.tx_errors++;
        if (rtl.diagnostic_events++ < 12)
        {
            serial_print("[rtl8168] TX complete desc=");
            serial_print_uint(rtl.tx_clean_index);
            serial_print(" status=");
            serial_print_hex32(opts);
            serial_print("\n");
        }
        rtl.tx_clean_index = (uint16_t)((rtl.tx_clean_index + 1) % RING_SIZE);
    }
    for (int budget = 0; budget < RING_SIZE; budget++)
    {
        uint16_t i = rtl.rx_index;
        rtl_desc_t *desc = &rx_ring[i];
        uint32_t opts = desc->options1;
        if (opts & DESC_OWN)
            break;
        asm volatile("" ::: "memory");
        uint32_t len = opts & DESC_LEN_MASK;
        if ((opts & (DESC_FS | DESC_LS)) != (DESC_FS | DESC_LS) ||
            (opts & RX_ERROR_MASK) || len < 4 || len > BUFFER_SIZE)
        {
            rtl.rx_errors++;
            serial_print("[rtl8168] RX error desc=");
            serial_print_uint(i);
            serial_print(" status=");
            serial_print_hex32(opts);
            serial_print("\n");
        }
        else
        {
            if (rtl.diagnostic_events++ < 12)
            {
                serial_print("[rtl8168] RX frame desc=");
                serial_print_uint(i);
                serial_print(" length=");
                serial_print_uint(len - 4);
                serial_print("\n");
            }
            netdev_receive(dev, rx_buffers[i], len - 4);
            rtl.rx_packets++;
        }
        desc->options2 = 0;
        asm volatile("" ::: "memory");
        desc->options1 = DESC_OWN | BUFFER_SIZE | (i == RING_SIZE - 1 ? DESC_EOR : 0);
        rtl.rx_index = (uint16_t)((i + 1) % RING_SIZE);
    }
    int link = get_link(dev);
    if (link != rtl.last_link)
    {
        rtl.last_link = link;
        dev->link_up = link;
        serial_print(link ? "[rtl8168] link up\n" : "[rtl8168] link down\n");
    }
}

static const netdev_ops_t ops = {transmit, poll_device, get_link};

int rtl8168_init(void)
{
    memset(&rtl, 0, sizeof(rtl));
    rtl.last_link = -1;
    serial_print("[rtl8168] searching PCI for 10EC:8168\n");
    if (!pci_enumerate(match_device, 0))
    {
        serial_print("[rtl8168] not found\n");
        return -1;
    }
    serial_print("[rtl8168] PCI location ");
    serial_print_hex32(rtl.pci.bus);
    serial_putc(':');
    serial_print_hex32(rtl.pci.device);
    serial_putc('.');
    serial_print_hex32(rtl.pci.function);
    serial_print("\n");
    int old_power_state = pci_set_power_d0(rtl.pci);
    if (old_power_state != 0)
        delay_ms(20);
    pci_disable_link_power_management(rtl.pci);
    pci_enable_bridge_path(rtl.pci);
    serial_print("[rtl8168] PCI power was D");
    serial_print_uint((unsigned)old_power_state);
    serial_print("; endpoint ASPM/CLKREQ disabled\n");
    int bar_index = select_bar();
    if (bar_index < 0)
    {
        serial_print("[rtl8168] ERROR: no usable register BAR\n");
        return -1;
    }
    if (rtl.bar.kind == PCI_BAR_MMIO)
        mm_map_mmio(rtl.bar.base, rtl.bar.size ? rtl.bar.size : 0x1000);
    pci_enable_bus_master(rtl.pci, rtl.bar.kind == PCI_BAR_IO, rtl.bar.kind == PCI_BAR_MMIO);
    /* Clear stale W1C PCI error bits. Any status reported by netstat was
       therefore raised by this boot's DMA traffic. */
    pci_config_write16(rtl.pci.bus, rtl.pci.device, rtl.pci.function,
                       0x06, 0xffffu);
    serial_print("[rtl8168] using BAR");
    serial_print_uint((unsigned)bar_index);
    serial_print(rtl.bar.kind == PCI_BAR_IO ? " port I/O\n" : " MMIO\n");
    if (r32(REG_TX_CONFIG) == 0xFFFFFFFFu)
    {
        serial_print("[rtl8168] ERROR: registers do not respond\n");
        return -1;
    }
    if (reset_controller() != 0)
        return -1;
    for (int i = 0; i < 6; i++)
        rtl.netdev.mac[i] = r8((uint16_t)(REG_MAC0 + i));
    serial_print("[rtl8168] MAC ");
    print_mac(rtl.netdev.mac);
    serial_print("\n");
    apply_revision_quirks();
    init_rings();

    w16(REG_INTR_MASK, 0);
    w16(REG_INTR_STATUS, 0xFFFF);
    w8(REG_CFG9346, 0xC0);
    if (rtl.hw_revision == 0x541 && configure_8168h() != 0)
    {
        serial_print("[rtl8168] ERROR: RTL8168H private-register setup timed out\n");
        w8(REG_CFG9346, 0);
        return -1;
    }
    if (rtl.hw_revision == 0x541 && apply_8168h_firmware() != 0)
    {
        w8(REG_CFG9346, 0);
        return -1;
    }
    if (rtl.hw_revision == 0x541 && configure_8168h_mac_timing() != 0)
    {
        serial_print("[rtl8168] ERROR: RTL8168H MAC timing setup failed\n");
        w8(REG_CFG9346, 0);
        return -1;
    }
    /* RTL8168H's known-good normal descriptor mode uses Normal_mode plus
       RX VLAN/checksum capability bits. Keep its timer selection at zero. */
    uint16_t cplus = CPLUS_NORMAL_MODE | CPLUS_RX_VLAN | CPLUS_RX_CHECKSUM;
    w16(REG_CPLUS_CMD, cplus);
    w16(REG_INTR_MITIGATE, 0);
    w16(REG_RX_MAX_SIZE, BUFFER_SIZE);
    /* MaxTxPacketSize is an 8-bit register expressed in 128-byte units. */
    w8(REG_MAX_TX_SIZE, TX_PACKET_MAX_UNITS);
    uint64_t tx = virt_to_phys(tx_ring), rx = virt_to_phys(rx_ring);
    /* High must be written before low: the low write latches the complete
       descriptor address on several members of this controller family. */
    w32(REG_TX_DESC + 4, (uint32_t)(tx >> 32));
    w32(REG_TX_DESC, (uint32_t)tx);
    w32(REG_RX_DESC + 4, (uint32_t)(rx >> 32));
    w32(REG_RX_DESC, (uint32_t)rx);
    /* Reading CPlusCmd commits the preceding descriptor-base writes on PCIe. */
    (void)r16(REG_CPLUS_CMD);
    w8(REG_CHIP_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);
    w32(REG_RX_CONFIG, RX128_INT_ENABLE | RX_MULTI_ENABLE | RX_EARLY_OFF |
                           RX_DMA_BURST | (1u << 3) | (1u << 2) | (1u << 1));
    /* Do not preserve the reset-time lower TxConfig bits.  Revision bits are
       read-only; 8168H additionally requires automatic FIFO sizing. */
    w32(REG_TX_CONFIG, TX_INTERFRAME_GAP | TX_DMA_BURST | TX_AUTO_FIFO);
    w8(REG_CFG9346, 0);

    uint8_t mac[6];
    for (int i = 0; i < 6; i++)
        mac[i] = rtl.netdev.mac[i];
    rtl.netdev = (netdev_t){"rtl0", {0}, NETDEV_DEFAULT_MTU, 0, &rtl, &ops};
    for (int i = 0; i < 6; i++)
        rtl.netdev.mac[i] = mac[i];
    rtl.netdev.link_up = get_link(&rtl.netdev);
    if (netdev_register(&rtl.netdev) != 0)
    {
        serial_print("[rtl8168] ERROR: netdev registration failed\n");
        w8(REG_CHIP_CMD, 0);
        return -1;
    }
    rtl.last_link = rtl.netdev.link_up;
    serial_print("[rtl8168] rings active; polling mode; link ");
    serial_print(rtl.netdev.link_up ? "up\n" : "down\n");
    serial_print("[rtl8168] PCI command=");
    serial_print_hex32(pci_config_read16(rtl.pci.bus, rtl.pci.device, rtl.pci.function, 0x04));
    serial_print(" rxring=");
    serial_print_hex64(rx);
    serial_print(" txring=");
    serial_print_hex64(tx);
    serial_print("\n");
    return 0;
}

void rtl8168_print_diagnostics(void)
{
    if (rtl.netdev.ops == 0)
        return;
    serial_print("rtl8168: revision=");
    serial_print_hex32(rtl.hw_revision);
    serial_print(" intr=");
    serial_print_hex32(r16(REG_INTR_STATUS));
    serial_print(" phy=");
    serial_print_hex32(r8(REG_PHY_STATUS));
    serial_print("\n");
    if (rtl.hw_revision == 0x541)
    {
        serial_print("rtl8168: firmware=");
        serial_print(rtl.firmware_status == 0 ? rtl.firmware_version : "failed");
        serial_print(" status=");
        serial_print_uint((unsigned)rtl.firmware_status);
        serial_print("\n");
    }
    serial_print("rtl8168: tx submitted=");
    serial_print_uint((unsigned)rtl.tx_packets);
    serial_print(" completed=");
    serial_print_uint((unsigned)rtl.tx_completed);
    serial_print(" busy=");
    serial_print_uint((unsigned)rtl.tx_busy);
    serial_print(" errors=");
    serial_print_uint((unsigned)rtl.tx_errors);
    serial_print("\n");
    serial_print("rtl8168: rx frames=");
    serial_print_uint((unsigned)rtl.rx_packets);
    serial_print(" errors=");
    serial_print_uint((unsigned)rtl.rx_errors);
    serial_print(" rx_desc=");
    serial_print_uint(rtl.rx_index);
    serial_print(" tx_desc=");
    serial_print_uint(rtl.tx_index);
    serial_print("\n");
    serial_print("rtl8168: chipcmd=");
    serial_print_hex32(r8(REG_CHIP_CMD));
    serial_print(" cplus=");
    serial_print_hex32(r16(REG_CPLUS_CMD));
    serial_print(" txpoll=");
    serial_print_hex32(r8(REG_TX_POLL));
    serial_print(" tx0=");
    serial_print_hex32(tx_ring[0].options1);
    serial_print(" txbase=");
    serial_print_hex64(((uint64_t)r32(REG_TX_DESC + 4) << 32) | r32(REG_TX_DESC));
    serial_print("\n");
    serial_print("rtl8168: rxcfg=");
    serial_print_hex32(r32(REG_RX_CONFIG));
    serial_print(" txcfg=");
    serial_print_hex32(r32(REG_TX_CONFIG));
    serial_print(" rxbase=");
    serial_print_hex64(((uint64_t)r32(REG_RX_DESC + 4) << 32) | r32(REG_RX_DESC));
    serial_print("\n");
    serial_print("rtl8168: tx0-buffer=");
    serial_print_hex64(tx_ring[0].address);
    serial_print(" rx0-buffer=");
    serial_print_hex64(rx_ring[0].address);
    serial_print(" tx0-desc-cpu=");
    serial_print_hex64(virt_to_phys(&tx_ring[0]));
    serial_print("\n");
    serial_print("rtl8168: txring-virt=");
    serial_print_hex64((uint64_t)(uintptr_t)&tx_ring[0]);
    serial_print(" linear=");
    serial_print_hex64(mm_virt_to_phys_linear(&tx_ring[0]));
    serial_print(" pagetable=");
    serial_print_hex64(mm_virt_to_phys_page_table(&tx_ring[0]));
    serial_print("\n");
    serial_print("rtl8168: kernel-virt=");
    serial_print_hex64(mm_kernel_virt_base());
    serial_print(" kernel-phys=");
    serial_print_hex64(mm_kernel_phys_base());
    serial_print(" hhdm=");
    serial_print_hex64(mm_hhdm_offset());
    serial_print("\n");
    serial_print("rtl8168: pci-command=");
    serial_print_hex32(pci_config_read16(rtl.pci.bus, rtl.pci.device,
                                        rtl.pci.function, 0x04));
    serial_print(" pci-status=");
    serial_print_hex32(pci_config_read16(rtl.pci.bus, rtl.pci.device,
                                        rtl.pci.function, 0x06));
    serial_print("\n");
}
