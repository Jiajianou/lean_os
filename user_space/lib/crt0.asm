; user_space/lib/crt0.asm
;
; The one piece of a user program's startup that C can't express: the raw
; entry point the kernel's ELF loader jumps to (via enter_user_mode - see
; kernel/proc/enter_user_mode.asm), calling into the program's main() and
; then exiting with its return value instead of falling off the end into
; nothing. Every user program links this in - it's what makes `int main
; (void)` a valid thing for user code to write instead of hand-writing
; _start itself. RSP arrives already 16-byte aligned (enter_user_mode
; sets it to a page-aligned stack top), which is exactly what the x86_64
; SysV ABI requires right before a `call`.

bits 64

extern main
extern sys_exit

global _start

section .text
_start:
    call main
    mov edi, eax    ; main's return value -> sys_exit's argument
    call sys_exit
.hang:              ; unreachable - sys_exit doesn't return
    jmp .hang
