; kernel/proc/embed_ap_trampoline.asm
;
; Embeds the standalone AP bring-up blob (kernel/arch/x86_64/
; ap_trampoline.asm, built separately as a flat binary - see the Makefile)
; into the kernel image as raw bytes, the same incbin pattern
; embed_programs.asm already uses for user ELF binaries.
; kernel/arch/x86_64/smp.c copies these bytes to physical
; AP_TRAMPOLINE_LOAD_ADDR at runtime, once per AP, right before sending
; that AP its SIPI.

section .rodata

global ap_trampoline_start
global ap_trampoline_end

ap_trampoline_start:
    incbin "build/ap_trampoline.bin"
ap_trampoline_end:
