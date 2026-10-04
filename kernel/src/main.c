#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

#include "serial.h"
#include "commands.h"
#include "fs.h"
#include "fb.h"
#include "mm.h"
#include "iommu.h"
#include "drivers/xhci.h"
#include "drivers/rtl8168.h"
#include "drivers/e1000.h"
#include "drivers/wifi/iwlwifi.h"
#include "helpers.h"
#include "net/net.h"
#include "net/wifi.h"
#include "acpi.h"
#include "games/games.h"

// Set the base revision to 6, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

__attribute__((used, section(".limine_requests"))) static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.

__attribute__((used, section(".limine_requests"))) volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0};

__attribute__((used, section(".limine_requests"))) volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0};

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.

__attribute__((used, section(".limine_requests_start"))) static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

__attribute__((used, section(".limine_requests"))) volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0};

uint64_t sysinfo_get_usable_ram(void)
{
    if (memmap_request.response == NULL)
        return 0;

    uint64_t total = 0;

    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++)
    {
        struct limine_memmap_entry *entry =
            memmap_request.response->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE)
            total += entry->length;
    }

    return total;
}

// Halt and catch fire function.
static void hcf(void)
{
    for (;;)
    {
        asm("hlt");
    }
}

// The following will be our kernel's entry point.
// If renaming kmain() to something else, make sure to change the
// linker script accordingly.
void kmain(void)
{
    serial_init();
    serial_clear();
    fb_console_init();

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false)
    {
        hcf();
    }
    mm_init(); // must run before phys_to_virt/virt_to_phys are used
    iommu_disable_firmware_dma_protection();
    acpi_init();
    xhci_init();    // initialize xHCI
    net_init();     // protocol stack must be ready before a NIC can receive
    rtl8168_init(); // initialize rtl8168 network driver
    e1000_init();   // optional Intel/QEMU NIC behind the same netdev interface
    iwlwifi_init(); // Intel 22000-family Wi-Fi transport (staged bring-up)

    if (module_request.response == NULL ||
        module_request.response->module_count < 1)
    {

        serial_print("No modules loaded!\n");
    }
    else
    {
        struct limine_file *file =
            module_request.response->modules[0];

        serial_print("Loaded module: ");
        serial_print(file->path);
        serial_print("\n");

        fs_init(file->address, file->size);
    }

    char buf[128];

    for (;;)
    {
        serial_print("> ");

        kb_readline(buf, sizeof(buf));

        char *args = buf;

        while (*args && *args != ' ')
            args++;

        if (*args == ' ')
        {
            *args = '\0';
            args++;
        }

        if (buf[0] != '\0')
        {
            if (streq(buf, "netstat"))
            {
                net_print_status();
                rtl8168_print_diagnostics();
            }
            else if (streq(buf, "wifistat"))
            {
                wifi_print_status();
                iwlwifi_print_diagnostics();
            }
            else if (streq(buf, "wifiprep"))
            {
                int result = iwlwifi_prepare_transport();
                if (result != 0)
                {
                    serial_print("Wi-Fi transport preparation failed, code=");
                    serial_print_uint((unsigned)(-result));
                    serial_print(". Run wifistat for details.\n");
                }
            }
            else if (streq(buf, "wifiqueues"))
            {
                int result = iwlwifi_prepare_queues();
                if (result != 0)
                {
                    serial_print("Wi-Fi queue preparation failed, code=");
                    serial_print_uint((unsigned)(-result));
                    serial_print(". Run wifistat for details.\n");
                }
            }
            else if (streq(buf, "dhcp") || streq(buf, "dhcpretry"))
            {
                int result = net_restart_dhcp();
                if (result == 0)
                    serial_print("DHCP retry queued immediately; run netstat to inspect TX/RX.\n");
                else
                {
                    serial_print("DHCP retry could not be queued, netdev error=");
                    serial_print_uint((unsigned)(-result));
                    serial_print(".\n");
                }
            }
            else if (streq(buf, "games"))
            {
                games_command(args);
                return;
            }
            else if (streq(buf, "poweroff") || streq(buf, "shutdown"))
            {
                acpi_poweroff();
                // if we get here, it failed — acpi_poweroff() already
                // printed a diagnostic over serial
            }
            else
                cmd_execute(buf, args);
        }
    }
}
