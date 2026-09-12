#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

#include "serial.h"
#include "commands.h"
#include "fs.h"
#include "fb.h"
#include "mm.h"
#include "drivers/xhci.h"
#include "drivers/rtl8168.h"

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

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.

__attribute__((used, section(".limine_requests_start"))) static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

__attribute__((used, section(".limine_requests"))) volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0};

// Halt and catch fire function.
static void hcf(void)
{
    for (;;)
    {
        asm("hlt");
    }
}

static int streq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
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
    mm_init();      // must run before phys_to_virt/virt_to_phys are used
    xhci_init();    // initialize xHCI
    rtl8168_init(); // initialize rtl8168 network driver

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

        if (buf[0] == '\0')
        {
            // empty line
        }
        else if (streq(buf, "help"))
        {
            cmd_help();
        }
        else if (streq(buf, "echo"))
        {
            cmd_echo(args);
        }
        else if (streq(buf, "clear"))
        {
            serial_clear();
        }
        else if (streq(buf, "ls"))
        {
            cmd_ls();
        }
        else if (streq(buf, "cat"))
        {
            cmd_cat(args);
        }
        else if (streq(buf, "pwd"))
        {
            cmd_pwd();
        }
        else if (streq(buf, "cd"))
        {
            cmd_cd(args);
        }
        else if (streq(buf, "mkdir"))
        {
            cmd_mkdir(args);
        }
        else if (streq(buf, "mkfile"))
        {
            cmd_mkfile(args);
        }
        else if (streq(buf, "rm"))
        {
            cmd_rm(args);
        }
        else if (streq(buf, "write"))
        {
            cmd_write(args);
        }
        else if (streq(buf, "sync"))
        {
            cmd_sync();
        }
        else if (streq(buf, "edit"))
        {
            cmd_edit(args);
        }
        else
        {
            serial_print("Unknown command: ");
            serial_print(buf);
            serial_print("\n");
        }
    }
}
