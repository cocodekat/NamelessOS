#ifndef SERIAL_H
#define SERIAL_H
#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_print(const char *s);

// Prints a 32-bit / 64-bit value as zero-padded hex, e.g. "0xDEADBEEF".
// Used heavily by the PCI/xHCI code for debug tracing, since serial output
// is the only feedback channel you have before the keyboard itself works.
void serial_print_hex32(uint32_t value);
void serial_print_hex64(uint64_t value);

int serial_received(void);
char serial_getc(void);
void serial_readline(char *buf, int max_len);

void serial_clear(void);
#endif