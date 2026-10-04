#include "e1000.h"
#include "../pci.h"
#include "../mm.h"
#include "../memory.h"
#include "../serial.h"
#include "../net/netdev.h"

#define RX_COUNT 64
#define TX_COUNT 64
#define BUFFER_SIZE 2048
#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_ICR 0x00C0
#define REG_IMC 0x00D8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_RAL 0x5400
#define REG_RAH 0x5404

typedef struct {
    uint64_t address; uint16_t length, checksum; uint8_t status, errors; uint16_t special;
} __attribute__((packed)) rx_desc_t;
typedef struct {
    uint64_t address; uint16_t length; uint8_t checksum_offset, command, status, checksum_start; uint16_t special;
} __attribute__((packed)) tx_desc_t;
typedef struct {
    pci_device_t pci; pci_bar_t bar; volatile uint8_t *mmio;
    uint16_t rx_index, tx_index; int last_link; netdev_t netdev;
} e1000_state_t;

static e1000_state_t nic;
static rx_desc_t rx_ring[RX_COUNT] __attribute__((aligned(128)));
static tx_desc_t tx_ring[TX_COUNT] __attribute__((aligned(128)));
static uint8_t rx_buffers[RX_COUNT][BUFFER_SIZE] __attribute__((aligned(64)));
static uint8_t tx_buffers[TX_COUNT][BUFFER_SIZE] __attribute__((aligned(64)));
static uint32_t rd32(uint32_t r) { return *(volatile uint32_t *)(nic.mmio + r); }
static void wr32(uint32_t r, uint32_t v) { *(volatile uint32_t *)(nic.mmio + r) = v; }

static int match(pci_device_t dev, uint16_t vendor, uint16_t device, void *ctx)
{
    (void)ctx;
    if (vendor != 0x8086 || (device != 0x100E && device != 0x100F && device != 0x1010)) return 0;
    nic.pci = dev; return 1;
}

static int link(netdev_t *dev) { (void)dev; return (rd32(REG_STATUS) & 2u) != 0; }

static int transmit(netdev_t *dev, const void *frame, size_t length)
{
    (void)dev;
    if (length > BUFFER_SIZE) return -1;
    uint16_t i = nic.tx_index; volatile tx_desc_t *d = &tx_ring[i];
    if (!(d->status & 1u)) return -2;
    size_t wire = length < 60 ? 60 : length;
    memcpy(tx_buffers[i], frame, length); if (wire > length) memset(tx_buffers[i] + length, 0, wire - length);
    d->length = (uint16_t)wire; d->status = 0; d->command = 1u | 2u | 8u;
    asm volatile ("" ::: "memory");
    nic.tx_index = (uint16_t)((i + 1) % TX_COUNT); wr32(REG_TDT, nic.tx_index);
    return 0;
}

static void poll_device(netdev_t *dev)
{
    (void)rd32(REG_ICR);
    for (int budget = 0; budget < RX_COUNT; budget++) {
        uint16_t i = nic.rx_index; volatile rx_desc_t *d = &rx_ring[i];
        if (!(d->status & 1u)) break;
        asm volatile ("" ::: "memory");
        if ((d->status & 2u) && !d->errors && d->length >= 14 && d->length <= BUFFER_SIZE)
            netdev_receive(dev, rx_buffers[i], d->length);
        d->status = 0; d->errors = 0; asm volatile ("" ::: "memory");
        wr32(REG_RDT, i); nic.rx_index = (uint16_t)((i + 1) % RX_COUNT);
    }
    int up = link(dev);
    if (up != nic.last_link) { nic.last_link = up; serial_print(up ? "[e1000] link up\n" : "[e1000] link down\n"); }
}

static const netdev_ops_t ops = {transmit, poll_device, link};

int e1000_init(void)
{
    memset(&nic, 0, sizeof(nic)); nic.last_link = -1;
    if (!pci_enumerate(match, 0)) return -1;
    if (!pci_get_bar(nic.pci, 0, &nic.bar) || nic.bar.kind != PCI_BAR_MMIO) {
        serial_print("[e1000] ERROR: BAR0 is not MMIO\n"); return -1;
    }
    pci_enable_bus_master(nic.pci, 0, 1);
    nic.mmio = mm_map_mmio(nic.bar.base, nic.bar.size ? nic.bar.size : 0x20000);
    if (rd32(REG_STATUS) == 0xFFFFFFFFu) { serial_print("[e1000] ERROR: registers do not respond\n"); return -1; }
    wr32(REG_IMC, 0xFFFFFFFFu); (void)rd32(REG_ICR);
    uint32_t ral = rd32(REG_RAL), rah = rd32(REG_RAH);
    for (int i = 0; i < 4; i++) nic.netdev.mac[i] = (uint8_t)(ral >> (i * 8));
    nic.netdev.mac[4] = (uint8_t)rah; nic.netdev.mac[5] = (uint8_t)(rah >> 8);

    memset(rx_ring, 0, sizeof(rx_ring)); memset(tx_ring, 0, sizeof(tx_ring));
    for (int i = 0; i < RX_COUNT; i++) rx_ring[i].address = virt_to_phys(rx_buffers[i]);
    for (int i = 0; i < TX_COUNT; i++) { tx_ring[i].address = virt_to_phys(tx_buffers[i]); tx_ring[i].status = 1; }
    uint64_t rx = virt_to_phys(rx_ring), tx = virt_to_phys(tx_ring);
    wr32(REG_RDBAL, (uint32_t)rx); wr32(REG_RDBAH, (uint32_t)(rx >> 32)); wr32(REG_RDLEN, sizeof(rx_ring));
    wr32(REG_RDH, 0); wr32(REG_RDT, RX_COUNT - 1);
    wr32(REG_TDBAL, (uint32_t)tx); wr32(REG_TDBAH, (uint32_t)(tx >> 32)); wr32(REG_TDLEN, sizeof(tx_ring));
    wr32(REG_TDH, 0); wr32(REG_TDT, 0);
    wr32(REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26));
    wr32(REG_TIPG, 10u | (8u << 10) | (6u << 20));
    wr32(REG_TCTL, (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12));
    wr32(REG_CTRL, rd32(REG_CTRL) | (1u << 6));

    uint8_t mac[6]; memcpy(mac, nic.netdev.mac, 6);
    nic.netdev = (netdev_t){"eth0", {0}, NETDEV_DEFAULT_MTU, 0, &nic, &ops}; memcpy(nic.netdev.mac, mac, 6);
    nic.netdev.link_up = link(&nic.netdev);
    if (netdev_register(&nic.netdev) != 0) return -1;
    nic.last_link = nic.netdev.link_up;
    serial_print("[e1000] initialized QEMU/test NIC; link "); serial_print(nic.last_link ? "up\n" : "down\n");
    return 0;
}
