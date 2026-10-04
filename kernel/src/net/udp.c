#include "udp.h"
#include "ipv4.h"
#include "util.h"
#include "../memory.h"

#define UDP_BINDINGS 8
typedef struct { uint16_t port; udp_handler_t handler; } binding_t;
static binding_t bindings[UDP_BINDINGS];
static uint8_t datagram[NETDEV_DEFAULT_MTU - 20];

int udp_bind(uint16_t port, udp_handler_t handler)
{
    if (!port || !handler) return -1;
    for (int i = 0; i < UDP_BINDINGS; i++) {
        if (bindings[i].port == port) { bindings[i].handler = handler; return 0; }
        if (!bindings[i].port) { bindings[i] = (binding_t){port, handler}; return 0; }
    }
    return -1;
}

void udp_receive(netdev_t *dev, uint32_t source, uint32_t destination, const uint8_t *p, size_t length)
{
    (void)destination;
    if (length < 8) return;
    uint16_t udp_len = net_read16(p + 4), destination_port = net_read16(p + 2);
    if (udp_len < 8 || udp_len > length) return;
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (bindings[i].port == destination_port && bindings[i].handler) {
            bindings[i].handler(dev, source, net_read16(p), p + 8, udp_len - 8); return;
        }
}

int udp_send(netdev_t *dev, uint32_t destination, uint16_t source_port, uint16_t destination_port,
             const void *payload, size_t length)
{
    if (!payload || length + 8 > sizeof(datagram)) return -1;
    net_write16(datagram, source_port); net_write16(datagram + 2, destination_port);
    net_write16(datagram + 4, (uint16_t)(length + 8)); net_write16(datagram + 6, 0);
    memcpy(datagram + 8, payload, length);
    // A zero UDP checksum is legal in IPv4. Software checksumming can be added without changing this API.
    return ipv4_send(dev, destination, 17, datagram, length + 8);
}
