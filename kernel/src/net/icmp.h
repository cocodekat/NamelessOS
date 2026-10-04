#ifndef NET_ICMP_H
#define NET_ICMP_H
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"
void icmp_receive(netdev_t *dev, uint32_t source, uint32_t destination, const uint8_t *packet, size_t length);
#endif
