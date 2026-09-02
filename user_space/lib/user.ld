/* user_space/lib/user.ld
 *
 * Linker script every user program builds with. Produces a real ET_EXEC
 * ELF64 executable (not objcopy'd flat like the kernel's own image) since
 * kernel/proc/elf.c's loader parses these program headers directly.
 * Load address (512 GiB) lands inside the PML4[1]-rooted region
 * kernel/proc/proc.c reserves exclusively for a process's own private
 * mappings - see vmm_create_address_space's header comment in mm/vmm.h
 * for why that has to be a slot the kernel's own PML4[0] never shares.
 */

/* M91: two PT_LOAD segments rather than one, and this is what makes the
 * NX bit mean anything for a program.
 *
 * Every binary this project has ever produced had a single RWE segment -
 * `readelf -l` on any of them said so - because the script said nothing
 * about program headers and ld merged everything into one. Page
 * permissions are per page, so a loader honouring p_flags on that image
 * would map the whole program writable AND executable, which is no
 * protection at all. Splitting text and rodata (R+X) from data and bss
 * (R+W) is the smallest change that gives kernel/proc/elf.c something
 * true to honour, and the ALIGN is not cosmetic: the two segments must
 * not share a page, or the page they share has to be given the union of
 * both sets of rights. */
PHDRS
{
    text PT_LOAD FLAGS(5); /* PF_R | PF_X */
    data PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

ENTRY(_start)

SECTIONS
{
    . = 0x8000000000;

    /* M94: .init and .fini, in that order and before .text.
     *
     * They are built out of fragments contributed by several objects -
     * see crti.asm - so the link script has to name them, or ld keeps
     * them as orphan sections in whatever order it pleases and the
     * prologue and epilogue stop bracketing anything. */
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

    . = ALIGN(0x1000);

    /* M94: the constructor and destructor arrays, with symbols around
     * them so the runtime can walk them.
     *
     * This is the modern half of the same mechanism .init is the ancient
     * half of: a compiler puts a pointer to each constructor here rather
     * than a call into .init, and something has to walk the array. The
     * KEEP matters - these sections are referenced only by the symbols
     * below, so --gc-sections would otherwise discard every constructor
     * in the program.
     *
     * **.ctors and .dtors are kept OUT of these arrays**, and that is
     * the correction rather than an omission. They are the pre-2000
     * spelling of the same idea and it is tempting to merge them in;
     * doing so is wrong, because a .ctors list is bracketed by sentinel
     * words - -1 at the front and 0 at the back - which .init_array is
     * not. Merged, the runtime's walk calls address 0xffffffffffffffff
     * as its first constructor. That is exactly what happened: the first
     * program GCC's own crtbegin.o was ever linked into died at
     * rip=0xFFFFFFFFFFFFFFFF before main, and the ELF's own
     * `.init_array` began with `ffffffff ffffffff`.
     *
     * They get their own output sections so a stray one from an old
     * object file is visible in `readelf -S` rather than silently
     * discarded - and nothing walks them, which is the honest state:
     * this runtime supports .init_array, and the compiler is configured
     * with --enable-initfini-array so that is what it emits. */
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

    .data : {
        *(.data*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss*)
    } :data
}
