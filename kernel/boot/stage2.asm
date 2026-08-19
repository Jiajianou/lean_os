; kernel/boot/stage2.asm
;
; Stage 2 bootloader — loaded by stage1 at 0x0000:0x7E00, still in 16-bit
; real mode. For M1 this only proves the stage1 -> stage2 handoff works
; (disk read succeeded, jump landed at the right address). The real
; mode -> protected mode -> long mode transition lands here in M2.
;
; print_string is duplicated from stage1.asm rather than shared: these are
; two independently assembled flat binaries with no linker between them at
; this stage. That goes away in M2, once stage2 grows into real C code
; linked properly instead of a second raw .asm blob.

bits 16
org 0x7E00

start:
    mov si, msg_stage2
    call print_string

    jmp halt

print_string:
    pusha
.next_char:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x07
    int 0x10
    jmp .next_char
.done:
    popa
    ret

halt:
    cli
    hlt
    jmp halt

msg_stage2: db "lean_os stage2: alive.", 13, 10, 0

; Pad out to exactly the number of sectors stage1 reads for us. If this
; ever overflows, nasm errors out on the negative fill (a natural size guard).
times (4*512) - ($ - $$) db 0
