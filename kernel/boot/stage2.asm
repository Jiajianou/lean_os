; kernel/boot/stage2.asm
;
; Stage 2 bootloader. Loaded by stage1 at 0x0000:0x7E00 in 16-bit real
; mode. Responsible for the full 16-bit -> 32-bit protected -> 64-bit long
; mode transition, then handing off to the real kernel binary (built from
; kernel/*.c + kernel/arch/x86_64/entry.asm, see M3 in milestones.md).
;
; Execution order here does NOT match the milestone checklist order
; (A20 -> GDT/protected mode -> E820) because BIOS interrupts only work in
; real mode: the E820 memory map and the kernel binary itself must both be
; read FIRST, before we ever touch CR0.PE. Everything else follows in the
; order it has to happen:
;   1. (16-bit real mode) print status, collect E820 map, load the kernel
;      binary from disk into a low scratch buffer, enable A20
;   2. (16-bit real mode) load GDT, set CR0.PE -> 32-bit protected mode
;   3. (32-bit protected mode) build identity-mapped page tables, enable
;      PAE, set EFER.LME, enable paging -> now in long mode
;   4. (32-bit protected mode) far jump to a 64-bit code segment
;   5. (64-bit long mode) copy the kernel from its real-mode-addressable
;      scratch buffer up to its real load address (1 MiB — real mode can't
;      address that directly, so it has to happen after paging is live and
;      addressing is flat), then jump straight into the kernel's entry
;      point with a pointer to the E820 map in RDI, matching the System V
;      AMD64 ABI's first-argument register.
;
; Memory map for structures this stage builds at fixed, page-aligned
; low-memory addresses (all safely within the first 2 MiB we identity-map,
; and below stage2 itself which starts at 0x7E00):
;   0x1000            PML4 (1 page)
;   0x2000            PDPT (1 page)
;   0x3000            PD   (1 page, 512 * 2 MiB entries = first 1 GiB)
;   0x9000            E820 entry count (dword)
;   0x9008            E820 entries (24 bytes each, up to MAX_E820_ENTRIES)
;   0x10000           kernel scratch load buffer (real-mode addressable;
;                     capped at 64 KiB for now — see KERNEL_SECTOR_COUNT
;                     guard below)
; The kernel's real home, 0x100000 (1 MiB), is only reachable once paging
; is live, hence the two-step real-mode-load-then-copy dance.

bits 16
org 0x7E00

PML4_ADDR         equ 0x1000
PDPT_ADDR         equ 0x2000
PD_ADDR           equ 0x3000

E820_COUNT_ADDR   equ 0x9000
E820_ENTRIES_ADDR equ 0x9008
MAX_E820_ENTRIES  equ 64

KERNEL_SCRATCH_SEGMENT equ 0x1000
KERNEL_SCRATCH_PHYS    equ 0x10000    ; = KERNEL_SCRATCH_SEGMENT * 16
KERNEL_START_LBA       equ 9          ; LBA 0 = stage1, LBA 1..8 = stage2
KERNEL_LOAD_ADDR        equ 0x100000  ; 1 MiB — matches kernel/linker.ld
MAX_SECTORS_PER_READ    equ 64        ; conservative per-call chunk size

; KERNEL_SECTOR_COUNT is passed in by the Makefile (`nasm -D
; KERNEL_SECTOR_COUNT=<n>`), computed from the actual built kernel.bin
; size. The fallback here only matters for a manual `nasm` invocation
; outside the Makefile.
%ifndef KERNEL_SECTOR_COUNT
%define KERNEL_SECTOR_COUNT 32
%endif
; load_kernel's segment-bumping loop (below) already handles a scratch
; buffer spanning many 64 KiB real-mode segments correctly - cur_segment
; just keeps advancing every time cur_offset overflows, all the way up to
; just past the 1 MiB mark where 16-bit segment arithmetic itself would
; start to wrap. The real ceiling on kernel size isn't that loop, it's
; that the kernel has to stay clear of leanfs's on-disk region (M12),
; which starts at sector 2048 (kernel/fs/leanfs.c's LEANFS_START_LBA,
; mirrored in the top-level Makefile's FS_START_LBA and enforced there
; too, at final-image-build time, using the *actual* built size rather
; than this compile-time guard's necessarily-conservative constant).
%if KERNEL_SECTOR_COUNT > 2048
%error "kernel.bin has grown into leanfs's on-disk region (LBA 2048) - see FS_START_LBA in the top-level Makefile"
%endif

VGA_BASE          equ 0xB8000

CODE32_SEL equ gdt_code32 - gdt_start
DATA32_SEL equ gdt_data32 - gdt_start
CODE64_SEL equ gdt_code64 - gdt_start

start:
    mov si, msg_stage2
    call print_string_rm

    call collect_e820_map
    call load_kernel
    call enable_a20

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE32_SEL:protected_mode_entry

; ---------------------------------------------------------------------
; 16-bit real mode helpers
; ---------------------------------------------------------------------

; print_string_rm: BIOS teletype-print a null-terminated string.
; In: SI = pointer to string
print_string_rm:
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

; collect_e820_map: enumerates the BIOS memory map via INT 15h, EAX=E820h
; and stores it at E820_COUNT_ADDR / E820_ENTRIES_ADDR (ES is 0 here, set
; by stage1, so ES:DI addresses match the flat physical addresses above).
collect_e820_map:
    pusha
    xor ebx, ebx
    xor bp, bp                     ; bp = entries found so far
    mov di, E820_ENTRIES_ADDR
.loop:
    mov eax, 0xE820
    mov edx, 0x534D4150            ; 'SMAP'
    mov ecx, 24
    mov dword [es:di + 20], 1      ; default ACPI 3.x attribute if the BIOS
                                    ; only returns a 20-byte entry
    int 0x15
    jc .done                       ; CF set: error, or no more entries
    cmp eax, 0x534D4150
    jne .done                      ; signature mismatch: bail out
    inc bp
    add di, 24
    cmp bp, MAX_E820_ENTRIES
    jae .done
    test ebx, ebx
    jnz .loop                      ; EBX = 0 means that was the last entry
.done:
    mov [E820_COUNT_ADDR], bp
    popa
    ret

; load_kernel: reads KERNEL_SECTOR_COUNT sectors starting at LBA
; KERNEL_START_LBA into the scratch buffer at
; KERNEL_SCRATCH_SEGMENT:0000, chunked at MAX_SECTORS_PER_READ sectors
; per INT 13h call and bumping the destination segment every 64 KiB so a
; 16-bit segment:offset pair never has to represent more than that.
load_kernel:
    pusha

    mov word [cur_segment], KERNEL_SCRATCH_SEGMENT
    mov word [cur_offset], 0
    mov dword [cur_lba], KERNEL_START_LBA
    mov cx, KERNEL_SECTOR_COUNT     ; sectors remaining
.loop:
    cmp cx, 0
    je .done

    mov ax, cx
    cmp ax, MAX_SECTORS_PER_READ
    jbe .chunk_size_ok
    mov ax, MAX_SECTORS_PER_READ
.chunk_size_ok:
    mov [dap.count], ax
    mov ax, [cur_offset]
    mov [dap.offset], ax
    mov ax, [cur_segment]
    mov [dap.segment], ax
    mov eax, [cur_lba]
    mov [dap.lba_low], eax
    mov dword [dap.lba_high], 0

    mov si, dap
    mov ah, 0x42
    int 0x13
    jc .disk_error

    mov ax, [dap.count]
    sub cx, ax

    movzx eax, word [dap.count]
    add [cur_lba], eax

    mov ax, [dap.count]
    shl ax, 9                       ; sectors -> bytes (count <= 64, so <= 32768, fits in ax)
    add [cur_offset], ax
    jnc .loop
    mov ax, [cur_segment]
    add ax, 0x1000                  ; +0x1000 segment = +0x10000 linear (64 KiB)
    mov [cur_segment], ax
    jmp .loop

.done:
    popa
    ret

.disk_error:
    mov si, msg_disk_error
    call print_string_rm
    jmp halt

; enable_a20: fast A20 gate via port 0x92. Good enough for QEMU and most
; real hardware; a from-scratch OS aiming for wider real-hardware support
; would also try the keyboard-controller and BIOS (INT 15h, AX=2401h)
; methods as fallbacks — left as a stretch-goal hardening pass.
enable_a20:
    pusha
    in al, 0x92
    test al, 2
    jnz .done
    or al, 2
    and al, 0xFE                   ; avoid bit 0: can fast-reset some chipsets
    out 0x92, al
.done:
    popa
    ret

halt:
    cli
    hlt
    jmp halt

; ---------------------------------------------------------------------
; 32-bit protected mode
; ---------------------------------------------------------------------
bits 32
protected_mode_entry:
    mov ax, DATA32_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7C00                 ; stage1 is done with this memory now

    call build_page_tables
    call enter_long_mode

    jmp CODE64_SEL:long_mode_entry

; build_page_tables: identity-maps the first 1 GiB using 2 MiB pages
; (PML4[0] -> PDPT[0] -> PD[0..511]).
build_page_tables:
    ; zero PML4, PDPT, PD (3 pages)
    mov edi, PML4_ADDR
    mov ecx, (3 * 4096) / 4
    xor eax, eax
    rep stosd

    mov edi, PML4_ADDR
    mov eax, PDPT_ADDR
    or eax, 0x3                     ; present | writable
    mov [edi], eax

    mov edi, PDPT_ADDR
    mov eax, PD_ADDR
    or eax, 0x3
    mov [edi], eax

    mov edi, PD_ADDR
    mov eax, 0x83                   ; present | writable | PS (2 MiB page)
    mov ecx, 512
.fill_pd:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .fill_pd
    ret

; enter_long_mode: PAE + CR3 + EFER.LME + paging. After this returns, the
; CPU is in IA-32e (long) mode but still running 32-bit code until the far
; jump reloads CS with a 64-bit code segment.
enter_long_mode:
    mov eax, cr4
    or eax, 1 << 5                  ; CR4.PAE
    mov cr4, eax

    mov eax, PML4_ADDR
    mov cr3, eax

    mov ecx, 0xC0000080              ; IA32_EFER
    rdmsr
    or eax, 1 << 8                   ; EFER.LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31                  ; CR0.PG
    mov cr0, eax
    ret

; ---------------------------------------------------------------------
; 64-bit long mode: copy the kernel to its final address and jump in
; ---------------------------------------------------------------------
bits 64
long_mode_entry:
    mov ax, DATA32_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov rsi, KERNEL_SCRATCH_PHYS
    mov rdi, KERNEL_LOAD_ADDR
    mov rcx, KERNEL_SECTOR_COUNT * 512
    cld
    rep movsb

    mov rdi, E820_COUNT_ADDR         ; first-arg register per System V ABI
    jmp KERNEL_LOAD_ADDR             ; flat binary: byte 0 is the entry point

; ---------------------------------------------------------------------
; Data
; ---------------------------------------------------------------------
msg_stage2:     db "lean_os stage2: loading kernel, entering long mode...", 13, 10, 0
msg_disk_error: db "lean_os stage2: KERNEL DISK READ ERROR", 13, 10, 0

cur_segment: dw 0
cur_offset:  dw 0
cur_lba:     dd 0

; Disk Address Packet for INT 13h, AH=42h (extended read). Shared between
; collect_e820_map's sibling load_kernel and any future real-mode readers.
align 4
dap:
    .size     db 0x10
    .reserved db 0
    .count    dw 0
    .offset   dw 0
    .segment  dw 0
    .lba_low  dd 0
    .lba_high dd 0

align 8
gdt_start:
gdt_null:
    dq 0
gdt_code32:
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
gdt_data32:
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_code64:
    dw 0x0000, 0x0000
    db 0x00, 10011010b, 00100000b, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; Pad out to exactly the number of sectors stage1 reads for us. If this
; ever overflows, nasm errors out on the negative fill (a natural size guard).
times (8*512) - ($ - $$) db 0
