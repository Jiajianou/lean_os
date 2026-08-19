; kernel/arch/x86_64/idt_asm.asm
;
; LIDT itself - straightforward, but there's no C intrinsic for it in a
; freestanding build without pulling in compiler builtins, so it lives
; here alongside the rest of the "asm-only" descriptor loads.

bits 64

section .text

; idt_flush: load a new IDT.
; In: rdi = pointer to the {limit:16, base:64} table descriptor.
global idt_flush
idt_flush:
    lidt [rdi]
    ret
