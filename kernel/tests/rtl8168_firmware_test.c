#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/rtl8168_firmware.h"

static uint16_t mock_registers[32768];
static uint16_t mock_ocp_base = 0xa400;
static unsigned reads;
static unsigned writes;

void delay_ms(uint32_t milliseconds)
{
    (void)milliseconds;
}

static int mock_target(uint32_t target, uint16_t *index)
{
    if (target > 0xffff || (target & 1)) return -1;
    *index = (uint16_t)(target / 2);
    return 0;
}

static int phy_read(void *context, uint16_t reg, uint16_t *value)
{
    (void)context;
    if (reg == 0x1f) {
        *value = mock_ocp_base == 0xa400 ? 0 : mock_ocp_base >> 4;
        return 0;
    }
    if (mock_ocp_base != 0xa400) {
        if (reg < 0x10) return -1;
        reg -= 0x10;
    }
    uint16_t index;
    if (mock_target((uint32_t)mock_ocp_base + reg * 2u, &index) != 0)
        return -1;
    *value = mock_registers[index];
    reads++;
    return 0;
}

static int phy_write(void *context, uint16_t reg, uint16_t value)
{
    (void)context;
    if (reg == 0x1f) {
        mock_ocp_base = value ? (uint16_t)(value << 4) : 0xa400;
        return 0;
    }
    if (mock_ocp_base != 0xa400) {
        if (reg < 0x10) return -1;
        reg -= 0x10;
    }
    uint16_t index;
    if (mock_target((uint32_t)mock_ocp_base + reg * 2u, &index) != 0)
        return -1;
    mock_registers[index] = value;
    writes++;
    return 0;
}

static int mac_read(void *context, uint16_t reg, uint16_t *value)
{
    (void)context;
    uint16_t index;
    if (mock_target((uint32_t)mock_ocp_base + reg, &index) != 0) return -1;
    *value = mock_registers[index];
    reads++;
    return 0;
}

static int mac_write(void *context, uint16_t reg, uint16_t value)
{
    (void)context;
    if (reg == 0x1f) {
        mock_ocp_base = (uint16_t)(value << 4);
        return 0;
    }
    uint16_t index;
    if (mock_target((uint32_t)mock_ocp_base + reg, &index) != 0) return -1;
    mock_registers[index] = value;
    writes++;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    if (fseek(file, 0, SEEK_END) != 0) return 2;
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) return 2;
    uint8_t *image = malloc((size_t)length);
    if (!image || fread(image, 1, (size_t)length, file) != (size_t)length)
        return 2;
    fclose(file);

    rtl8168_fw_bus_t phy = {NULL, phy_read, phy_write};
    rtl8168_fw_bus_t mac = {NULL, mac_read, mac_write};
    char version[33];
    int result = rtl8168_fw_apply(image, (size_t)length, &phy, &mac, version);
    if (result != 0 || strcmp(version, "rtl8168h-2_0.0.2 02/26/15") != 0 ||
        writes == 0) {
        fprintf(stderr, "apply failed: result=%d version=%s reads=%u writes=%u\n",
                result, version, reads, writes);
        return 1;
    }

    image[0] ^= 1;
    if (rtl8168_fw_apply(image, (size_t)length, &phy, &mac, version) != -2) {
        fprintf(stderr, "corrupt firmware was accepted\n");
        return 1;
    }
    free(image);
    printf("firmware test passed: %s, reads=%u writes=%u\n",
           version, reads, writes);
    return 0;
}
