#ifndef NET_ETHERNET_H
#define NET_ETHERNET_H
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"
#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806
void ethernet_init(void);
int ethernet_send(netdev_t *dev, const uint8_t destination[6], uint16_t type,
                  const void *payload, size_t length);
#endif
