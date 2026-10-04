#include "ipv4.h"
#include "ethernet.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "util.h"
#include "../memory.h"

#define IPV4_HEADER_SIZE 20
static ipv4_config_t config;
static uint16_t identification;
static uint8_t packet_buffer[NETDEV_DEFAULT_MTU];

void ipv4_init(void) { memset(&config, 0, sizeof(config)); identification = 1; }
void ipv4_set_config(uint32_t a, uint32_t m, uint32_t g, uint32_t d) { config = (ipv4_config_t){a,m,g,d,a != 0}; }
const ipv4_config_t *ipv4_get_config(void) { return &config; }

static int is_for_us(uint32_t destination)
{
    if (destination == IPV4_BROADCAST) return 1;
    if (config.configured && destination == config.address) return 1;
    if (config.configured && destination == (config.address | ~config.netmask)) return 1;
    return !config.configured && destination == 0;
}

void ipv4_receive(netdev_t *dev, const uint8_t source_mac[6], const uint8_t *p, size_t length)
{
    if (length < IPV4_HEADER_SIZE || (p[0] >> 4) != 4) return;
    size_t header_len = (size_t)(p[0] & 15u) * 4u;
    uint16_t total = net_read16(p + 2);
    if (header_len < IPV4_HEADER_SIZE || total < header_len || total > length) return;
    if (net_checksum_finish(net_checksum_add(0, p, header_len)) != 0) return;
    if (net_read16(p + 6) & 0x3FFFu) return; // fragmentation is not implemented
    uint32_t source = net_read32(p + 12), destination = net_read32(p + 16);
    if (!is_for_us(destination)) return;
    arp_learn(source, source_mac);
    const uint8_t *payload = p + header_len; size_t payload_len = total - header_len;
    if (p[9] == 1) icmp_receive(dev, source, destination, payload, payload_len);
    else if (p[9] == 6) tcp_receive(dev, source, destination, payload, payload_len);
    else if (p[9] == 17) udp_receive(dev, source, destination, payload, payload_len);
}

int ipv4_send(netdev_t *dev, uint32_t destination, uint8_t protocol, const void *payload, size_t length)
{
    if (!dev || !payload || length + IPV4_HEADER_SIZE > dev->mtu) return -1;
    uint8_t destination_mac[6];
    if (destination == IPV4_BROADCAST || (config.configured && destination == (config.address | ~config.netmask)))
        memset(destination_mac, 0xFF, 6);
    else {
        uint32_t next_hop = destination;
        if (config.configured && config.netmask &&
            (destination & config.netmask) != (config.address & config.netmask))
            next_hop = config.gateway;
        if (!next_hop || !arp_lookup(next_hop, destination_mac)) { if (next_hop) arp_request(dev, next_hop); return -2; }
    }
    uint8_t *p = packet_buffer; memset(p, 0, IPV4_HEADER_SIZE);
    p[0] = 0x45; net_write16(p + 2, (uint16_t)(length + IPV4_HEADER_SIZE));
    net_write16(p + 4, identification++); net_write16(p + 6, 0x4000); p[8] = 64; p[9] = protocol;
    net_write32(p + 12, config.address); net_write32(p + 16, destination);
    net_write16(p + 10, net_checksum_finish(net_checksum_add(0, p, IPV4_HEADER_SIZE)));
    memcpy(p + IPV4_HEADER_SIZE, payload, length);
    return ethernet_send(dev, destination_mac, ETH_TYPE_IPV4, p, IPV4_HEADER_SIZE + length);
}
