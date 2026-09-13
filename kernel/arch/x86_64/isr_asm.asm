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

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

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

isr128:
    push qword 0
    push qword 0x80
    jmp syscall_common_stub

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
    mov rdi, rsp
    call isr_handler
    RESTORE_REGS
    add rsp, 16
    iretq

irq_common_stub:
    SAVE_REGS
    mov rdi, rsp
    call irq_handler
    RESTORE_REGS
    add rsp, 16
    iretq

syscall_common_stub:
    SAVE_REGS
    mov rdi, rsp
    call syscall_handler
    RESTORE_REGS
    add rsp, 16
    iretq

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
