#include "clock.h"
#include "delay.h"

static uint64_t base_tsc;
static uint64_t cycles_per_ms;

static uint64_t read_tsc(void)
{
    uint32_t low, high;
    asm volatile ("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
}

void clock_init(void)
{
    uint64_t start = read_tsc();
    delay_ms(10);
    uint64_t elapsed = read_tsc() - start;
    cycles_per_ms = elapsed / 10;
    if (!cycles_per_ms) cycles_per_ms = 1;
    base_tsc = read_tsc();
}

uint64_t clock_milliseconds(void)
{
    return (read_tsc() - base_tsc) / cycles_per_ms;
}
