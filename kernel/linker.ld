/* kernel/linker.ld
 *
 * Links the kernel as a flat image starting at 1 MiB, matching
 * KERNEL_LOAD_ADDR in kernel/boot/uefi/boot.c and the identity-mapped low
 * memory the boot loader sets up before jumping here. _start (entry.asm)
 * is placed first so it lands at byte 0 of the objcopy'd flat binary.
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

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
