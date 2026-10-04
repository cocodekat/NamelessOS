#ifndef DRIVERS_WIFI_IWL_TRANSPORT_H
#define DRIVERS_WIFI_IWL_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/* Intel 22000-family context-info queue geometry used by QuZ/AX201. */
#define IWL_RX_RING_ENTRIES       2048u
#define IWL_RX_BOOTSTRAP_BUFFERS   256u
#define IWL_RX_BUFFER_SIZE         4096u
#define IWL_CMD_RING_ENTRIES         32u
#define IWL_TFH_NUM_TBS              25u
#define IWL_FIRST_TB_SIZE             20u
#define IWL_FIRST_TB_STRIDE           64u

#define IWL_CONTEXT_RB_CB_SHIFT        4u
#define IWL_CONTEXT_TFD_LONG       0x0100u
#define IWL_CONTEXT_RB_4K          0x0800u

typedef struct __attribute__((packed)) {
    uint16_t mac_id, version, size, reserved;
} iwl_context_version_t;

typedef struct __attribute__((packed)) {
    uint32_t flags, reserved;
} iwl_context_control_t;

typedef struct __attribute__((packed)) {
    uint64_t free_rbd, used_rbd, status_write;
} iwl_context_rbd_t;

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint8_t size;
    uint8_t reserved[7];
} iwl_context_command_t;

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint32_t size;
    uint32_t reserved;
} iwl_context_buffer_t;

typedef struct __attribute__((packed)) {
    uint64_t umac[64];
    uint64_t lmac[64];
    uint64_t paging[64];
} iwl_context_dram_t;

typedef struct __attribute__((packed)) {
    iwl_context_version_t version;
    iwl_context_control_t control;
    uint64_t reserved0;
    iwl_context_rbd_t rbd;
    iwl_context_command_t command;
    uint32_t reserved1[4];
    iwl_context_buffer_t dump;
    iwl_context_buffer_t early_debug;
    iwl_context_buffer_t pnvm;
    uint32_t reserved2[16];
    iwl_context_dram_t dram;
    uint32_t reserved3[16];
} iwl_context_info_t;

typedef struct __attribute__((packed)) {
    uint16_t closed_rb_num;
    uint16_t closed_frame_num;
    uint16_t finished_rb_num;
    uint16_t finished_frame_num;
    uint32_t reserved;
} iwl_rx_status_t;

typedef struct __attribute__((packed)) {
    uint16_t length;
    uint64_t address;
} iwl_tfh_tb_t;

typedef struct __attribute__((packed)) {
    uint16_t num_tbs;
    iwl_tfh_tb_t tbs[IWL_TFH_NUM_TBS];
    uint32_t reserved;
} iwl_tfh_tfd_t;

static inline uint32_t iwl_context_control_flags(void)
{
    /* log2(2048) == 11; the exponent occupies bits 4..7. */
    return (11u << IWL_CONTEXT_RB_CB_SHIFT) |
           IWL_CONTEXT_TFD_LONG | IWL_CONTEXT_RB_4K;
}

static inline uint8_t iwl_command_ring_size_code(void)
{
    /* The hardware encoding is log2(entries) - 3. */
    return 2u;
}

_Static_assert(sizeof(iwl_context_dram_t) == 1536u, "Intel DRAM map layout");
_Static_assert(sizeof(iwl_context_info_t) == 1792u, "Intel context info layout");
_Static_assert(sizeof(iwl_rx_status_t) == 12u, "Intel RX status layout");
_Static_assert(sizeof(iwl_tfh_tb_t) == 10u, "Intel TFH transfer buffer layout");
_Static_assert(sizeof(iwl_tfh_tfd_t) == 256u, "Intel long TFD layout");
_Static_assert(IWL_RX_BOOTSTRAP_BUFFERS < IWL_RX_RING_ENTRIES,
               "RX ring must retain an empty slot");

#endif
