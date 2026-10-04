#ifndef NET_UDP_H
#define NET_UDP_H
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"
typedef void (*udp_handler_t)(netdev_t *dev, uint32_t source_ip, uint16_t source_port,
                              const uint8_t *payload, size_t length);
int udp_bind(uint16_t port, udp_handler_t handler);
void udp_receive(netdev_t *dev, uint32_t source, uint32_t destination, const uint8_t *packet, size_t length);
int udp_send(netdev_t *dev, uint32_t destination, uint16_t source_port, uint16_t destination_port,
             const void *payload, size_t length);
#endif
