#include "netdev.h"
#include "../poll.h"
#include "../serial.h"

static netdev_t *devices[NETDEV_MAX_DEVICES];
static int device_count;
static netdev_rx_fn rx_handler;
static int poll_registered;

static void netdev_poll_callback(void *context)
{
    (void)context;
    netdev_poll_all();
}

int netdev_register(netdev_t *dev)
{
    if (dev == 0 || dev->ops == 0 || dev->ops->transmit == 0 ||
        device_count == NETDEV_MAX_DEVICES)
        return -1;
    if (!poll_registered)
    {
        if (poll_register(netdev_poll_callback, 0) != 0)
            return -1;
        poll_registered = 1;
    }
    if (dev->mtu == 0)
        dev->mtu = NETDEV_DEFAULT_MTU;
    devices[device_count++] = dev;
    serial_print("[netdev] registered ");
    serial_print(dev->name ? dev->name : "unnamed");
    serial_print("\n");
    return 0;
}

void netdev_set_rx_handler(netdev_rx_fn handler) { rx_handler = handler; }

void netdev_receive(netdev_t *dev, const void *frame, size_t length)
{
    if (rx_handler != 0 && dev != 0 && frame != 0 && length >= 14)
        rx_handler(dev, (const uint8_t *)frame, length);
}

int netdev_transmit(netdev_t *dev, const void *frame, size_t length)
{
    if (dev == 0 || frame == 0 || length < 14 || length > (size_t)dev->mtu + 14u)
        return -1;
    return dev->ops->transmit(dev, frame, length);
}

void netdev_poll_all(void)
{
    for (int i = 0; i < device_count; i++)
    {
        netdev_t *dev = devices[i];
        if (dev->ops->poll != 0)
            dev->ops->poll(dev);
        if (dev->ops->get_link != 0)
            dev->link_up = dev->ops->get_link(dev);
    }
}

netdev_t *netdev_default(void) { return device_count ? devices[0] : 0; }
int netdev_count(void) { return device_count; }
netdev_t *netdev_get(int index)
{
    return index >= 0 && index < device_count ? devices[index] : 0;
}
