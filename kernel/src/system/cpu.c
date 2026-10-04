#include "cpu.h"

static void cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t *eax,
    uint32_t *ebx,
    uint32_t *ecx,
    uint32_t *edx)
{
    asm volatile(
        "cpuid"
        : "=a"(*eax),
          "=b"(*ebx),
          "=c"(*ecx),
          "=d"(*edx)
        : "a"(leaf),
          "c"(subleaf));
}

void cpu_get_info(cpu_info_t *info)
{
    uint32_t eax, ebx, ecx, edx;

    /*
     * CPUID leaf 0:
     * EBX, EDX, ECX contain the CPU vendor string.
     */
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);

    *(uint32_t *)&info->vendor[0] = ebx;
    *(uint32_t *)&info->vendor[4] = edx;
    *(uint32_t *)&info->vendor[8] = ecx;
    info->vendor[12] = '\0';

    /*
     * CPUID extended leaves:
     * 0x80000002 - 0x80000004 contain the CPU brand string.
     */
    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);

    if (eax >= 0x80000004)
    {
        uint32_t *brand = (uint32_t *)info->brand;

        cpuid(0x80000002, 0,
              &brand[0], &brand[1], &brand[2], &brand[3]);

        cpuid(0x80000003, 0,
              &brand[4], &brand[5], &brand[6], &brand[7]);

        cpuid(0x80000004, 0,
              &brand[8], &brand[9], &brand[10], &brand[11]);

        info->brand[48] = '\0';
    }
    else
    {
        info->brand[0] = '\0';
    }

    /*
     * CPUID leaf 1:
     * EBX[23:16] contains the number of logical processors
     * in the package (for the traditional CPUID interface).
     */
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);

    info->logical_cpus = (ebx >> 16) & 0xFF;

    if (info->logical_cpus == 0)
        info->logical_cpus = 1;
}