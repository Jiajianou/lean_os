; kernel/boot/stage1.asm
;
; MBR boot sector — stage 1 bootloader.
;
; The BIOS loads this 512-byte sector to 0x0000:0x7C00 and jumps to it in
; 16-bit real mode with DL = boot drive number. This stage prints a status
; message, loads stage 2 from disk, and jumps into it.
;
; Disk reads use INT 13h's extended (LBA) read (AH=42h) rather than the
; classic CHS read: CHS caps sector counts at 63 per track, which stage2's
; kernel loader (kernel/boot/stage2.asm) will blow past as the kernel
; grows. LBA addressing sidesteps that entirely, so both stages use it
; consistently. LBA 0 is this boot sector itself.

bits 16
org 0x7C00

STAGE2_LOAD_SEGMENT equ 0x0000
STAGE2_LOAD_OFFSET  equ 0x7E00   ; immediately after this boot sector
STAGE2_START_LBA    equ 1        ; LBA 0 is this boot sector
STAGE2_SECTOR_COUNT equ 8        ; 8 * 512 = 4 KiB reserved for stage 2

; M24 (UEFI boot path): this same disk image is also a valid legacy-MBR
; partitioned disk, so a UEFI firmware (which never executes any of the
; code above - it reads this sector only as a partition table, never as
; boot code) can find and boot the EFI System Partition below. Chosen to
; sit in the gap this image already left between the boot blob (stage1 +
; stage2 + kernel.bin, currently ~500 sectors, growing over time) and
; leanfs's own start (kernel/fs/leanfs.h's LEANFS_START_LBA, 2048) - see
; the top-level Makefile's IMAGE recipe for where this partition actually
; gets formatted and populated with kernel/boot/uefi's BOOTX64.EFI.
ESP_START_LBA    equ 1024
ESP_SECTOR_COUNT equ 1024        ; ends exactly at LEANFS_START_LBA (2048)

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

; load_stage2: reads STAGE2_SECTOR_COUNT sectors starting at LBA
; STAGE2_START_LBA from the boot drive (DL, set by the BIOS) into
; STAGE2_LOAD_SEGMENT:STAGE2_LOAD_OFFSET via INT 13h's extended read.
load_stage2:
    pusha

    mov word [dap.count], STAGE2_SECTOR_COUNT
    mov word [dap.offset], STAGE2_LOAD_OFFSET
    mov word [dap.segment], STAGE2_LOAD_SEGMENT
    mov dword [dap.lba_low], STAGE2_START_LBA
    mov dword [dap.lba_high], 0

    mov si, dap
    mov ah, 0x42
    int 0x13
    jc .disk_error

    popa
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

; Disk Address Packet for INT 13h, AH=42h (extended read).
align 4
dap:
    .size     db 0x10
    .reserved db 0
    .count    dw 0
    .offset   dw 0
    .segment  dw 0
    .lba_low  dd 0
    .lba_high dd 0

; Legacy MBR partition table (offset 0x1BE / 446) - one entry, the rest
; left zeroed (a valid MBR with fewer than 4 partitions). CHS fields are
; the conventional "exceeds CHS range, use LBA" filler (0xFE,0xFF,0xFF):
; this disk is never addressed by CHS on either boot path. Type 0xEF is
; the standard "EFI System Partition" id UEFI firmware's partition
; driver looks for when auto-generating a boot option.
times 0x1BE - ($ - $$) db 0
db 0x00                  ; status: not BIOS-bootable (irrelevant to UEFI,
                          ; and stage1's own code above is what BIOS runs
                          ; regardless of this table)
db 0xFE, 0xFF, 0xFF       ; CHS start (unused)
db 0xEF                   ; type: EFI System Partition
db 0xFE, 0xFF, 0xFF       ; CHS end (unused)
dd ESP_START_LBA
dd ESP_SECTOR_COUNT

times 510 - ($ - $$) db 0
dw 0xAA55
