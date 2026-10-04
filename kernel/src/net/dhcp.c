#include "dhcp.h"
#include "netdev.h"
#include "udp.h"
#include "ipv4.h"
#include "util.h"
#include "../memory.h"
#include "../poll.h"
#include "../serial.h"
#include "../clock.h"

#define CLIENT_PORT 68
#define SERVER_PORT 67
#define MAGIC 0x63825363u
#define RETRY_MS 1000u
enum { IDLE, SELECTING, REQUESTING, BOUND, FAILED };

typedef struct {
    int state, attempts;
    uint32_t xid, offered_ip, server, netmask, gateway, dns;
    uint64_t retry_at;
    netdev_t *dev;
} client_t;
static client_t client;
static uint8_t message[300];

static uint8_t *option(uint8_t *p, uint8_t type, const void *data, uint8_t length)
{
    *p++ = type; *p++ = length; memcpy(p, data, length); return p + length;
}

static int send_message(uint8_t type)
{
    memset(message, 0, sizeof(message));
    message[0] = 1; message[1] = 1; message[2] = 6;
    net_write32(message + 4, client.xid); net_write16(message + 10, 0x8000);
    memcpy(message + 28, client.dev->mac, 6); net_write32(message + 236, MAGIC);
    uint8_t *o = option(message + 240, 53, &type, 1);
    uint8_t client_id[7] = {1}; memcpy(client_id + 1, client.dev->mac, 6);
    o = option(o, 61, client_id, sizeof(client_id));
    if (type == 3) {
        uint8_t ip[4], server[4]; net_write32(ip, client.offered_ip); net_write32(server, client.server);
        o = option(o, 50, ip, 4); o = option(o, 54, server, 4);
    }
    uint8_t requested[] = {1,3,6,51,54}; o = option(o, 55, requested, sizeof(requested));
    *o = 255;
    serial_print(type == 1 ? "[dhcp] sending DISCOVER\n" : "[dhcp] sending REQUEST\n");
    return udp_send(client.dev, IPV4_BROADCAST, CLIENT_PORT, SERVER_PORT, message, sizeof(message));
}

static int begin(void)
{
    client.dev = netdev_default();
    if (!client.dev) return -1;
    if (!client.dev->link_up) return -2;
    client.xid = 0x4D594F53u;
    for (int i = 0; i < 6; i++) client.xid = (client.xid << 5) ^ (client.xid >> 27) ^ client.dev->mac[i];
    client.attempts = 1; client.retry_at = clock_milliseconds() + RETRY_MS; client.state = SELECTING;
    ipv4_set_config(0, 0, 0, 0);
    return send_message(1);
}

static uint8_t parse_options(const uint8_t *p, size_t length)
{
    uint8_t type = 0;
    for (size_t i = 240; i < length;) {
        uint8_t code = p[i++]; if (code == 255) break; if (code == 0) continue;
        if (i >= length) break; uint8_t n = p[i++]; if ((size_t)n > length - i) break;
        if (code == 53 && n == 1) type = p[i];
        else if (code == 54 && n == 4) client.server = net_read32(p + i);
        else if (code == 1 && n == 4) client.netmask = net_read32(p + i);
        else if (code == 3 && n >= 4) client.gateway = net_read32(p + i);
        else if (code == 6 && n >= 4) client.dns = net_read32(p + i);
        i += n;
    }
    return type;
}

static void receive(netdev_t *dev, uint32_t source_ip, uint16_t source_port,
                    const uint8_t *p, size_t length)
{
    (void)source_ip;
    if (dev != client.dev || source_port != SERVER_PORT || length < 240 || p[0] != 2 ||
        p[1] != 1 || p[2] != 6 || net_read32(p + 4) != client.xid ||
        memcmp(p + 28, dev->mac, 6) != 0 || net_read32(p + 236) != MAGIC) return;
    uint8_t type = parse_options(p, length);
    if (client.state == SELECTING && type == 2) {
        client.offered_ip = net_read32(p + 16);
        if (!client.offered_ip || !client.server) return;
        serial_print("[dhcp] offer "); net_print_ipv4(client.offered_ip); serial_print("\n");
        client.state = REQUESTING; client.retry_at = clock_milliseconds() + RETRY_MS; send_message(3);
    } else if (client.state == REQUESTING && type == 5) {
        uint32_t address = net_read32(p + 16); if (!address) address = client.offered_ip;
        if (!client.netmask) client.netmask = 0xFFFFFF00u;
        ipv4_set_config(address, client.netmask, client.gateway, client.dns); client.state = BOUND;
        serial_print("[dhcp] bound address="); net_print_ipv4(address);
        serial_print(" mask="); net_print_ipv4(client.netmask);
        serial_print(" gateway="); net_print_ipv4(client.gateway);
        serial_print(" dns="); net_print_ipv4(client.dns); serial_print("\n");
    } else if (type == 6) {
        serial_print("[dhcp] server sent NAK\n"); client.state = IDLE;
    }
}

static void poll_client(void *context)
{
    (void)context;
    if (client.state == IDLE) { (void)begin(); return; }
    if (client.state == BOUND || client.state == FAILED) return;
    if (clock_milliseconds() < client.retry_at) return;
    client.retry_at = clock_milliseconds() + RETRY_MS;
    if (++client.attempts > 4) { client.state = FAILED; serial_print("[dhcp] no reply after 4 attempts\n"); return; }
    send_message(client.state == SELECTING ? 1 : 3);
}

void dhcp_init(void)
{
    memset(&client, 0, sizeof(client));
    if (udp_bind(CLIENT_PORT, receive) != 0 || poll_register(poll_client, 0) != 0)
        serial_print("[dhcp] ERROR: initialization failed\n");
}
int dhcp_restart(void)
{
    memset(&client, 0, sizeof(client));
    return begin();
}
const char *dhcp_state_name(void)
{
    static const char *names[] = {"idle","selecting","requesting","bound","failed"};
    return names[client.state];
}
