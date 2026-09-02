/* user_space/ld/ld-start.S - M95
 *
 * The dynamic linker's own entry point, in assembly for three reasons
 * that are each unavoidable:
 *
 *   1. It has to find its OWN load address before any C code runs. There
 *      is no global it could read that would be correct yet - a global's
 *      address is exactly what has not been relocated - so the base is
 *      computed from the instruction pointer with a RIP-relative lea
 *      against a symbol whose link-time address is known.
 *   2. It has to preserve RDI across the whole load. The kernel put the
 *      argument region there, and the program's crt0 expects to find it
 *      there - so a dynamic program and a static one look identical from
 *      crt0 down. That is the property that makes this linker invisible.
 *   3. The final transfer is a JMP and not a CALL. There is nothing to
 *      return to: the program's entry ends in exit().
 */
    /* _DYNAMIC is defined by the linker in every ET_DYN it produces,
     * and is weak+hidden here so that referring to it before it exists
     * is a link that succeeds rather than one that fails on a symbol the
     * linker is about to create. Hidden because it is this object's own
     * and must never be resolved against another's. */
    .hidden _DYNAMIC

    .text
    .globl _start
    /* Hidden, and this is not cosmetic. A shared object's global symbols
     * are interposable by default, so `lea _start(%rip)` is a
     * PC-relative reference ld refuses to make - it cannot know at link
     * time which definition wins. Hidden means "this object's own,
     * never interposed", which is exactly true of an entry point and is
     * what lets the reference be PC-relative and therefore need no
     * relocation. Without it the link fails with "relocation
     * R_X86_64_PC32 against symbol `_start' can not be used when making
     * a shared object", which reads as a compile-flag problem and is
     * not. */
    .hidden _start
    .type _start, @function
_start:
    /* Save the argument region - RDI is the one thing the kernel gave
     * us and the one thing the program must get. */
    movq    %rdi, %rbx

    /* This object's load base. `_DYNAMIC` and `_start` both have
     * link-time addresses relative to 0, so the difference between where
     * _start actually IS and where it was linked is the bias. lea with
     * RIP-relative addressing is the only way to ask "where am I" on
     * x86-64, and it is why this cannot be C. */
    leaq    _start(%rip), %rdi      /* actual address of _start */
    leaq    __ld_start_linked(%rip), %rax
    subq    (%rax), %rdi            /* rdi = base */

    leaq    _DYNAMIC(%rip), %rsi    /* &_DYNAMIC, already biased by RIP */
    call    _dl_relocate_self

    /* From here globals work. Load the program. */
    movq    %rbx, %rdi
    call    _dl_entry

    /* _dl_entry returned the program's entry point. RDI back to the
     * argument region, and go - see reason 3. */
    movq    %rbx, %rdi
    jmpq    *%rax
    .size _start, .-_start

    /* The link-time address of _start, as data. Read through a
     * RIP-relative lea (which needs no relocation) and holding an
     * absolute address (which needs one) - so this word is itself
     * fixed up by an R_X86_64_RELATIVE that has NOT been applied yet
     * when it is read.
     *
     * That is deliberate and is the trick: an unrelocated RELATIVE slot
     * holds its addend, which is the link-time address. Reading it
     * before relocation gives exactly the number this needs, and reading
     * it after would give the runtime address and a bias of zero. */
    .section .data.rel.ro
    .align 8
__ld_start_linked:
    .quad _start
