; kernel/arch/x86_64/ap_trampoline.asm
;
; Standalone 16-bit real-mode AP bring-up blob - NOT linked into kernel.elf
; (see the Makefile: it's built on its own with `nasm -f bin`, the same way
; kernel/boot/mbr.asm is, then incbin'd into the kernel image by
; kernel/proc/embed_ap_trampoline.asm, the same pattern embed_programs.asm
; already uses for every user ELF binary).
;
; kernel/arch/x86_64/smp.c copies these bytes to physical
; AP_TRAMPOLINE_LOAD_ADDR (0x8000) right before sending an AP its
; INIT-SIPI-SIPI sequence. A STARTUP IPI's vector operand IS the target
; physical address divided by 4 KiB, and the CPU begins executing there in
; 16-bit real mode with CS:IP = (vector << 8):0000 - i.e. every AP starts in
; real mode and has to walk itself up to long mode regardless of how the
; BSP itself booted (an x86 architectural fact about STARTUP IPIs, not tied
; to BIOS or UEFI). This blob is that walk: no BIOS calls needed (an AP has
; no BIOS state of its own to call into - no A20 gate to touch either,
; since the BSP already enabled it for the whole machine), just reading its
; boot parameters from a fixed low-memory struct at AP_PARAMS_ADDR (0x7000)
; that smp.c fills in fresh before every SIPI - see smp.c's own header
; comment for the exact byte layout both sides agree on.
;
; This blob only needs its OWN transient 32/64-bit GDT to reach long mode -
; the descriptor bytes below are a standard flat code32/data32/code64 GDT
; (same encoding a from-scratch long-mode bootstrap always needs - null,
; flat 32-bit code, flat 32-bit data, flat 64-bit code - proven correct by
; every AP bring-up this kernel has done since M7's SMP support landed).
; The instant 64-bit mode is reached, it switches straight to the kernel's
; REAL GDT (read out of AP_PARAMS_ADDR, the exact same table/selectors
; every other CPU already runs under - see gdt.c's gdt_get_table_ptr) and
; jumps to kernel/arch/x86_64/ap_entry.asm's ap_entry_asm_stub, a normal
; linked kernel symbol whose address smp.c also wrote into AP_PARAMS_ADDR.

bits 16
org 0x8000
default abs ; explicit: every [ADDR + off] memory operand below (including
             ; in the bits64 section) means the flat absolute address, not
             ; RIP-relative - nasm's own default, made explicit to silence
             ; its "implicit DEFAULT ABS is deprecated" warning.

AP_PARAMS_ADDR   equ 0x7000
AP_OFF_CR3       equ 0
AP_OFF_ENTRY64   equ 16
AP_OFF_GDT_LIMIT equ 24
AP_OFF_GDT_BASE  equ 26

CODE32_SEL equ trampoline_gdt_code32 - trampoline_gdt_start
DATA32_SEL equ trampoline_gdt_data32 - trampoline_gdt_start
CODE64_SEL equ trampoline_gdt_code64 - trampoline_gdt_start

ap_trampoline_entry:
    cli
    ; Normalize CS to 0: SIPI leaves CS = vector<<8 (0x0800 for vector
    ; 0x08), IP = 0 - every label below, compiled under `org 0x8000`,
    ; assumes a zero segment base, so every reference to one has to happen
    ; with CS actually 0 first. A far jump with an explicit segment is the
    ; standard way to reload CS in real mode.
    jmp 0x0000:normalize

normalize:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7FF0          ; scratch stack just below AP_PARAMS_ADDR/this
                              ; blob - nothing here ever uses `call`, so this
                              ; barely matters, but it's there for `push`
                              ; safety and never touched again once 64-bit
                              ; mode takes over below.

    lgdt [trampoline_gdt_descriptor]

    mov eax, cr0
    or eax, 1                ; CR0.PE
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
    or eax, 1 << 5            ; CR4.PAE
    mov cr4, eax

    mov eax, [AP_PARAMS_ADDR + AP_OFF_CR3]
    mov cr3, eax              ; the kernel's real PML4 - already maps the
                                ; heap (this AP's real stack lives there)
                                ; and every kernel-linked address, since
                                ; PML4[0] is shared by every address space
                                ; in this kernel (see vmm.c)

    mov ecx, 0xC0000080        ; IA32_EFER
    rdmsr
    or eax, 1 << 8              ; EFER.LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31             ; CR0.PG
    mov cr0, eax

    jmp CODE64_SEL:ap_start64

bits 64
ap_start64:
    mov ax, DATA32_SEL          ; still a valid flat data selector in long
                                  ; mode - segment bases/limits are ignored
                                  ; for DS/ES/SS/FS/GS here, only the
                                  ; present/writable/DPL bits matter, and
                                  ; this descriptor already has the right
                                  ; ones
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Retire this blob's own transient GDT for the kernel's real one -
    ; the same table/selectors (GDT_KERNEL_CODE_SEL=0x08/
    ; GDT_KERNEL_DATA_SEL=0x10, kernel/arch/x86_64/gdt.h) every other CPU
    ; already runs under.
    lgdt [AP_PARAMS_ADDR + AP_OFF_GDT_LIMIT]
    mov ax, 0x10                 ; GDT_KERNEL_DATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rax, [AP_PARAMS_ADDR + AP_OFF_ENTRY64]
    jmp rax                       ; -> kernel/arch/x86_64/ap_entry.asm's ap_entry_asm_stub

; ---------------------------------------------------------------------
; This blob's own transient GDT - only used to reach 64-bit mode above;
; retired the instant ap_start64 loads the kernel's real one.
; ---------------------------------------------------------------------
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
