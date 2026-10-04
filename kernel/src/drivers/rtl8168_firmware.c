#include "rtl8168_firmware.h"
#include "../delay.h"

#define FW_HEADER_SIZE 45u
#define FW_VERSION_OFFSET 4u
#define FW_VERSION_SIZE 32u
#define FW_START_OFFSET 36u
#define FW_LENGTH_OFFSET 40u
#define FW_STEP_LIMIT 1000000u

enum fw_opcode {
    FW_READ = 0x0,
    FW_DATA_OR = 0x1,
    FW_DATA_AND = 0x2,
    FW_BACK = 0x3,
    FW_BUS_SELECT = 0x4,
    FW_CLEAR_READ_COUNT = 0x7,
    FW_WRITE = 0x8,
    FW_READ_COUNT_SKIP = 0x9,
    FW_EQUAL_SKIP = 0xa,
    FW_NOT_EQUAL_SKIP = 0xb,
    FW_WRITE_PREVIOUS = 0xc,
    FW_SKIP = 0xd,
    FW_DELAY_MS = 0xe,
};

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int validate_actions(const uint8_t *code, size_t count)
{
    uint32_t total_delay = 0;
    for (size_t pc = 0; pc < count; pc++) {
        uint32_t action = load_le32(code + pc * 4);
        uint16_t data = (uint16_t)action;
        size_t distance = (action >> 16) & 0x0fffu;

        switch (action >> 28) {
        case FW_READ:
        case FW_DATA_OR:
        case FW_DATA_AND:
        case FW_CLEAR_READ_COUNT:
        case FW_WRITE:
        case FW_WRITE_PREVIOUS:
            break;
        case FW_DELAY_MS:
            if (data > 1000 || total_delay + data > 10000) return -1;
            total_delay += data;
            break;
        case FW_BUS_SELECT:
            if (data > 1) return -1;
            break;
        case FW_BACK:
            if (distance > pc) return -1;
            break;
        case FW_READ_COUNT_SKIP:
            if (pc + 2 >= count) return -1;
            break;
        case FW_EQUAL_SKIP:
        case FW_NOT_EQUAL_SKIP:
        case FW_SKIP:
            if (distance >= count - pc - 1) return -1;
            break;
        default:
            return -1;
        }
    }
    return 0;
}

int rtl8168_fw_apply(const uint8_t *image, size_t image_size,
                     const rtl8168_fw_bus_t *phy,
                     const rtl8168_fw_bus_t *mac,
                     char version[33])
{
    if (!image || image_size < FW_HEADER_SIZE || !phy || !mac ||
        !phy->read || !phy->write || !mac->read || !mac->write)
        return -1;

    /* This driver accepts the checksummed, header-based firmware format. */
    if (load_le32(image) != 0) return -2;
    uint8_t checksum = 0;
    for (size_t i = 0; i < image_size; i++) checksum += image[i];
    if (checksum != 0) return -3;

    size_t start = load_le32(image + FW_START_OFFSET);
    size_t count = load_le32(image + FW_LENGTH_OFFSET);
    if (start > image_size || count > (image_size - start) / 4)
        return -4;

    const uint8_t *code = image + start;
    if (validate_actions(code, count) != 0) return -5;

    if (version) {
        for (size_t i = 0; i < FW_VERSION_SIZE; i++)
            version[i] = (char)image[FW_VERSION_OFFSET + i];
        version[FW_VERSION_SIZE] = '\0';
    }

    const rtl8168_fw_bus_t *bus = phy;
    uint16_t previous = 0;
    uint32_t read_count = 0;
    size_t pc = 0;

    for (size_t steps = 0; pc < count && steps < FW_STEP_LIMIT; steps++) {
        uint32_t action = load_le32(code + pc * 4);
        uint16_t data = (uint16_t)action;
        uint16_t reg = (uint16_t)((action >> 16) & 0x0fffu);
        size_t next = pc + 1;
        int result;

        switch (action >> 28) {
        case FW_READ:
            result = bus->read(bus->context, reg, &previous);
            if (result != 0) return -6;
            read_count++;
            break;
        case FW_DATA_OR:
            previous = (uint16_t)(previous | data);
            break;
        case FW_DATA_AND:
            previous = (uint16_t)(previous & data);
            break;
        case FW_BACK:
            next = pc - reg;
            break;
        case FW_BUS_SELECT:
            bus = data ? mac : phy;
            break;
        case FW_CLEAR_READ_COUNT:
            read_count = 0;
            break;
        case FW_WRITE:
            if (bus->write(bus->context, reg, data) != 0) return -7;
            break;
        case FW_READ_COUNT_SKIP:
            if (read_count == data) next++;
            break;
        case FW_EQUAL_SKIP:
            if (previous == data) next += reg;
            break;
        case FW_NOT_EQUAL_SKIP:
            if (previous != data) next += reg;
            break;
        case FW_WRITE_PREVIOUS:
            if (bus->write(bus->context, reg, previous) != 0) return -8;
            break;
        case FW_SKIP:
            next += reg;
            break;
        case FW_DELAY_MS:
            delay_ms(data);
            break;
        default:
            return -9; /* Already rejected by validate_actions(). */
        }
        pc = next;
    }

    return pc == count ? 0 : -10;
}
