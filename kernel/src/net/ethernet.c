#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "util.h"
#include "../memory.h"

#define ETH_HEADER_SIZE 14
static uint8_t tx_frame[ETH_HEADER_SIZE + NETDEV_DEFAULT_MTU];

static void receive(netdev_t *dev, const uint8_t *frame, size_t length)
{
    if (length < ETH_HEADER_SIZE) return;
    uint16_t type = net_read16(frame + 12);
    if (type == ETH_TYPE_ARP) arp_receive(dev, frame + 6, frame + 14, length - 14);
    else if (type == ETH_TYPE_IPV4) ipv4_receive(dev, frame + 6, frame + 14, length - 14);
}

void ethernet_init(void) { netdev_set_rx_handler(receive); }

int ethernet_send(netdev_t *dev, const uint8_t destination[6], uint16_t type,
                  const void *payload, size_t length)
{
    if (!dev || !destination || !payload || length > dev->mtu) return -1;
    memcpy(tx_frame, destination, 6); memcpy(tx_frame + 6, dev->mac, 6);
    net_write16(tx_frame + 12, type); memcpy(tx_frame + 14, payload, length);
    return netdev_transmit(dev, tx_frame, 14 + length);
}
