#ifndef NET_WIFI_H
#define NET_WIFI_H

#include <stdint.h>

#define WIFI_MAX_DEVICES 4

typedef enum {
    WIFI_STATE_DETECTED,
    WIFI_STATE_NEEDS_FIRMWARE,
    WIFI_STATE_FIRMWARE_READY,
    WIFI_STATE_DMA_READY,
    WIFI_STATE_QUEUES_READY,
    WIFI_STATE_SCANNING,
    WIFI_STATE_ASSOCIATED,
    WIFI_STATE_ERROR
} wifi_state_t;

typedef struct wifi_device wifi_device_t;

typedef struct {
    int (*load_firmware)(wifi_device_t *dev, const void *image, uint64_t size);
    int (*scan)(wifi_device_t *dev);
    int (*connect)(wifi_device_t *dev, const char *ssid, const char *password);
    void (*poll)(wifi_device_t *dev);
} wifi_device_ops_t;

struct wifi_device {
    const char *name;
    const char *hardware_name;
    const char *firmware_name;
    wifi_state_t state;
    uint8_t mac[6];
    void *driver_data;
    const wifi_device_ops_t *ops;
};

void wifi_init(void);
int wifi_register(wifi_device_t *device);
wifi_device_t *wifi_default(void);
const char *wifi_state_name(wifi_state_t state);
void wifi_print_status(void);

#endif
