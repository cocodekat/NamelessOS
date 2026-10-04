#ifndef NET_TCP_H
#define NET_TCP_H

#include <stddef.h>
#include <stdint.h>
#include "netdev.h"

/* Called with all bytes accumulated for the current connection. Return zero
   when more data is needed, or the number of response bytes written. */
typedef size_t (*tcp_stream_handler_t)(netdev_t *dev, uint32_t source_ip,
    uint16_t source_port, const uint8_t *request, size_t request_length,
    uint8_t *response, size_t response_capacity);

void tcp_init(void);
int tcp_listen(uint16_t port, tcp_stream_handler_t handler);
void tcp_receive(netdev_t *dev, uint32_t source, uint32_t destination,
                 const uint8_t *packet, size_t length);

#endif
