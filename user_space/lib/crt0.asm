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
;
; M60: RDI arrives pointing at the argument page (proc.h's USER_ARG_ADDR),
; which now holds a real argument vector rather than one string:
;
;     [rdi + 0]   argc
;     [rdi + 8]   argv[0..argc], NULL-terminated
;
; so this unpacks it into main(int argc, char **argv). Deliberately a
; *page* rather than a stack frame the kernel builds: RDI already pointed
; here, so nothing about the ring-3 transition had to change, and the two
; instructions below are the entire cost of the unpacking. A program
; written as `int main(void)` keeps working untouched - SysV puts the
; extra arguments in registers it simply never reads.

bits 64

extern main
extern sys_exit

global _start

section .text
_start:
    mov rax, [rdi]      ; argc
    lea rsi, [rdi + 8]  ; argv
    mov rdi, rax
    call main
    mov edi, eax    ; main's return value -> sys_exit's argument
    call sys_exit
.hang:              ; unreachable - sys_exit doesn't return
    jmp .hang
