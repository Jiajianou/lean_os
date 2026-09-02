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
    /* M96: the thread-local segment.
     *
     * PT_TLS is not loaded - the kernel's ELF loader maps PT_LOAD and
     * nothing else, and .tdata below is inside the data PT_LOAD, which
     * is where its initialization image lives. What PT_TLS is FOR is the
     * linker: it is how ld knows there is a TLS block at all, and
     * therefore how it resolves `%fs:`-relative references to
     * `__thread` variables. Without it, every such reference is an
     * error at link time rather than a wrong answer at run time, which
     * is the good direction to fail in. */
    tls  PT_TLS  FLAGS(4); /* PF_R */
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

    /* ---- M97: the tables an exception unwinds through ----------------
     *
     * `.eh_frame` is the DWARF call-frame information the unwinder reads
     * to walk one frame back to its caller, and `.gcc_except_table` is
     * the per-function LSDA that says which catch clauses and which
     * destructors apply to which range of instructions. Neither is code
     * and neither is data the program reads: they exist so that a throw
     * can find its way out.
     *
     * There was no rule for either, and an orphan section is not a
     * no-op - ld places it wherever it likes, which for these two means
     * the unwinder's own scan runs over whatever landed next to it. What
     * the unwinder needs is that the whole of `.eh_frame` is CONTIGUOUS
     * and in link order: crtbegin.o's `__EH_FRAME_BEGIN__` is a label at
     * offset 0 of its (empty) .eh_frame, and crtend.o's `__FRAME_END__`
     * is a four-byte zero terminator, so the registry walks from the
     * first to the terminator and a gap between them is a walk off the
     * end of the table.
     *
     * KEEP on both, because --gc-sections has no way to know that a
     * table nothing references by name is the only thing standing
     * between a throw and std::terminate.
     *
     * Read-only and in the text segment: they are looked at by the
     * unwinder and never written, and putting them in a writable segment
     * would be handing an attacker the map a throw follows. */
    .eh_frame : {
        KEEP(*(.eh_frame))
        KEEP(*(.eh_frame.*))
    } :text

    .gcc_except_table : {
        KEEP(*(.gcc_except_table))
        KEEP(*(.gcc_except_table.*))
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

    /* ---- M96: thread-local storage -------------------------------
     *
     * .tdata holds the initialized `__thread` variables and .tbss the
     * zeroed ones, and together they are the *template* every thread
     * gets its own copy of. The symbols around them are what
     * user_space/libc/src/tls.c reads to make that copy; ld defines no
     * standard ones, so they are defined here.
     *
     * .tbss is NOBITS and occupies no space in the image, which is why
     * `__lean_tls_total` is computed from where .tbss ENDS rather than
     * from where the next section starts - the two differ by exactly the
     * bytes .tbss would have taken, and that difference is the part of
     * the block a thread has to zero.
     *
     * Both sections belong to two program headers: :data because their
     * bytes have to be in the loadable image, and :tls because that is
     * what tells ld a TLS block exists. That double assignment is the
     * whole trick and looks like a mistake if you have not seen it. */
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
    /* NOT `.` - and this is the trap.
     *
     * .tbss is NOBITS, so the location counter does not advance across
     * it: `.` here is exactly where .tdata ended, and a `__lean_tls_end
     * = .` reports a TLS block of zero bytes however many `__thread`
     * variables the program has. The first program built this way had a
     * four-byte errno and a computed total of 0, which means every
     * thread's TLS block is a TCB and nothing else - and `errno` then
     * lands in whatever is below it.
     *
     * ADDR + SIZEOF is exact and is what the location counter cannot
     * be. It is also why .data below starts at the same address .tbss
     * did, which looks wrong in `readelf -S` and is correct: .tbss
     * occupies no space in the image. */
    __lean_tls_end = ADDR(.tbss) + SIZEOF(.tbss);

    .data : {
        *(.data*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss*)
    } :data
}
