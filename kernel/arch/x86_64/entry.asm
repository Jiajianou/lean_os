; kernel/arch/x86_64/entry.asm
;
; Kernel entry point. The bootloader (kernel/boot/stage2.asm) jumps here
; directly in 64-bit long mode — this is byte 0 of the flat kernel.bin, at
; its linked load address (see kernel/linker.ld). RDI holds a pointer to
; the E820 memory map header and RSI a pointer to the fb_boot_info_t
; framebuffer descriptor (M16) - the System V AMD64 ABI's first and
; second integer-argument registers, matching what stage2 loads them
; with before the jump.

bits 64

extern kernel_main

section .text.entry

global _start
_start:
    mov rsp, kernel_stack_top   ; off the bootloader's scratch stack, onto
                                 ; one that's actually part of the kernel
                                 ; image (so the M5 memory manager knows
                                 ; not to hand these pages out)
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
kernel_stack_bottom:
    resb 16384                  ; 16 KiB kernel stack
kernel_stack_top:
