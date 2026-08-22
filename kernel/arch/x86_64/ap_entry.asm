; kernel/arch/x86_64/ap_entry.asm
;
; 64-bit landing point for an AP. By the time control reaches here,
; kernel/arch/x86_64/ap_trampoline.asm (a standalone, separately-assembled
; 16-bit blob - see that file's own header comment and the Makefile) has
; already walked the AP through real -> protected -> long mode, loaded
; CR3 = the kernel's real PML4, and switched onto the kernel's real GDT -
; this is a normal, kernel-linked symbol, reached by an absolute jump from
; inside that blob once smp.c has written this address into the shared
; AP_PARAMS_ADDR struct.
;
; All that's left is AP-specific: read this core's own private stack top
; and cpu_id out of that same shared struct (kernel/arch/x86_64/smp.c
; documents its exact byte layout - both sides have to agree on it by
; hand, since there's no way to share a real C struct type across the
; "raw 16-bit blob" boundary), switch onto that stack, tell the BSP it's
; safe to reuse AP_PARAMS_ADDR for the next AP, and call into C.

bits 64
default abs ; explicit: the [AP_PARAMS_ADDR + off] operands below are flat
             ; absolute addresses, not RIP-relative - nasm's own default,
             ; made explicit to silence its "implicit DEFAULT ABS is
             ; deprecated" warning.

extern ap_main

AP_PARAMS_ADDR   equ 0x7000
AP_OFF_STACK_TOP equ 8
AP_OFF_CPU_ID    equ 34
AP_OFF_AP_READY  equ 38

section .text

global ap_entry_asm_stub
ap_entry_asm_stub:
    mov rsp, [AP_PARAMS_ADDR + AP_OFF_STACK_TOP]
    mov edi, [AP_PARAMS_ADDR + AP_OFF_CPU_ID]   ; ap_main(uint32_t cpu_id) - first SysV integer arg
    ; Nothing below this line reads AP_PARAMS_ADDR again - safe for the
    ; BSP to start overwriting it for the next AP the instant it observes
    ; ap_ready set.
    mov dword [AP_PARAMS_ADDR + AP_OFF_AP_READY], 1
    call ap_main
.hang:
    cli
    hlt
    jmp .hang
