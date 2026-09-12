#include "serial.h"
#include "fb.h"

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);    // Disable interrupts
    outb(COM1 + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(COM1 + 0, 0x01);    // Set divisor low byte (115200 baud)
    outb(COM1 + 1, 0x00);    //         divisor high byte
    outb(COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7);    // Enable FIFO, clear, 14-byte threshold
    outb(COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

static int is_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    while (is_transmit_empty() == 0);
    outb(COM1, c);
    fb_console_putc(c);
}

void serial_print(const char *s) {
    while (*s) {
        serial_putc(*s++);
    }
}

static void print_hex_nibbles(uint64_t value, int nibbles) {
    static const char hex[] = "0123456789ABCDEF";
    serial_print("0x");
    for (int i = nibbles - 1; i >= 0; i--) {
        uint8_t nibble = (uint8_t)((value >> (i * 4)) & 0xF);
        serial_putc(hex[nibble]);
    }
}

void serial_print_hex32(uint32_t value) {
    print_hex_nibbles(value, 8);
}

void serial_print_hex64(uint64_t value) {
    print_hex_nibbles(value, 16);
}

int serial_received(void) {
    return inb(COM1 + 5) & 0x01;
}

char serial_getc(void) {
    while (serial_received() == 0);
    return inb(COM1);
}

void serial_clear(void) {
    serial_print("\x1b[2J\x1b[H");
}

void serial_readline(char *buf, int max_len) {
    int i = 0;
    for (;;) {
        char c = serial_getc();

        if (c == '\r' || c == '\n') {
            serial_print("\n");
            buf[i] = '\0';
            return;
        }

        if ((c == '\b' || c == 0x7F) && i > 0) {
            i--;
            serial_print("\b \b"); // erase last char visually
            continue;
        }

        if (i < max_len - 1) {
            buf[i++] = c;
            serial_putc(c); // echo it back
        }
    }
}