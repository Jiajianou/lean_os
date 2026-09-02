; user_space/lib/crt0-pie.asm
;
; ---- M95: the position-independent variant --------------------------
;
; The same file as crt0.asm, with one difference: every call to a global
; goes through the PLT. In a static ET_EXEC a `call sym` is a
; PC-relative branch to a fixed address and that is exactly right; in a
; PIE, `sym` is interposable and ld refuses the same relocation with
; "can not be used when making a PIE object". `wrt ..plt` asks for the
; branch to go through the procedure linkage table, which is where a
; dynamic linker's answer lands.
;
; A separate file rather than an %ifdef, because the two are separate
; objects with separate names in the sysroot - crt1.o and Scrt1.o - and
; every toolchain that has both keeps them apart for the same reason: a
; program is one or the other and the driver picks by flag, so a
; conditional here would just move the choice somewhere it is harder to
; see.
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
; M60: RDI arrives pointing at the argument region (proc.h's
; USER_ARG_ADDR), which now holds a real argument vector rather than one
; string. M75 added an environment immediately after it:
;
;     [rdi + 0]              argc
;     [rdi + 8]              argv[0..argc], NULL-terminated
;     [rdi + 8 + 8*(argc+1)] envp[0..envc], NULL-terminated
;
; so this unpacks all three into __lean_start(argc, argv, envp), which
; publishes `environ` and then calls main. envp is found the way every C
; runtime since V7 Unix has found it - one slot past argv's NULL - which
; is why the kernel puts it there rather than at an address this file
; would have to be told separately.
;
; Deliberately a *region* rather than a stack frame the kernel builds:
; RDI already pointed here, so nothing about the ring-3 transition had to
; change, and the three instructions below are the entire cost of the
; unpacking. A program written as `int main(void)` keeps working
; untouched - SysV puts the extra arguments in registers it never reads.

bits 64

extern __lean_start
extern sys_exit

global _start

section .text
_start:
    mov rax, [rdi]              ; argc
    lea rsi, [rdi + 8]          ; argv
    lea rdx, [rsi + rax*8 + 8]  ; envp = argv + argc + 1
    mov rdi, rax                ; argc
    call __lean_start wrt ..plt
    mov edi, eax    ; main's return value -> sys_exit's argument
    call sys_exit wrt ..plt
.hang:              ; unreachable - sys_exit doesn't return
    jmp .hang

; ---- M76: where a signal handler returns to -----------------------------
;
; The kernel builds a signal frame on this process's own stack and points
; RSP at a word holding this address, so an ordinary `ret` out of an
; ordinary C handler lands here. What follows the return address on the
; stack is the sig_frame_t (system_api/include/signal.h) the kernel
; saved - so RSP, right now, IS the frame's address.
;
; In assembly because there is no way to write it in C: a C function
; would push a frame of its own before it could take the address of
; anything, and the address that matters is the one RSP holds on entry -
; before a single instruction has run.
;
; Its address is handed to the kernel by SYS_sigaction rather than being
; a constant the kernel knows. A kernel that hardcoded a user-space
; address would be a kernel with an opinion about how programs are
; linked, which is exactly what user.ld exists to keep it out of.
;
; SYS_sigreturn does not return: the kernel overwrites the whole ring-3
; frame it is about to iretq through, so execution resumes wherever the
; signal interrupted.

extern __lean_sigreturn_number   ; SYS_sigreturn, defined in C so the ABI header stays the one source of the number

global __lean_sigreturn
__lean_sigreturn:
    mov rdi, rsp                    ; the frame the kernel wrote, immediately above the return address it popped
    mov eax, [rel __lean_sigreturn_number]
    int 0x80
.hang:                              ; unreachable - the kernel replaced this context
    jmp .hang
