; kernel/arch/x86_64/isr_asm.asm
;
; Per-vector trampolines for all 32 CPU exceptions and the 16 PIC IRQ
; lines (remapped to 32-47 by pic.c). The CPU only pushes an error code
; for some exceptions (Intel SDM Vol 3A, 6.15) - the ISR_NOERR stubs push
; a dummy 0 so every path hits isr_common_stub with an identical frame.
;
; isr_common_stub/irq_common_stub save the general-purpose registers,
; call into C with rsp (pointing at the saved regs) as the first
; argument per the System V AMD64 ABI, then unwind and iretq. This is
; exactly the kind of "C can't reach it" spot the ground rules call out:
; nothing in C can push a CPU-defined exception frame, save a raw
; register file for a handler to inspect, or iretq back into interrupted
; code.

bits 64

extern isr_handler
extern irq_handler
extern syscall_handler
extern lapic_vector_handler

%macro ISR_NOERR 1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
isr%1:
    push qword %1
    jmp isr_common_stub
%endmacro

%macro IRQ 2
irq%1:
    push qword 0
    push qword %2
    jmp irq_common_stub
%endmacro

section .text

ISR_NOERR 0   ; #DE  Divide-by-zero
ISR_NOERR 1   ; #DB  Debug
ISR_NOERR 2   ;      NMI
ISR_NOERR 3   ; #BP  Breakpoint
ISR_NOERR 4   ; #OF  Overflow
ISR_NOERR 5   ; #BR  Bound range exceeded
ISR_NOERR 6   ; #UD  Invalid opcode
ISR_NOERR 7   ; #NM  Device not available
ISR_ERR   8   ; #DF  Double fault
ISR_NOERR 9   ;      Coprocessor segment overrun (legacy, unused)
ISR_ERR   10  ; #TS  Invalid TSS
ISR_ERR   11  ; #NP  Segment not present
ISR_ERR   12  ; #SS  Stack-segment fault
ISR_ERR   13  ; #GP  General protection fault
ISR_ERR   14  ; #PF  Page fault
ISR_NOERR 15  ;      Reserved
ISR_NOERR 16  ; #MF  x87 floating-point exception
ISR_ERR   17  ; #AC  Alignment check
ISR_NOERR 18  ; #MC  Machine check
ISR_NOERR 19  ; #XM  SIMD floating-point exception
ISR_NOERR 20  ; #VE  Virtualization exception
ISR_ERR   21  ; #CP  Control protection exception
ISR_NOERR 22  ;      Reserved
ISR_NOERR 23  ;      Reserved
ISR_NOERR 24  ;      Reserved
ISR_NOERR 25  ;      Reserved
ISR_NOERR 26  ;      Reserved
ISR_NOERR 27  ;      Reserved
ISR_NOERR 28  ;      Reserved
ISR_NOERR 29  ;      Reserved
ISR_ERR   30  ; #SX  Security exception
ISR_NOERR 31  ;      Reserved

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; Dedicated software-interrupt gate for syscalls (M8) - not a CPU
; exception or a PIC IRQ, so it gets its own stub and common path rather
; than reusing isr_common_stub/irq_common_stub, whose C dispatchers mean
; something different ("unhandled -> panic", "unhandled -> EOI+ignore").
; `int 0x80` pushes no error code, same as any ISR_NOERR vector.
isr128:
    push qword 0
    push qword 0x80
    jmp syscall_common_stub

; SMP: the two Local-APIC-raised vectors (idt.c's IPI_SCHEDULE_VECTOR=0xF0/
; LAPIC_SPURIOUS_VECTOR=0xFF) - same "own stub, own common path" reasoning
; as isr128 above, since lapic_vector_handler's dispatch (and EOI target -
; the Local APIC, not the 8259) is different from every other path here.
isr_ipi_schedule:
    push qword 0
    push qword 0xF0
    jmp lapic_common_stub

isr_lapic_spurious:
    push qword 0
    push qword 0xFF
    jmp lapic_common_stub

%macro SAVE_REGS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro RESTORE_REGS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

isr_common_stub:
    SAVE_REGS
    mov rdi, rsp        ; &isr_regs_t, per isr.h's layout
    call isr_handler
    RESTORE_REGS
    add rsp, 16          ; drop vector + error_code
    iretq

irq_common_stub:
    SAVE_REGS
    mov rdi, rsp
    call irq_handler
    RESTORE_REGS
    add rsp, 16
    iretq

; syscall_handler sets regs->rax to the return value before returning;
; RESTORE_REGS pops rax last, so that value is what the caller sees in
; RAX once this iretq's done - no separate return-value plumbing needed.
syscall_common_stub:
    SAVE_REGS
    mov rdi, rsp
    call syscall_handler
    RESTORE_REGS
    add rsp, 16
    iretq

; ---- M83: entering ring 3 from a register frame, not from an entry point
;
; fork_return_to_user(isr_regs_t *frame) - never returns.
;
; A forked child has no entry point to be started at: it has to resume in
; the middle of its parent's `int 0x80`, with every register the parent
; had and rax replaced by zero. proc.c writes that frame onto the child's
; own kernel stack and sched.c's fork trampoline hands it here, where the
; last four instructions of a syscall return - the same four the stub
; above ends with - do the rest.
;
; Deliberately in this file rather than a new one, so that the register
; order lives in exactly one place: this must pop the frame the SAVE_REGS
; macro above pushes, and two copies of that order would be a bug waiting
; for someone to add a register.
global fork_return_to_user
fork_return_to_user:
    mov rsp, rdi
    RESTORE_REGS
    add rsp, 16
    iretq

lapic_common_stub:
    SAVE_REGS
    mov rdi, rsp
    call lapic_vector_handler
    RESTORE_REGS
    add rsp, 16
    iretq

; Address tables idt.c installs into the IDT - keeps idt.c from needing
; 48+ individual extern declarations.
section .rodata

global isr_stub_table
isr_stub_table:
    dq isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    dq isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

global irq_stub_table
irq_stub_table:
    dq irq0, irq1, irq2,  irq3,  irq4,  irq5,  irq6,  irq7
    dq irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

global syscall_stub_addr
syscall_stub_addr:
    dq isr128

global isr_ipi_schedule_addr
isr_ipi_schedule_addr:
    dq isr_ipi_schedule

global isr_lapic_spurious_addr
isr_lapic_spurious_addr:
    dq isr_lapic_spurious
