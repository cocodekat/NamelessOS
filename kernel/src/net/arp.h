#ifndef NET_ARP_H
#define NET_ARP_H
#include <stddef.h>
#include <stdint.h>
#include "netdev.h"
void arp_init(void);
void arp_receive(netdev_t *dev, const uint8_t source_mac[6], const uint8_t *packet, size_t length);
void arp_learn(uint32_t address, const uint8_t mac[6]);
int arp_lookup(uint32_t address, uint8_t mac[6]);
int arp_request(netdev_t *dev, uint32_t address);
#endif
