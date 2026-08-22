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

ENTRY(_start)

SECTIONS
{
    . = 0x8000000000;

    .text : {
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
}
