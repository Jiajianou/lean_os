/* kernel/linker.ld
 *
 * Links the kernel as a flat image starting at 1 MiB, matching
 * KERNEL_LOAD_ADDR in kernel/boot/uefi/boot.c and the identity-mapped low
 * memory the boot loader sets up before jumping here. _start (entry.asm)
 * is placed first so it lands at byte 0 of the objcopy'd flat binary.
 *
 * M40: __bss_start/__kernel_end bracket the zero-initialized region that
 * entry.asm clears by hand before calling kernel_main. .bss is NOBITS, so
 * objcopy -O binary leaves it out of kernel.bin entirely and the UEFI
 * loader only ever reads the file's own bytes into memory - which means
 * nothing was providing C's "statics start zeroed" guarantee at all. That
 * held up purely because the firmware happened to leave zeros there, and
 * stopped holding the moment a struct grew (see entry.asm's own comment).
 * Both symbols are 8-byte aligned so the clearing loop can move whole
 * quadwords without a tail case.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : {
        *(.text.entry)
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    . = ALIGN(8);
    __bss_start = .;
    .bss : {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(8);
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
