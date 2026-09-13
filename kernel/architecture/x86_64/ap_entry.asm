bits 64
default abs

extern ap_main

AP_PARAMS_ADDR   equ 0x7000
AP_OFF_STACK_TOP equ 8
AP_OFF_CPU_ID    equ 34
AP_OFF_AP_READY  equ 38

section .text

global ap_entry_asm_stub
ap_entry_asm_stub:
    mov rsp, [AP_PARAMS_ADDR + AP_OFF_STACK_TOP]
    mov edi, [AP_PARAMS_ADDR + AP_OFF_CPU_ID]
    mov dword [AP_PARAMS_ADDR + AP_OFF_AP_READY], 1
    call ap_main
.hang:
    cli
    hlt
    jmp .hang
