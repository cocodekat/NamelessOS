#include "util.h"
#include "../serial.h"

uint32_t net_checksum_add(uint32_t sum, const void *data, size_t length)
{
    const uint8_t *p = data;
    while (length >= 2) { sum += ((uint16_t)p[0] << 8) | p[1]; p += 2; length -= 2; }
    if (length) sum += (uint16_t)p[0] << 8;
    return sum;
}

uint16_t net_checksum_finish(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

void net_print_ipv4(uint32_t a)
{
    serial_print_uint(a >> 24); serial_putc('.'); serial_print_uint((a >> 16) & 255);
    serial_putc('.'); serial_print_uint((a >> 8) & 255); serial_putc('.'); serial_print_uint(a & 255);
}
