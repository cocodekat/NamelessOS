#include "net.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "dhcp.h"
#include "tcp.h"
#include "http_test.h"
#include "wifi.h"
#include "util.h"
#include "../serial.h"
#include "../clock.h"

void net_init(void)
{
    clock_init();
    wifi_init();
    arp_init();
    ipv4_init();
    ethernet_init();
    tcp_init();
    http_test_init();
    dhcp_init();
}

void net_print_status(void)
{
    netdev_t *dev = netdev_default();
    if (!dev) { serial_print("network: no device\n"); return; }
    serial_print("device="); serial_print(dev->name); serial_print(" link=");
    serial_print(dev->link_up ? "up" : "down"); serial_print(" dhcp=");
    serial_print(dhcp_state_name()); serial_print("\n");
    const ipv4_config_t *c = ipv4_get_config();
    serial_print("address="); net_print_ipv4(c->address); serial_print(" mask="); net_print_ipv4(c->netmask);
    serial_print(" gateway="); net_print_ipv4(c->gateway); serial_print(" dns="); net_print_ipv4(c->dns); serial_print("\n");
}

int net_restart_dhcp(void) { return dhcp_restart(); }
