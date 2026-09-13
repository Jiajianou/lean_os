bits 64

extern __lean_start
extern main
extern sys_exit

global _start

section .text
_start:
    mov rax, [rdi]
    lea rsi, [rdi + 8]
    lea rdx, [rsi + rax*8 + 8]
    mov rdi, rax
    lea rcx, [rel main]
    call __lean_start
    mov edi, eax
    call sys_exit
.hang:
    jmp .hang

extern __lean_sigreturn_number

global __lean_sigreturn
__lean_sigreturn:
    mov rdi, rsp
    mov eax, [rel __lean_sigreturn_number]
    int 0x80
.hang:
    jmp .hang
