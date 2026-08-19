; kernel/boot/stage1.asm
;
; MBR boot sector — stage 1 bootloader.
;
; The BIOS loads this 512-byte sector to 0x0000:0x7C00 and jumps to it in
; 16-bit real mode with DL = boot drive number. This stage prints a status
; message, loads stage 2 from disk via INT 13h, and jumps into it.

bits 16
org 0x7C00

STAGE2_LOAD_SEGMENT equ 0x0000
STAGE2_LOAD_OFFSET  equ 0x7E00   ; immediately after this boot sector
STAGE2_START_SECTOR equ 2        ; CHS sectors are 1-indexed; sector 1 is us
STAGE2_SECTOR_COUNT equ 4        ; 4 * 512 = 2 KiB reserved for stage 2

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00          ; stack grows down from us, away from loaded code
    sti

    mov si, msg_stage1
    call print_string

    call load_stage2

    mov si, msg_jump
    call print_string

    jmp STAGE2_LOAD_SEGMENT:STAGE2_LOAD_OFFSET

; print_string: BIOS teletype-print a null-terminated string.
; In:  SI = pointer to string
print_string:
    pusha
.next_char:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E            ; BIOS teletype output
    mov bh, 0x00             ; page 0
    mov bl, 0x07             ; light grey on black (only used in graphics modes)
    int 0x10
    jmp .next_char
.done:
    popa
    ret

; load_stage2: reads STAGE2_SECTOR_COUNT sectors starting at
; STAGE2_START_SECTOR from the boot drive (DL, set by the BIOS) into
; STAGE2_LOAD_SEGMENT:STAGE2_LOAD_OFFSET.
load_stage2:
    push ax
    push bx
    push cx
    push dx
    push es

    mov ax, STAGE2_LOAD_SEGMENT
    mov es, ax
    mov bx, STAGE2_LOAD_OFFSET

    mov ah, 0x02                    ; BIOS: read sectors into ES:BX
    mov al, STAGE2_SECTOR_COUNT
    mov ch, 0x00                    ; cylinder 0
    mov cl, STAGE2_START_SECTOR
    mov dh, 0x00                    ; head 0
    ; DL already holds the BIOS-provided boot drive number

    int 0x13
    jc .disk_error

    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.disk_error:
    mov si, msg_disk_error
    call print_string
    jmp halt

halt:
    cli
    hlt
    jmp halt

msg_stage1:     db "lean_os stage1: booting...", 13, 10, 0
msg_jump:       db "lean_os stage1: stage2 loaded, jumping...", 13, 10, 0
msg_disk_error: db "lean_os stage1: DISK READ ERROR", 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
