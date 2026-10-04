    .section .rodata
    .balign 16
    .global iwlwifi_quz_hr_77_fw_start
    .global iwlwifi_quz_hr_77_fw_end
iwlwifi_quz_hr_77_fw_start:
    .incbin "firmware/iwlwifi-QuZ-a0-hr-b0-77.ucode"
iwlwifi_quz_hr_77_fw_end:
    .section .note.GNU-stack,"",@progbits
