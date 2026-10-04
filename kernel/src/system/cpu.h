#ifndef CPU_H
#define CPU_H

#include <stdint.h>

typedef struct
{
    char vendor[13];
    char brand[49];
    uint32_t logical_cpus;
} cpu_info_t;

void cpu_get_info(cpu_info_t *info);

#endif