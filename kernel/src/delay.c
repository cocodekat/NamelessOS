#include "delay.h"
#include "io.h"

#define PIT_FREQUENCY_HZ 1193182u

static void pit_wait_ticks(uint16_t ticks)
{
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)(p61 & ~0x02u));
    outb(0x43, 0xB0);
    outb(0x42, (uint8_t)ticks);
    outb(0x42, (uint8_t)(ticks >> 8));
    uint8_t gate = (uint8_t)(inb(0x61) & ~0x01u);
    outb(0x61, gate);
    outb(0x61, (uint8_t)(gate | 0x01u));
    while (!(inb(0x61) & 0x20u))
        asm volatile ("pause");
}

void delay_ms(uint32_t milliseconds)
{
    while (milliseconds != 0)
    {
        uint32_t chunk = milliseconds > 50 ? 50 : milliseconds;
        pit_wait_ticks((uint16_t)((PIT_FREQUENCY_HZ / 1000u) * chunk));
        milliseconds -= chunk;
    }
}
