#include "tcp.h"
#include "ipv4.h"
#include "util.h"
#include "../memory.h"

#define TCP_HEADER_SIZE 20u
#define TCP_MAX_PAYLOAD (NETDEV_DEFAULT_MTU - 20u - TCP_HEADER_SIZE)
#define TCP_LISTENERS 4
#define TCP_REQUEST_CAPACITY 2048u
#define TCP_RESPONSE_CAPACITY 1024u

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

typedef struct {
    uint16_t port;
    tcp_stream_handler_t handler;
} tcp_listener_t;

typedef enum {
    TCP_CLOSED,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_LAST_ACK
} tcp_state_t;

typedef struct {
    tcp_state_t state;
    netdev_t *dev;
    tcp_listener_t *listener;
    uint32_t peer_ip;
    uint16_t peer_port;
    uint32_t receive_next;
    uint32_t send_initial;
    uint32_t send_next;
    size_t request_length;
    uint8_t request[TCP_REQUEST_CAPACITY];
    size_t response_length;
    uint32_t response_sequence;
    uint8_t response[TCP_RESPONSE_CAPACITY];
} tcp_connection_t;

static tcp_listener_t listeners[TCP_LISTENERS];
static tcp_connection_t connection;
static uint32_t next_sequence;
static uint8_t segment[NETDEV_DEFAULT_MTU - 20u];

static uint32_t checksum_pseudo(uint32_t source, uint32_t destination,
                                const uint8_t *tcp, size_t length)
{
    uint8_t pseudo[12];
    net_write32(pseudo, source);
    net_write32(pseudo + 4, destination);
    pseudo[8] = 0;
    pseudo[9] = 6;
    net_write16(pseudo + 10, (uint16_t)length);
    uint32_t sum = net_checksum_add(0, pseudo, sizeof(pseudo));
    return net_checksum_add(sum, tcp, length);
}

static int send_segment(tcp_connection_t *c, uint8_t flags, uint32_t sequence,
                        const void *payload, size_t length)
{
    if (!c->dev || !c->listener || length > TCP_MAX_PAYLOAD)
        return -1;
    memset(segment, 0, TCP_HEADER_SIZE);
    net_write16(segment, c->listener->port);
    net_write16(segment + 2, c->peer_port);
    net_write32(segment + 4, sequence);
    net_write32(segment + 8, c->receive_next);
    segment[12] = 5u << 4;
    segment[13] = flags;
    net_write16(segment + 14, 4096);
    if (length)
        memcpy(segment + TCP_HEADER_SIZE, payload, length);
    uint32_t local_ip = ipv4_get_config()->address;
    uint16_t checksum = net_checksum_finish(checksum_pseudo(local_ip,
        c->peer_ip, segment, TCP_HEADER_SIZE + length));
    net_write16(segment + 16, checksum);
    return ipv4_send(c->dev, c->peer_ip, 6, segment, TCP_HEADER_SIZE + length);
}

static void close_connection(void)
{
    memset(&connection, 0, sizeof(connection));
}

void tcp_init(void)
{
    memset(listeners, 0, sizeof(listeners));
    close_connection();
    next_sequence = 0x6d794f53u;
}

int tcp_listen(uint16_t port, tcp_stream_handler_t handler)
{
    if (!port || !handler)
        return -1;
    for (unsigned i = 0; i < TCP_LISTENERS; i++) {
        if (listeners[i].port == port || listeners[i].port == 0) {
            listeners[i].port = port;
            listeners[i].handler = handler;
            return 0;
        }
    }
    return -1;
}

static tcp_listener_t *find_listener(uint16_t port)
{
    for (unsigned i = 0; i < TCP_LISTENERS; i++)
        if (listeners[i].port == port)
            return &listeners[i];
    return NULL;
}

static int matches_connection(uint32_t source, uint16_t source_port,
                              uint16_t destination_port)
{
    return connection.state != TCP_CLOSED && connection.peer_ip == source &&
           connection.peer_port == source_port && connection.listener &&
           connection.listener->port == destination_port;
}

void tcp_receive(netdev_t *dev, uint32_t source, uint32_t destination,
                 const uint8_t *p, size_t length)
{
    if (length < TCP_HEADER_SIZE)
        return;
    size_t header_length = (size_t)(p[12] >> 4) * 4u;
    if (header_length < TCP_HEADER_SIZE || header_length > length)
        return;
    if (net_checksum_finish(checksum_pseudo(source, destination, p, length)) != 0)
        return;

    uint16_t source_port = net_read16(p);
    uint16_t destination_port = net_read16(p + 2);
    uint32_t sequence = net_read32(p + 4);
    uint32_t acknowledgement = net_read32(p + 8);
    uint8_t flags = p[13];
    const uint8_t *payload = p + header_length;
    size_t payload_length = length - header_length;

    if (flags & TCP_RST) {
        if (matches_connection(source, source_port, destination_port))
            close_connection();
        return;
    }

    if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
        tcp_listener_t *listener = find_listener(destination_port);
        if (!listener)
            return;
        if (!matches_connection(source, source_port, destination_port)) {
            close_connection();
            connection.state = TCP_SYN_RECEIVED;
            connection.dev = dev;
            connection.listener = listener;
            connection.peer_ip = source;
            connection.peer_port = source_port;
            connection.receive_next = sequence + 1;
            connection.send_initial = next_sequence;
            connection.send_next = next_sequence + 1;
            next_sequence += 0x101u;
        }
        send_segment(&connection, TCP_SYN | TCP_ACK,
                     connection.send_initial, NULL, 0);
        return;
    }

    if (!matches_connection(source, source_port, destination_port))
        return;
    if ((flags & TCP_ACK) && connection.state == TCP_SYN_RECEIVED &&
        acknowledgement == connection.send_next)
        connection.state = TCP_ESTABLISHED;
    if (connection.state == TCP_LAST_ACK) {
        if ((flags & TCP_ACK) && acknowledgement >= connection.send_next)
            close_connection();
        else if (payload_length)
            send_segment(&connection, TCP_PSH | TCP_ACK | TCP_FIN,
                connection.response_sequence, connection.response,
                connection.response_length);
        return;
    }
    if (connection.state != TCP_ESTABLISHED)
        return;

    if (payload_length) {
        if (sequence != connection.receive_next) {
            send_segment(&connection, TCP_ACK, connection.send_next, NULL, 0);
            return;
        }
        if (payload_length > sizeof(connection.request) - connection.request_length) {
            send_segment(&connection, TCP_RST | TCP_ACK,
                         connection.send_next, NULL, 0);
            close_connection();
            return;
        }
        memcpy(connection.request + connection.request_length, payload,
               payload_length);
        connection.request_length += payload_length;
        connection.receive_next += (uint32_t)payload_length;
        send_segment(&connection, TCP_ACK, connection.send_next, NULL, 0);

        size_t response_length = connection.listener->handler(dev, source,
            source_port, connection.request, connection.request_length,
            connection.response, sizeof(connection.response));
        if (response_length > sizeof(connection.response))
            response_length = sizeof(connection.response);
        if (response_length) {
            connection.response_length = response_length;
            connection.response_sequence = connection.send_next;
            send_segment(&connection, TCP_PSH | TCP_ACK | TCP_FIN,
                connection.send_next, connection.response, response_length);
            connection.send_next += (uint32_t)response_length + 1u;
            connection.state = TCP_LAST_ACK;
        }
    }

    if ((flags & TCP_FIN) && connection.state == TCP_ESTABLISHED) {
        connection.receive_next++;
        send_segment(&connection, TCP_ACK | TCP_FIN,
                     connection.send_next, NULL, 0);
        connection.send_next++;
        connection.state = TCP_LAST_ACK;
    }
}
