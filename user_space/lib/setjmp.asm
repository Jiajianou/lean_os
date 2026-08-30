; user_space/lib/setjmp.asm - M80 groundwork
;
; setjmp/longjmp for x86-64 System V, in assembly because there is no way
; to write them in C: what they save is the caller's own return address
; and stack pointer, which a C function cannot name.
;
; This is the first thing CPython's own source asks for that this project
; had no answer to at all, and it is the smallest of the things it asks
; for - which is why it is here rather than waiting for the rest of that
; port to be possible.
;
; What is saved is exactly the SysV callee-saved set plus the two things
; that make a jump a jump:
;
;   [0]  rbx   [8]  rbp   [16] r12  [24] r13
;   [32] r14   [40] r15   [48] rsp (as it will be after this ret)
;   [56] rip (the return address this setjmp will come back through)
;
; Caller-saved registers are deliberately NOT saved, which is the C
; standard's own rule rather than a shortcut: after a longjmp, a local
; that is not volatile has an indeterminate value, precisely because the
; compiler was entitled to keep it in a register nothing here restores.
;
; The x87/SSE control words are not saved either. Every real
; implementation makes the same choice, and this one has a second reason:
; M63 gave each *task* its own FXSAVE area, so a longjmp inside one task
; cannot land on another's floating-point state.

bits 64

global setjmp
global longjmp

section .text

setjmp:
    mov [rdi + 0],  rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rax, [rsp + 8]      ; rsp as the caller will see it once this returns
    mov [rdi + 48], rax
    mov rax, [rsp]          ; the return address - where longjmp will resume
    mov [rdi + 56], rax
    xor eax, eax            ; setjmp itself returns 0
    ret

longjmp:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    ; longjmp(buf, 0) must make setjmp return 1, not 0 - the standard
    ; says so, because 0 is how a caller tells "this is the first time
    ; through" from "somebody jumped here".
    mov eax, esi
    test eax, eax
    jnz .go
    mov eax, 1
.go:
    jmp qword [rdi + 56]
