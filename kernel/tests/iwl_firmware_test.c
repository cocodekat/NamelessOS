#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/wifi/iwl_firmware.h"

static int parse_should_fail(uint8_t *image, size_t size, int expected)
{
    iwl_fw_info_t info;
    int result = iwl_fw_parse(image, size, 77u, &info);
    if (result != expected) {
        fprintf(stderr, "expected parser error %d, got %d (%s)\n",
                expected, result, iwl_fw_error_string(result));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0)
        return 2;
    long file_size = ftell(file);
    if (file_size <= 0 || fseek(file, 0, SEEK_SET) != 0)
        return 2;
    uint8_t *image = malloc((size_t)file_size);
    if (!image || fread(image, 1, (size_t)file_size, file) != (size_t)file_size)
        return 2;
    fclose(file);

    iwl_fw_info_t info;
    int result = iwl_fw_parse(image, (size_t)file_size, 77u, &info);
    if (result != IWL_FW_OK || info.api_version != 77u ||
        strcmp(info.human_readable, "release/core74::aa2dd297") != 0 ||
        !info.runtime.section_count) {
        fprintf(stderr, "firmware parse failed: %d (%s), name=%s api=%u rt=%zu\n",
                result, iwl_fw_error_string(result), info.human_readable,
                info.api_version, info.runtime.section_count);
        return 1;
    }

    if (parse_should_fail(image, 87u, IWL_FW_ERR_HEADER))
        return 1;
    image[4] ^= 1u;
    if (parse_should_fail(image, (size_t)file_size, IWL_FW_ERR_MAGIC))
        return 1;
    image[4] ^= 1u;
    uint8_t saved_length[4];
    memcpy(saved_length, image + 92u, sizeof(saved_length));
    memset(image + 92u, 0xff, 4u);
    if (parse_should_fail(image, (size_t)file_size, IWL_FW_ERR_TRUNCATED))
        return 1;
    memcpy(image + 92u, saved_length, sizeof(saved_length));
    if (parse_should_fail(image, (size_t)file_size - 1u, IWL_FW_ERR_TRUNCATED))
        return 1;

    printf("Intel firmware test passed: %s api=%u build=%08x tlvs=%u "
           "runtime=%zu init=%zu wowlan=%zu sniffer=%zu cpus=%u paging=%u iml=%zu\n",
           info.human_readable, info.api_version, info.build, info.tlv_count,
           info.runtime.section_count, info.init.section_count,
           info.wowlan.section_count, info.sniffer.section_count,
           info.num_cpus, info.paging_size, info.iml_size);
    free(image);
    return 0;
}
