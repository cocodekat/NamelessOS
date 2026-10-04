#ifndef RTL8168_H
#define RTL8168_H

#include <stdint.h>

int rtl8168_init(void);
/* Prints hardware/ring counters to the existing serial console. */
void rtl8168_print_diagnostics(void);

#endif
