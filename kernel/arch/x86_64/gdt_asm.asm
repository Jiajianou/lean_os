; kernel/arch/x86_64/gdt_asm.asm
;
; The parts of GDT/TSS loading C can't do: LGDT plus reloading every
; segment register (including CS, which needs a far transfer - there's no
; "mov cs" instruction), and LTR for the task register.

bits 64

section .text

; gdt_flush: load a new GDT and reload every segment register to match.
; In: rdi = pointer to the {limit:16, base:64} table descriptor.
global gdt_flush
gdt_flush:
    lgdt [rdi]

    mov ax, 0x10        ; GDT_KERNEL_DATA_SEL (gdt.h) - kept as a literal
    mov ds, ax           ; here since asm can't include the C header
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; CS can only be reloaded via a far transfer. A direct far jump can't
    ; encode a full 64-bit target in long mode, so use the standard
    ; far-return trick: push a far pointer to the label below, then retf
    ; into it, which reloads CS as a side effect.
    lea rax, [rel .reload_cs]
    push qword 0x08      ; GDT_KERNEL_CODE_SEL (gdt.h)
    push rax
    o64 retf
.reload_cs:
    ret

; tss_flush: load the task register.
; In: di = TSS selector.
global tss_flush
tss_flush:
    mov ax, di
    ltr ax
    ret
