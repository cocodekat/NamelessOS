#include <stdint.h>
#include <stdio.h>
#include "drivers/wifi/iwl_transport.h"

int main(void)
{
    if (iwl_context_control_flags() != 0x9b0u) {
        fprintf(stderr, "wrong context flags: 0x%x\n",
                iwl_context_control_flags());
        return 1;
    }
    if (iwl_command_ring_size_code() != 2u ||
        sizeof(iwl_tfh_tfd_t) * IWL_CMD_RING_ENTRIES != 8192u) {
        fprintf(stderr, "wrong command ring geometry\n");
        return 1;
    }
    printf("Intel transport layout test passed: rx=%u cmd=%u flags=0x%x\n",
           IWL_RX_RING_ENTRIES, IWL_CMD_RING_ENTRIES,
           iwl_context_control_flags());
    return 0;
}
