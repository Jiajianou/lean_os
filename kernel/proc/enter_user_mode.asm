; kernel/proc/enter_user_mode.asm
;
; enter_user_mode(entry, user_stack, arg_ptr, user_data_sel, user_code_sel):
; drops from ring 0 to ring 3 via `iretq` - the only instruction that can
; load a new CS/SS at a different privilege level in one step (same rule
; gdt_flush already runs into: CS can't be loaded with `mov`, and a direct
; jump/call can't cross rings).
; Never returns to its caller - the only way back into kernel code for
; this task is a future interrupt/syscall, landing via isr_common_stub/
; syscall_common_stub's own iretq, using TSS.RSP0 as the entry stack
; (gdt.c's tss_set_rsp0, kept current by the scheduler).
;
; arg_ptr ends up in RDI at the moment execution resumes in ring 3 - crt0
; (user_space/lib/crt0.asm) passes it straight through as main()'s first
; argument, System V style, without crt0 needing to know anything special
; about where it came from. This is lean_os's entire "argv" mechanism
; (M13): a single string, not a real argc/argv array - proc.c's job to
; populate the page it points at.
;
; Selectors are passed in exactly as they should appear in the iretq
; frame / segment registers - i.e. already `| 3` (RPL 3) - proc.c's job,
; not this routine's.

bits 64

global enter_user_mode
enter_user_mode:
    ; System V AMD64: rdi=entry, rsi=user_stack, rdx=arg_ptr,
    ; rcx=user_data_sel, r8=user_code_sel
    mov ax, cx
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push rcx        ; SS
    push rsi        ; RSP
    pushfq
    pop rax
    or rax, 0x200   ; force IF set - ring 3 code must run with interrupts enabled
    push rax        ; RFLAGS
    push r8          ; CS
    push rdi         ; RIP
    mov rdi, rdx     ; set RDI = arg_ptr last, once every other use of rdi/rcx above is done
    iretq
