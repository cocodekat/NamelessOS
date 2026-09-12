// keyboard.c
#include <stdint.h>
#include "keyboard.h"
#include "xhci.h"

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    asm volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

// Scancode Set 1 (the legacy set every PS/2 controller — real or emulated
// — falls back to). Index = make code. 0 = no mapping (modifier/F-keys,
// ignored for now).
static const char scancode_to_ascii[128] = {
    0,   27,  '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ', 0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',
    '2','3','0','.', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

static const char scancode_to_ascii_shift[128] = {
    0,   27,  '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ', 0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',
    '2','3','0','.', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

#define SC_LSHIFT      0x2A
#define SC_RSHIFT      0x36
#define SC_LSHIFT_REL  0xAA
#define SC_RSHIFT_REL  0xB6
#define SC_LCTRL       0x1D
#define SC_LCTRL_REL   0x9D
#define SC_EXTENDED    0xE0

static int shift_held = 0;
static int ctrl_held = 0;

// Holds pending bytes for multi-byte returns (arrow-key escape sequences),
// drained one at a time by subsequent kb_getc() calls.
static char pending[4];
static int pending_len = 0;
static int pending_pos = 0;

static int ps2_has_scancode(void) {
    return inb(PS2_STATUS_PORT) & 1;
}

static uint8_t ps2_read_scancode(void) {
    return inb(PS2_DATA_PORT);
}

// Non-blocking: returns the translated byte from one PS/2 scancode, or -1
// if the code was a modifier/release/unmapped key that doesn't produce a
// byte on its own (caller should just loop back around in that case).
// Assumes ps2_has_scancode() was already true.
static int ps2_consume_one(void) {
    uint8_t code = ps2_read_scancode();

    if (code == SC_EXTENDED) {
        // Extended codes are two bytes; block briefly for the second one
        // since it always follows immediately (real hardware/QEMU both
        // send it back-to-back without a meaningful gap).
        while (!ps2_has_scancode()) { }
        uint8_t code2 = ps2_read_scancode();
        switch (code2) {
            case 0x48: pending[0]=0x1B; pending[1]='['; pending[2]='A'; break; // up
            case 0x50: pending[0]=0x1B; pending[1]='['; pending[2]='B'; break; // down
            case 0x4D: pending[0]=0x1B; pending[1]='['; pending[2]='C'; break; // right
            case 0x4B: pending[0]=0x1B; pending[1]='['; pending[2]='D'; break; // left
            default: return -1;
        }
        pending_len = 3;
        pending_pos = 1;
        return (unsigned char)pending[0];
    }

    if (code == SC_LSHIFT || code == SC_RSHIFT) { shift_held = 1; return -1; }
    if (code == SC_LSHIFT_REL || code == SC_RSHIFT_REL) { shift_held = 0; return -1; }
    if (code == SC_LCTRL) { ctrl_held = 1; return -1; }
    if (code == SC_LCTRL_REL) { ctrl_held = 0; return -1; }

    if (code & 0x80) return -1; // other key releases: ignore

    char ascii = shift_held ? scancode_to_ascii_shift[code] : scancode_to_ascii[code];
    if (ascii == 0) return -1; // unmapped key

    if (ctrl_held && ascii >= 'a' && ascii <= 'z') return (unsigned char)(ascii - 'a' + 1);
    if (ctrl_held && ascii >= 'A' && ascii <= 'Z') return (unsigned char)(ascii - 'A' + 1);

    return (unsigned char)ascii;
}

char kb_getc(void) {
    if (pending_pos < pending_len) {
        return pending[pending_pos++];
    }

    for (;;) {
        if (ps2_has_scancode()) {
            int c = ps2_consume_one();
            if (c >= 0) return (char)c;
            continue; // modifier/release/unmapped: no byte produced, poll again
        }

        int uc = xhci_poll_key();
        if (uc >= 0) return (char)uc;

        asm volatile ("pause");
    }
}