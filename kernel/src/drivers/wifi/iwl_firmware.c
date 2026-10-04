#include "iwl_firmware.h"
#include "../../memory.h"

#define IWL_TLV_HEADER_SIZE 88u
#define IWL_TLV_MAGIC 0x0a4c5749u

enum iwl_tlv_type {
    IWL_TLV_FLAGS = 18,
    IWL_TLV_SEC_RT = 19,
    IWL_TLV_SEC_INIT = 20,
    IWL_TLV_SEC_WOWLAN = 21,
    IWL_TLV_SECURE_SEC_RT = 24,
    IWL_TLV_SECURE_SEC_INIT = 25,
    IWL_TLV_SECURE_SEC_WOWLAN = 26,
    IWL_TLV_NUM_OF_CPU = 27,
    IWL_TLV_API_CHANGES_SET = 29,
    IWL_TLV_ENABLED_CAPABILITIES = 30,
    IWL_TLV_PAGING = 32,
    IWL_TLV_SEC_RT_USNIFFER = 34,
    IWL_TLV_FW_VERSION = 36,
    IWL_TLV_CMD_VERSIONS = 48,
    IWL_TLV_IML = 52
};

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int add_section(iwl_fw_image_t *image, const uint8_t *data,
                       size_t length)
{
    if (length < 4u)
        return IWL_FW_ERR_TLV_LENGTH;
    if (image->section_count == IWL_FW_MAX_SECTIONS)
        return IWL_FW_ERR_TOO_MANY_SECTIONS;
    iwl_fw_section_t *section = &image->sections[image->section_count++];
    section->offset = read_le32(data);
    section->data = data + 4u;
    section->size = length - 4u;
    return IWL_FW_OK;
}

static int validate_fixed_tlv(uint32_t type, size_t length)
{
    switch (type) {
    case IWL_TLV_FLAGS:
    case IWL_TLV_NUM_OF_CPU:
    case IWL_TLV_PAGING:
        return length == 4u ? IWL_FW_OK : IWL_FW_ERR_TLV_LENGTH;
    case IWL_TLV_API_CHANGES_SET:
    case IWL_TLV_ENABLED_CAPABILITIES:
        return length != 0u && (length % 8u) == 0u
            ? IWL_FW_OK : IWL_FW_ERR_TLV_LENGTH;
    case IWL_TLV_FW_VERSION:
        return length == 12u ? IWL_FW_OK : IWL_FW_ERR_TLV_LENGTH;
    case IWL_TLV_CMD_VERSIONS:
        /* Each command version record is group, command, version, notification. */
        return (length % 4u) == 0u ? IWL_FW_OK : IWL_FW_ERR_TLV_LENGTH;
    default:
        return IWL_FW_OK;
    }
}

int iwl_fw_parse(const uint8_t *data, size_t size, uint32_t expected_api,
                 iwl_fw_info_t *info)
{
    if (!data || !info)
        return IWL_FW_ERR_ARGUMENT;
    memset(info, 0, sizeof(*info));
    if (size < IWL_TLV_HEADER_SIZE)
        return IWL_FW_ERR_HEADER;
    if (read_le32(data) != 0u || read_le32(data + 4u) != IWL_TLV_MAGIC)
        return IWL_FW_ERR_MAGIC;

    for (size_t i = 0; i < IWL_FW_HUMAN_READABLE_SIZE; i++) {
        uint8_t c = data[8u + i];
        info->human_readable[i] = (c >= 0x20u && c <= 0x7eu) ? (char)c : '\0';
        if (!c)
            break;
    }
    info->human_readable[IWL_FW_HUMAN_READABLE_SIZE] = '\0';
    info->version = read_le32(data + 72u);
    info->build = read_le32(data + 76u);
    info->api_version = info->version <= 0xffu
        ? info->version : (info->version >> 8) & 0xffu;
    if (expected_api && info->api_version != expected_api)
        return IWL_FW_ERR_API;

    size_t cursor = IWL_TLV_HEADER_SIZE;
    while (cursor < size) {
        if (size - cursor < 8u)
            return IWL_FW_ERR_TRUNCATED;
        uint32_t type = read_le32(data + cursor);
        size_t length = read_le32(data + cursor + 4u);
        cursor += 8u;
        if (length > size - cursor)
            return IWL_FW_ERR_TRUNCATED;
        const uint8_t *value = data + cursor;
        int result = validate_fixed_tlv(type, length);
        if (result != IWL_FW_OK)
            return result;

        switch (type) {
        case IWL_TLV_SEC_RT:
        case IWL_TLV_SECURE_SEC_RT:
            result = add_section(&info->runtime, value, length);
            break;
        case IWL_TLV_SEC_INIT:
        case IWL_TLV_SECURE_SEC_INIT:
            result = add_section(&info->init, value, length);
            break;
        case IWL_TLV_SEC_WOWLAN:
        case IWL_TLV_SECURE_SEC_WOWLAN:
            result = add_section(&info->wowlan, value, length);
            break;
        case IWL_TLV_SEC_RT_USNIFFER:
            result = add_section(&info->sniffer, value, length);
            break;
        case IWL_TLV_NUM_OF_CPU:
            info->num_cpus = read_le32(value);
            break;
        case IWL_TLV_PAGING:
            info->paging_size = read_le32(value);
            break;
        case IWL_TLV_IML:
            info->iml_data = value;
            info->iml_size = length;
            break;
        default:
            break; /* Unknown TLVs are retained in the original blob. */
        }
        if (result != IWL_FW_OK)
            return result;
        info->tlv_count++;

        size_t aligned = (length + 3u) & ~(size_t)3u;
        if (aligned < length || aligned > size - cursor)
            return IWL_FW_ERR_TRUNCATED;
        cursor += aligned;
    }
    if (!info->runtime.section_count)
        return IWL_FW_ERR_MISSING_RUNTIME;
    return IWL_FW_OK;
}

const char *iwl_fw_error_string(int error)
{
    switch (error) {
    case IWL_FW_OK: return "ok";
    case IWL_FW_ERR_ARGUMENT: return "invalid argument";
    case IWL_FW_ERR_HEADER: return "header too short";
    case IWL_FW_ERR_MAGIC: return "bad TLV magic";
    case IWL_FW_ERR_TRUNCATED: return "truncated TLV";
    case IWL_FW_ERR_TOO_MANY_SECTIONS: return "too many sections";
    case IWL_FW_ERR_TLV_LENGTH: return "invalid TLV length";
    case IWL_FW_ERR_API: return "firmware API mismatch";
    case IWL_FW_ERR_MISSING_RUNTIME: return "runtime image missing";
    default: return "unknown parser error";
    }
}
