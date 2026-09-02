/* user_space/libc/src/tls.c - M96
 *
 * Thread-local storage: what makes `__thread` a variable each thread has
 * its own of, and what makes `errno` per-thread rather than a lie two
 * threads share.
 *
 * ---- the layout, because it is upside down --------------------------
 *
 * On x86-64 the thread pointer (%fs base) points at a Thread Control
 * Block, and the thread's TLS block sits **below** it, at negative
 * offsets. `%fs:0` holds the thread pointer itself - a self-pointer -
 * because some code needs the absolute address and there is no
 * instruction to read a segment base in ring 3.
 *
 *     low                                                     high
 *     +-------------------------------+--------------------------+
 *     | this thread's copy of .tdata  |  TCB                     |
 *     | and .tbss  (tls_total bytes)  |  [0] = the pointer here  |
 *     +-------------------------------+--------------------------+
 *                                     ^
 *                                     tp, written to IA32_FS_BASE
 *
 * A `__thread int x` becomes `mov %fs:-4, %eax`, and the -4 is computed
 * by the LINKER from the PT_TLS segment - which is why
 * user_space/lib/user.ld has to declare one even though nothing loads
 * it. Getting the direction backwards produces a program that reads
 * whatever is above the TCB, which is heap, and looks like memory
 * corruption rather than like a TLS bug.
 *
 * ---- which models are supported -------------------------------------
 *
 * local-exec and initial-exec, which is what a static executable uses
 * and is everything this machine can produce today. The general-dynamic
 * model needs `__tls_get_addr` and a module list, and both belong with
 * the dynamic loader (M95) rather than here: a `dlopen`ed object's TLS
 * block does not exist until the object is loaded, which is the whole
 * reason that model exists. Said here so that "TLS works" is not read as
 * a larger claim than it is.
 */
#include <stdlib.h>
#include <string.h>

#include "syscall_wrappers.h"
#include "proc.h" /* system_api/include/proc.h - ARCH_SET_FS */

/* Defined by the linker script. `__lean_tls_init_*` bracket the bytes to
 * copy; `__lean_tls_end` is where the zeroed part stops. All three are
 * addresses in this image, not sizes. */
extern char __lean_tls_init_start[];
extern char __lean_tls_init_end[];
extern char __lean_tls_end[];

size_t __lean_tls_init_size(void) {
    return (size_t)(__lean_tls_init_end - __lean_tls_init_start);
}

size_t __lean_tls_total_size(void) {
    return (size_t)(__lean_tls_end - __lean_tls_init_start);
}

/* Builds one thread's block and points this task's %fs at it.
 *
 * Returns the allocation so a caller that owns the thread can free it,
 * or NULL. A program with no `__thread` variables at all still gets a
 * TCB: `%fs:0` has to be readable, because that is how any code that
 * wants the thread pointer gets it.
 *
 * 64-byte aligned, which covers every alignment a `__thread` variable
 * can ask for on this machine short of a page. An over-aligned one would
 * be silently misaligned, so the alignment is stated rather than
 * derived - deriving it needs p_align out of PT_TLS, which is in the
 * ELF header this program cannot see.
 */
void *__lean_tls_setup(void) {
    size_t total = __lean_tls_total_size();
    size_t init = __lean_tls_init_size();
    /* The TCB is one pointer, and the block is what precedes it. Rounded
     * up so the thread pointer itself is aligned. */
    size_t aligned_total = (total + 63u) & ~(size_t)63u;
    size_t bytes = aligned_total + 64;

    char *block = (char *)malloc(bytes);
    if (!block) {
        return 0;
    }
    memset(block, 0, bytes);
    char *tp = block + aligned_total;
    if (init > 0) {
        /* The initialized part sits at the BOTTOM of the block, which is
         * tp - total and not tp - aligned_total: the padding introduced
         * by the alignment goes below the variables, not between them
         * and the thread pointer, or every offset the linker computed
         * would be wrong by the padding. */
        memcpy(tp - total, __lean_tls_init_start, init);
    }
    *(void **)tp = tp; /* the self-pointer at %fs:0 */

    if (sys_arch_prctl(ARCH_SET_FS, (unsigned long)tp) != 0) {
        free(block);
        return 0;
    }
    return block;
}
