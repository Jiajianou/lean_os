bits 64

section .text

global gdt_flush
gdt_flush:
    lgdt [rdi]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    lea rax, [rel .reload_cs]
    push qword 0x08
    push rax
    o64 retf
.reload_cs:
    ret

global tss_flush
tss_flush:
    mov ax, di
    ltr ax
    ret
