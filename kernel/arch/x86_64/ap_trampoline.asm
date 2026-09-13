bits 16
org 0x8000
default abs

AP_PARAMS_ADDR   equ 0x7000
AP_OFF_CR3       equ 0
AP_OFF_ENTRY64   equ 16
AP_OFF_GDT_LIMIT equ 24
AP_OFF_GDT_BASE  equ 26
AP_OFF_NX        equ 42

CODE32_SEL equ trampoline_gdt_code32 - trampoline_gdt_start
DATA32_SEL equ trampoline_gdt_data32 - trampoline_gdt_start
CODE64_SEL equ trampoline_gdt_code64 - trampoline_gdt_start

ap_trampoline_entry:
    cli
    jmp 0x0000:normalize

normalize:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7FF0

    lgdt [trampoline_gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE32_SEL:ap_start32

bits 32
ap_start32:
    mov ax, DATA32_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7FF0

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov eax, [AP_PARAMS_ADDR + AP_OFF_CR3]
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    cmp byte [AP_PARAMS_ADDR + AP_OFF_NX], 0
    je .no_nx
    or eax, 1 << 11
.no_nx:
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    jmp CODE64_SEL:ap_start64

bits 64
ap_start64:
    mov ax, DATA32_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    lgdt [AP_PARAMS_ADDR + AP_OFF_GDT_LIMIT]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rax, [AP_PARAMS_ADDR + AP_OFF_ENTRY64]
    jmp rax

align 8
trampoline_gdt_start:
    dq 0
trampoline_gdt_code32:
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
trampoline_gdt_data32:
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
trampoline_gdt_code64:
    dw 0x0000, 0x0000
    db 0x00, 10011010b, 00100000b, 0x00
trampoline_gdt_end:

trampoline_gdt_descriptor:
    dw trampoline_gdt_end - trampoline_gdt_start - 1
    dd trampoline_gdt_start
