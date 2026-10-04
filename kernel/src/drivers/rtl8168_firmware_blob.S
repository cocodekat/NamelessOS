    .section .rodata.rtl8168_firmware,"a",@progbits
    .balign 4
    .global rtl8168h_2_fw_start
    .global rtl8168h_2_fw_end
rtl8168h_2_fw_start:
    .incbin "firmware/rtl8168h-2.fw"
rtl8168h_2_fw_end:

    .section .note.GNU-stack,"",@progbits
