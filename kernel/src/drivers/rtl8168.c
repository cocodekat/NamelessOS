#include "rtl8168.h"
#include "../pci.h"
#include "../serial.h"
#include "../io.h"

#define RTL_VENDOR_ID 0x10EC
#define RTL_DEVICE_ID 0x8168

#define RTL_REG_MAC0 0x00
#define RTL_REG_MAC4 0x04
#define RTL_REG_CHIPCMD 0x37

static volatile uint8_t *rtl_mmio = 0;

static uint8_t mmio_read8(uint32_t reg)
{
    return *(volatile uint8_t *)(rtl_mmio + reg);
}

static uint32_t mmio_read32(uint32_t reg)
{
    return *(volatile uint32_t *)(rtl_mmio + reg);
}

static void mmio_write8(uint32_t reg, uint8_t value)
{
    *(volatile uint8_t *)(rtl_mmio + reg) = value;
}

static void print_hex8(uint8_t value)
{
    const char hex[] = "0123456789ABCDEF";

    char s[3];
    s[0] = hex[(value >> 4) & 0xF];
    s[1] = hex[value & 0xF];
    s[2] = '\0';

    serial_print(s);
}

static void print_hex32(uint32_t value)
{
    const char hex[] = "0123456789ABCDEF";

    char s[9];

    for (int i = 0; i < 8; i++)
    {
        s[7 - i] = hex[value & 0xF];
        value >>= 4;
    }

    s[8] = '\0';
    serial_print(s);
}

int rtl8168_init(void)
{
    pci_device_t dev;

    serial_print("[rtl8168] searching for Realtek RTL8168...\n");

    /*
     * Ethernet controller:
     *
     * class    = 0x02
     * subclass = 0x00
     * prog-if  = 0x00
     */
    if (!pci_find_by_class(0x02, 0x00, 0x00, &dev))
    {
        serial_print("[rtl8168] no Ethernet controller found\n");
        return -1;
    }

    uint32_t id = pci_config_read32(
        dev.bus,
        dev.device,
        dev.function,
        0x00);

    uint16_t vendor = id & 0xFFFF;
    uint16_t device = id >> 16;

    serial_print("[rtl8168] PCI device: ");
    print_hex8(dev.bus);
    serial_print(":");
    print_hex8(dev.device);
    serial_print(".");
    print_hex8(dev.function);
    serial_print("\n");

    serial_print("[rtl8168] vendor = 0x");
    print_hex32(vendor);
    serial_print("\n");

    serial_print("[rtl8168] device = 0x");
    print_hex32(device);
    serial_print("\n");

    /*
     * Make sure this is actually the Realtek NIC we expect.
     */
    if (vendor != RTL_VENDOR_ID || device != RTL_DEVICE_ID)
    {
        serial_print("[rtl8168] Ethernet controller is not RTL8168\n");
        return -1;
    }

    serial_print("[rtl8168] RTL8168 found!\n");

    /*
     * Enable:
     *   Memory Space
     *   Bus Mastering
     */
    pci_enable_device(dev);

    uint32_t bar0_raw = pci_config_read32(
        dev.bus,
        dev.device,
        dev.function,
        0x10);

    uint16_t io_base = (uint16_t)(bar0_raw & ~0x3u);

    serial_print("[rtl8168] BAR0 raw = ");
    serial_print_hex64(bar0_raw);
    serial_print("\n");

    serial_print("[rtl8168] I/O base = ");
    serial_print_hex64(io_base);
    serial_print("\n");

    if ((bar0_raw & 1) == 0)
    {
        serial_print("[rtl8168] BAR0 is not an I/O BAR\n");
        return -1;
    }

    uint8_t mac[6];

    for (int i = 0; i < 6; i++)
    {
        mac[i] = inb(io_base + i);
    }

    serial_print("[rtl8168] MAC = ");

    for (int i = 0; i < 6; i++)
    {
        serial_print_hex64(mac[i]);

        if (i != 5)
            serial_print(":");
    }

    serial_print("\n");

    /*
     * Read the MAC address.
     *
     * Registers 0x00-0x05 contain the Ethernet MAC address.
     */
    uint32_t mac_low = mmio_read32(RTL_REG_MAC0);
    uint16_t mac_high =
        *(volatile uint16_t *)(rtl_mmio + RTL_REG_MAC4);

    uint8_t mac[6];

    mac[0] = mac_low & 0xFF;
    mac[1] = (mac_low >> 8) & 0xFF;
    mac[2] = (mac_low >> 16) & 0xFF;
    mac[3] = (mac_low >> 24) & 0xFF;
    mac[4] = mac_high & 0xFF;
    mac[5] = (mac_high >> 8) & 0xFF;

    serial_print("[rtl8168] MAC = ");

    for (int i = 0; i < 6; i++)
    {
        print_hex8(mac[i]);

        if (i != 5)
            serial_print(":");
    }

    serial_print("\n");

    return 0;
}