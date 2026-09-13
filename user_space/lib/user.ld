SEARCH_DIR("=/usr/lib");

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    tls  PT_TLS  FLAGS(4);
}

ENTRY(_start)

SECTIONS
{
    . = 0x8000000000;

    .init : {
        KEEP(*(.init))
    } :text

    .fini : {
        KEEP(*(.fini))
    } :text

    .text : {
        *(.text*)
    } :text

    .rodata : {
        *(.rodata*)
    } :text

    .eh_frame : {
        KEEP(*(.eh_frame))
        KEEP(*(.eh_frame.*))
    } :text

    .gcc_except_table : {
        KEEP(*(.gcc_except_table))
        KEEP(*(.gcc_except_table.*))
    } :text

    . = ALIGN(0x1000);

    . = ALIGN(8);
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :data

    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data

    .ctors : { KEEP(*(SORT(.ctors))) } :data
    .dtors : { KEEP(*(SORT(.dtors))) } :data

    . = ALIGN(64);
    __lean_tls_init_start = .;
    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls
    __lean_tls_init_end = .;
    .tbss : {
        *(.tbss .tbss.*)
        *(.tcommon)
    } :data :tls
    __lean_tls_end = ADDR(.tbss) + SIZEOF(.tbss);

    .data : {
        *(.data*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss*)
    } :data
}
