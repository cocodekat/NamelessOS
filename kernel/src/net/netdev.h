#ifndef NET_NETDEV_H
#define NET_NETDEV_H

#include <stddef.h>
#include <stdint.h>

#define NETDEV_MAX_DEVICES 4
#define NETDEV_ETH_ADDR_LEN 6
#define NETDEV_DEFAULT_MTU 1500

typedef struct netdev netdev_t;
typedef struct {
    int (*transmit)(netdev_t *dev, const void *frame, size_t length);
    void (*poll)(netdev_t *dev);
    int (*get_link)(netdev_t *dev);
} netdev_ops_t;

struct netdev {
    const char *name;
    uint8_t mac[NETDEV_ETH_ADDR_LEN];
    uint16_t mtu;
    int link_up;
    void *driver_data;
    const netdev_ops_t *ops;
};

typedef void (*netdev_rx_fn)(netdev_t *dev, const uint8_t *frame, size_t length);
int netdev_register(netdev_t *dev);
void netdev_set_rx_handler(netdev_rx_fn handler);
void netdev_receive(netdev_t *dev, const void *frame, size_t length);
int netdev_transmit(netdev_t *dev, const void *frame, size_t length);
void netdev_poll_all(void);
netdev_t *netdev_default(void);
int netdev_count(void);
netdev_t *netdev_get(int index);

#endif
