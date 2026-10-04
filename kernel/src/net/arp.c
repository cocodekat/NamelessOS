#include "arp.h"
#include "ethernet.h"
#include "ipv4.h"
#include "util.h"
#include "../memory.h"

#define ARP_TABLE_SIZE 16
typedef struct { uint32_t ip; uint8_t mac[6]; int valid; } arp_entry_t;
static arp_entry_t table[ARP_TABLE_SIZE];
static unsigned replace_index;

void arp_init(void) { memset(table, 0, sizeof(table)); replace_index = 0; }

void arp_learn(uint32_t address, const uint8_t mac[6])
{
    if (!address || !mac) return;
    int slot = -1;
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (table[i].valid && table[i].ip == address) { slot = i; break; }
        if (!table[i].valid && slot < 0) slot = i;
    }
    if (slot < 0) slot = (int)(replace_index++ % ARP_TABLE_SIZE);
    table[slot].ip = address; memcpy(table[slot].mac, mac, 6); table[slot].valid = 1;
}

int arp_lookup(uint32_t address, uint8_t mac[6])
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++)
        if (table[i].valid && table[i].ip == address) { memcpy(mac, table[i].mac, 6); return 1; }
    return 0;
}

static int send_packet(netdev_t *dev, uint16_t operation, const uint8_t target_mac[6], uint32_t target_ip)
{
    uint8_t p[28];
    net_write16(p, 1); net_write16(p + 2, ETH_TYPE_IPV4); p[4] = 6; p[5] = 4;
    net_write16(p + 6, operation); memcpy(p + 8, dev->mac, 6);
    net_write32(p + 14, ipv4_get_config()->address);
    memcpy(p + 18, target_mac, 6); net_write32(p + 24, target_ip);
    uint8_t broadcast[6] = {255,255,255,255,255,255};
    return ethernet_send(dev, operation == 1 ? broadcast : target_mac, ETH_TYPE_ARP, p, sizeof(p));
}

int arp_request(netdev_t *dev, uint32_t address)
{
    uint8_t zero[6] = {0}; return send_packet(dev, 1, zero, address);
}

void arp_receive(netdev_t *dev, const uint8_t source_mac[6], const uint8_t *p, size_t length)
{
    if (length < 28 || net_read16(p) != 1 || net_read16(p + 2) != ETH_TYPE_IPV4 || p[4] != 6 || p[5] != 4) return;
    uint16_t op = net_read16(p + 6); uint32_t sender_ip = net_read32(p + 14);
    uint32_t target_ip = net_read32(p + 24);
    arp_learn(sender_ip, p + 8);
    if (op == 1 && target_ip != 0 && target_ip == ipv4_get_config()->address)
        send_packet(dev, 2, source_mac, sender_ip);
}
