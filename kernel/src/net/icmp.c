#include "icmp.h"
#include "ipv4.h"
#include "util.h"
#include "../memory.h"

static uint8_t reply[NETDEV_DEFAULT_MTU - 20];

void icmp_receive(netdev_t *dev, uint32_t source, uint32_t destination, const uint8_t *p, size_t length)
{
    (void)destination;
    if (length < 8 || p[0] != 8 || p[1] != 0 || net_checksum_finish(net_checksum_add(0, p, length)) != 0) return;
    if (length > sizeof(reply)) return;
    memcpy(reply, p, length); reply[0] = 0; reply[2] = 0; reply[3] = 0;
    net_write16(reply + 2, net_checksum_finish(net_checksum_add(0, reply, length)));
    ipv4_send(dev, source, 1, reply, length);
}
