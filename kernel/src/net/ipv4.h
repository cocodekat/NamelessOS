#ifndef NET_IPV4_H
#define NET_IPV4_H
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

#define IPV4_BROADCAST 0xFFFFFFFFu
typedef struct { uint32_t address, netmask, gateway, dns; int configured; } ipv4_config_t;
void ipv4_init(void);
void ipv4_set_config(uint32_t address, uint32_t netmask, uint32_t gateway, uint32_t dns);
const ipv4_config_t *ipv4_get_config(void);
void ipv4_receive(netdev_t *dev, const uint8_t source_mac[6], const uint8_t *packet, size_t length);
int ipv4_send(netdev_t *dev, uint32_t destination, uint8_t protocol, const void *payload, size_t length);
#endif
