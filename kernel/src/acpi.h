#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stdbool.h>

// Must be called after mm_init() (needs phys_to_virt) and after Limine's
// RSDP request response is available.
bool acpi_init(void);

// Powers the machine off via ACPI (_S5 / PM1 control register write).
// Does not return on success. On failure, prints a diagnostic over serial
// and returns so the caller can decide what to do next.
void acpi_poweroff(void);

#endif