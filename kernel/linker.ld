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

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
