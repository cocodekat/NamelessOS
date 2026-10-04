#ifndef DRIVERS_WIFI_IWL_FIRMWARE_H
#define DRIVERS_WIFI_IWL_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

#define IWL_FW_MAX_SECTIONS 64u
#define IWL_FW_HUMAN_READABLE_SIZE 64u

typedef struct {
    uint32_t offset;
    const uint8_t *data;
    size_t size;
} iwl_fw_section_t;

typedef struct {
    iwl_fw_section_t sections[IWL_FW_MAX_SECTIONS];
    size_t section_count;
} iwl_fw_image_t;

typedef struct {
    char human_readable[IWL_FW_HUMAN_READABLE_SIZE + 1u];
    uint32_t version;
    uint32_t build;
    uint32_t api_version;
    uint32_t tlv_count;
    uint32_t num_cpus;
    uint32_t paging_size;
    const uint8_t *iml_data;
    size_t iml_size;
    iwl_fw_image_t runtime;
    iwl_fw_image_t init;
    iwl_fw_image_t wowlan;
    iwl_fw_image_t sniffer;
} iwl_fw_info_t;

enum {
    IWL_FW_OK = 0,
    IWL_FW_ERR_ARGUMENT = -1,
    IWL_FW_ERR_HEADER = -2,
    IWL_FW_ERR_MAGIC = -3,
    IWL_FW_ERR_TRUNCATED = -4,
    IWL_FW_ERR_TOO_MANY_SECTIONS = -5,
    IWL_FW_ERR_TLV_LENGTH = -6,
    IWL_FW_ERR_API = -7,
    IWL_FW_ERR_MISSING_RUNTIME = -8
};

int iwl_fw_parse(const uint8_t *data, size_t size, uint32_t expected_api,
                 iwl_fw_info_t *info);
const char *iwl_fw_error_string(int error);

extern const uint8_t iwlwifi_quz_hr_77_fw_start[];
extern const uint8_t iwlwifi_quz_hr_77_fw_end[];

#endif
