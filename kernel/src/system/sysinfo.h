#ifndef SYSINFO_H
#define SYSINFO_H

#include "../mm.h"

void print_sysinfo(const char *args);

uint64_t sysinfo_get_usable_ram(void);

#endif