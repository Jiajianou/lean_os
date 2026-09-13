section .rodata

global ap_trampoline_start
global ap_trampoline_end

ap_trampoline_start:
    incbin "build/ap_trampoline.bin"
ap_trampoline_end:
