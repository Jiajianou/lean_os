ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : {
        __kernel_text_start = .;
        *(.text.entry)
        *(.text*)
        __kernel_text_end = .;
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

    /* OVMF keeps memory just above 8 MiB, and BOOTX64 has to reserve the whole
       image - zeroed data included - from 1 MiB up; past this line the boot
       stops at "could not reserve kernel load address". Found twice by
       booting (M198, M207); this finds it at link time. */
    ASSERT(__kernel_end <= 0x800000, "the kernel image and its bss must end below 8 MiB - move a large static array to the heap")

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
