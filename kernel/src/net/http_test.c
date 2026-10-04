#include "http_test.h"
#include "tcp.h"
#include "util.h"
#include "../memory.h"
#include "../serial.h"

#define HTTP_PORT 8080

static size_t find_header_end(const uint8_t *request, size_t length)
{
    for (size_t i = 0; i + 3 < length; i++)
        if (request[i] == '\r' && request[i + 1] == '\n' &&
            request[i + 2] == '\r' && request[i + 3] == '\n')
            return i + 4;
    return 0;
}

static size_t content_length(const uint8_t *request, size_t header_length)
{
    static const char name[] = "Content-Length:";
    for (size_t i = 0; i + sizeof(name) - 1 < header_length; i++)
    {
        size_t j = 0;
        while (j < sizeof(name) - 1 && request[i + j] == (uint8_t)name[j])
            j++;
        if (j != sizeof(name) - 1)
            continue;
        size_t p = i + j;
        while (p < header_length && (request[p] == ' ' || request[p] == '\t'))
            p++;
        size_t value = 0;
        while (p < header_length && request[p] >= '0' && request[p] <= '9')
        {
            if (value > 100000u)
                return 100001u;
            value = value * 10u + (size_t)(request[p++] - '0');
        }
        return value;
    }
    return 0;
}

static size_t handle_http(netdev_t *dev, uint32_t source_ip,
                          uint16_t source_port, const uint8_t *request, size_t request_length,
                          uint8_t *response, size_t response_capacity)
{
    (void)dev;
    size_t header_length = find_header_end(request, request_length);
    if (!header_length)
        return 0;
    size_t body_length = content_length(request, header_length);
    if (body_length > request_length - header_length)
        return 0;

    serial_print("[http] POST from ");
    net_print_ipv4(source_ip);
    serial_putc(':');
    serial_print_uint(source_port);
    serial_print(" message=\"");
    size_t shown = body_length > 1024u ? 1024u : body_length;
    for (size_t i = 0; i < shown; i++)
    {
        uint8_t c = request[header_length + i];
        serial_putc((c >= 32 && c <= 126) || c == '\n' ? (char)c : '.');
    }
    if (shown != body_length)
        serial_print("...");
    serial_print("\"\n");

    static const char reply[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 17\r\n"
        "Connection: close\r\n"
        "\r\n"
        "myOS received it\n";
    if (sizeof(reply) - 1 > response_capacity)
        return 0;
    memcpy(response, reply, sizeof(reply) - 1);
    return sizeof(reply) - 1;
}

void http_test_init(void)
{
    if (tcp_listen(HTTP_PORT, handle_http) == 0)
        serial_print("[http] test server listening on TCP port 8080\n");
    else
        serial_print("[http] ERROR: could not listen on TCP port 8080\n");
}
