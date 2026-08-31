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

    .text : {
        *(.text*)
    } :text

    .rodata : {
        *(.rodata*)
    } :text

    . = ALIGN(0x1000);

    .data : {
        *(.data*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss*)
    } :data
}
