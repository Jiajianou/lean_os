; kernel/boot/mbr.asm
;
; LBA 0 of the disk image. UEFI-only now (see milestones.md, M26) - nothing
; ever executes this sector as code. Its only job is to be a valid legacy
; MBR partition table so UEFI firmware can find the EFI System Partition:
; QEMU/OVMF's boot manager, finding no GPT, falls back to scanning the
; legacy MBR partition entries for one of type 0xEF (EFI System Partition)
; and boots from that - confirmed working behavior since M24, kept as-is
; here now that it's the only boot path.
;
; One partition entry, the rest of the table left zeroed (a valid MBR with
; fewer than 4 partitions). CHS fields are the conventional "exceeds CHS
; range, use LBA" filler (0xFE,0xFF,0xFF) - this disk is never addressed by
; CHS. ESP_START_LBA/ESP_SECTOR_COUNT are passed in via `nasm -D` from the
; top-level Makefile, which is also what formats and populates that region
; with kernel/boot/uefi's BOOTX64.EFI - single source of truth for both
; numbers.

%ifndef ESP_START_LBA
%define ESP_START_LBA 1024
%endif
%ifndef ESP_SECTOR_COUNT
%define ESP_SECTOR_COUNT 1024
%endif

times 0x1BE db 0          ; boot code area: unused, nothing ever executes this sector

db 0x00                   ; status: not BIOS-bootable (irrelevant - no BIOS path exists)
db 0xFE, 0xFF, 0xFF        ; CHS start (unused)
db 0xEF                    ; type: EFI System Partition
db 0xFE, 0xFF, 0xFF        ; CHS end (unused)
dd ESP_START_LBA
dd ESP_SECTOR_COUNT

times 510 - ($ - $$) db 0
dw 0xAA55
