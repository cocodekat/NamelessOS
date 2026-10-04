#ifndef RTL8168_FIRMWARE_H
#define RTL8168_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *context;
    int (*read)(void *context, uint16_t reg, uint16_t *value);
    int (*write)(void *context, uint16_t reg, uint16_t value);
} rtl8168_fw_bus_t;

/*
 * Validate and execute a Realtek r8169-family action firmware image.
 * The caller supplies separate PHY and MAC-MCU register buses because the
 * image may switch between them.  Returns zero on success.
 */
int rtl8168_fw_apply(const uint8_t *image, size_t image_size,
                     const rtl8168_fw_bus_t *phy,
                     const rtl8168_fw_bus_t *mac,
                     char version[33]);

extern const uint8_t rtl8168h_2_fw_start[];
extern const uint8_t rtl8168h_2_fw_end[];

#endif
