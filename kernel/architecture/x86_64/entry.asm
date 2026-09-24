bits 64

extern kernel_main
extern __bss_start
extern __kernel_end

section .text.entry

global _start
_start:
    mov rsp, kernel_stack_top

    mov rax, __bss_start
    mov rcx, __kernel_end
.zero_bss:
    cmp rax, rcx
    jae .zero_bss_done
    mov qword [rax], 0
    add rax, 8
    jmp .zero_bss
.zero_bss_done:

    mov rcx, r8

    mov rax, kernel_stack_guard
    mov rbx, 0x5354414B47554152
    mov qword [rax], rbx

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16

global kernel_stack_guard
kernel_stack_guard:
    resq 1

kernel_stack_bottom:
    resb 65536
kernel_stack_top:
