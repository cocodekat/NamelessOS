#include "sysinfo.h"
#include "../serial.h"
#include "../helpers.h"
#include "cpu.h"
#include <limine.h>
#include <stdint.h>

void print_sysinfo(const char *args)
{
    cpu_info_t cpu;

    if (strings_equal(args, "-os"))
    {
        serial_print("\n");
        serial_print("OS:           myOS\n");
        serial_print("Architecture: x86_64\n");
        return;
    }

    if (strings_equal(args, "-cpu"))
    {
        cpu_get_info(&cpu);

        serial_print("\n");
        serial_print("CPU Vendor:   ");
        serial_print(cpu.vendor);
        serial_print("\n");

        serial_print("CPU:          ");
        serial_print(cpu.brand);
        serial_print("\n");

        serial_print("Logical CPUs: ");
        serial_print_hex32(cpu.logical_cpus);
        serial_print("\n");

        return;
    }

    if (strings_equal(args, "-ram"))
    {
        serial_print("Usable RAM:          ");
        int ram = byte_conv(sysinfo_get_usable_ram(), "b", "gb");
        serial_print_uint(ram);
        serial_print(" GB\n");
        return;
    }

    /* Full system information */

    cpu_get_info(&cpu);

    serial_print("\n");

    serial_print("███╗   ███╗██╗   ██╗ ██████╗ ███████╗\n");
    serial_print("████╗ ████║╚██╗ ██╔╝██╔═══██╗██╔════╝\n");
    serial_print("██╔████╔██║ ╚████╔╝ ██║   ██║███████╗\n");
    serial_print("██║╚██╔╝██║  ╚██╔╝  ██║   ██║╚════██║\n");
    serial_print("██║ ╚═╝ ██║   ██║   ╚██████╔╝███████║\n");
    serial_print("╚═╝     ╚═╝   ╚═╝    ╚═════╝ ╚══════╝\n");

    serial_print("\n");
    serial_print("myOS System Information\n");
    serial_print("-----------------------\n");

    serial_print("OS:           myOS\n");
    serial_print("Architecture: x86_64\n");

    serial_print("CPU Vendor:   ");
    serial_print(cpu.vendor);
    serial_print("\n");

    serial_print("CPU:          ");
    serial_print(cpu.brand);
    serial_print("\n");

    serial_print("Logical CPUs: ");
    serial_print_hex32(cpu.logical_cpus);
    serial_print("\n");

    serial_print("Usable RAM:          ");
    int ram = byte_conv(sysinfo_get_usable_ram(), "b", "gb");
    serial_print_uint(ram);
    serial_print(" GB\n");

    serial_print("\n");
}