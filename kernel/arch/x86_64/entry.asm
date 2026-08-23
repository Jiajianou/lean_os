; kernel/arch/x86_64/entry.asm
;
; Kernel entry point. The bootloader (kernel/boot/uefi/boot.c) jumps here
; directly in 64-bit long mode — this is byte 0 of the flat kernel.bin, at
; its linked load address (see kernel/linker.ld). RDI holds a pointer to
; the e820 memory map header and RSI a pointer to the fb_boot_info_t
; framebuffer descriptor (M16) - the System V AMD64 ABI's first and
; second integer-argument registers, matching what boot.c loads them
; with before the jump.

bits 64

extern kernel_main
extern __bss_start
extern __kernel_end

section .text.entry

global _start
_start:
    mov rsp, kernel_stack_top   ; off the bootloader's scratch stack, onto
                                 ; one that's actually part of the kernel
                                 ; image (so the M5 memory manager knows
                                 ; not to hand these pages out)

    ; M40: zero .bss. Every C `static` in this kernel is written assuming
    ; the language's own guarantee that it starts at zero, and until now
    ; literally nothing established that: .bss is NOBITS, so it isn't in
    ; kernel.bin, and boot.c's loader only reads kernel.bin's own bytes
    ; (KERNEL_SECTOR_COUNT sectors) to KERNEL_LOAD_ADDR - the ~176 KiB of
    ; .bss past the end of the file was simply whatever the firmware had
    ; left in that memory. It read as zeros in practice, so the kernel
    ; booted, right up until M40 grew task_t (MAX_FDS 32 -> 64) and pushed
    ; sched.c's tasks[] into a dirtier stretch: the very next process
    ; spawn mapped a shm segment at virtual address 0 (an uninitialized
    ; shm_next_vaddr) and panicked. Real bug, found by growing a struct.
    ;
    ; Runs before any call, so nothing is on the new stack yet - which
    ; matters, because kernel_stack_bottom below is itself in .bss and
    ; gets cleared right along with everything else. RDI/RSI are left
    ; untouched: they carry boot.c's e820 and framebuffer pointers
    ; through to kernel_main, and both point into the loader's own
    ; memory, never into the region being cleared here.
    mov rax, __bss_start
    mov rcx, __kernel_end
.zero_bss:
    cmp rax, rcx
    jae .zero_bss_done
    mov qword [rax], 0
    add rax, 8
    jmp .zero_bss
.zero_bss_done:

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
kernel_stack_bottom:
    resb 16384                  ; 16 KiB kernel stack
kernel_stack_top:
