/* user_space/bin/oomtest.c - M102
 *
 * Asks the machine for more memory than it has, and reports what
 * happened.
 *
 * ---- Why this is a program and not a self-test in kernel.c -----------
 *
 * Because the thing under test is what a *ring-3* program experiences
 * when the machine runs out, and a kernel thread cannot experience it.
 * The allocations a kernel self-test makes come from the kernel heap and
 * would take the machine's own memory down with them; the ones here come
 * through SYS_mmap and the page-fault path, which is where a real program
 * meets the limit and where M102's OOM kill lives.
 *
 * ---- The two halves, which fail differently and both matter ----------
 *
 * **Reserving** address space (SYS_mmap) does not consume memory on this
 * machine - M82 made mappings demand-paged - so a reservation the machine
 * cannot honour succeeds, and the failure arrives later. That is not a
 * bug and it is not this program's job to complain about it; it is the
 * same overcommit every Unix does, and it is why the second half exists.
 *
 * **Touching** it is where the memory is actually taken. A write to a
 * reserved page that the kernel cannot back is the exact fault M102
 * turned from a machine halt into a process kill. So this program touches
 * what it reserves, and the expected outcome is that *this program dies*
 * and the machine does not.
 *
 * ---- What "pass" means here ------------------------------------------
 *
 * There are two acceptable endings and one unacceptable one:
 *
 *   - mmap starts refusing (returns MAP_FAILED). The program exits 0
 *     having proved the reservation path reports failure.
 *   - the program is killed touching a page the kernel cannot back.
 *     It never gets to print anything, and kernel.c grades the exit
 *     status: SIGKILL, from the [oom] path.
 *
 * The unacceptable ending is the machine stopping, and that one is graded
 * by the boot continuing at all.
 *
 * Exit 0 means "the allocation path refused cleanly"; being killed is
 * equally a pass and is graded by the caller. Exit 1 means this program
 * reached a state it should not have.
 */
#include "syscall_wrappers.h"

#include <stdio.h>
#include <sys/mman.h>

/* 64 MiB a time, and the size is set by a limit rather than by taste:
 * a process gets MAX_MMAP_REGIONS (128) mappings, so the most it can
 * reserve is 128 times this. At 4 MiB that was 512 MiB - less than this
 * machine has, so the test ran out of *regions* instead of memory and
 * proved the wrong thing. At 64 MiB the ceiling is 8 GiB, which is more
 * than QEMU_MEM is ever set to here. */
#define CHUNK (64u * 1024 * 1024)

/* A ceiling, so that a machine with far more memory than this test
 * expects stops rather than running until the harness times out. At
 * 4 MiB a chunk this is 8 GiB, which is more than QEMU_MEM is ever set
 * to here and more than M90 tracks. */
#define MAX_CHUNKS 128

int main(void) {
    unsigned long touched_bytes = 0;
    int chunks = 0;

    for (chunks = 0; chunks < MAX_CHUNKS; chunks++) {
        void *p = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("oomtest: mmap refused after %d chunks (%lu KiB touched) - "
                   "the reservation path reports failure\n",
                   chunks, touched_bytes / 1024);
            return 0;
        }

        /* One byte per page is enough to make the kernel find a frame for
         * it: this is the fault path, not a memcpy benchmark, and
         * touching every byte would spend the machine's time proving
         * something the first byte already proved. */
        volatile unsigned char *bytes = (volatile unsigned char *)p;
        for (unsigned int off = 0; off < CHUNK; off += 4096) {
            bytes[off] = 0xA5;
            touched_bytes += 4096;
        }
    }

    printf("oomtest: reserved and touched %d chunks (%lu KiB) without being "
           "refused or killed - this machine has more memory than this test "
           "was built to exhaust\n", chunks, touched_bytes / 1024);
    return 1;
}
