#include "wifi.h"
#include "../memory.h"
#include "../serial.h"

static wifi_device_t *devices[WIFI_MAX_DEVICES];
static unsigned device_count;

void wifi_init(void)
{
    memset(devices, 0, sizeof(devices));
    device_count = 0;
}

int wifi_register(wifi_device_t *device)
{
    if (!device || !device->name || device_count == WIFI_MAX_DEVICES)
        return -1;
    devices[device_count++] = device;
    serial_print("[wifi] registered "); serial_print(device->name);
    serial_print(" ("); serial_print(device->hardware_name); serial_print(")\n");
    return 0;
}

wifi_device_t *wifi_default(void)
{
    return device_count ? devices[0] : 0;
}

const char *wifi_state_name(wifi_state_t state)
{
    static const char *names[] = {
        "detected", "needs-firmware", "firmware-ready", "dma-ready",
        "queues-ready", "scanning", "associated", "error"
    };
    return (unsigned)state < sizeof(names) / sizeof(names[0])
        ? names[state] : "unknown";
}

void wifi_print_status(void)
{
    if (!device_count) {
        serial_print("wifi: no supported device detected\n");
        return;
    }
    for (unsigned i = 0; i < device_count; i++) {
        wifi_device_t *dev = devices[i];
        serial_print("wifi: device="); serial_print(dev->name);
        serial_print(" hardware="); serial_print(dev->hardware_name);
        serial_print(" state="); serial_print(wifi_state_name(dev->state));
        serial_print(" firmware="); serial_print(dev->firmware_name);
        serial_print("\n");
    }
}
