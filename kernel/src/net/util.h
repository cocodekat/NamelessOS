#ifndef NET_UTIL_H
#define NET_UTIL_H
#include <stddef.h>
#include <stdint.h>

static inline uint16_t net_read16(const void *p) { const uint8_t *b = p; return ((uint16_t)b[0] << 8) | b[1]; }
static inline uint32_t net_read32(const void *p) { const uint8_t *b = p; return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }
static inline void net_write16(void *p, uint16_t v) { uint8_t *b = p; b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static inline void net_write32(void *p, uint32_t v) { uint8_t *b = p; b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }

uint32_t net_checksum_add(uint32_t sum, const void *data, size_t length);
uint16_t net_checksum_finish(uint32_t sum);
void net_print_ipv4(uint32_t address);

#endif
