/* system_api/include/mman.h
 *
 * M78: SYS_mmap's `prot` and `flags`, shared by the kernel and by
 * <sys/mman.h> in user space so the two cannot hold different opinions
 * about what a bit means.
 *
 * The numbers match Linux's, as every other constant in system_api does
 * where there is a convention to match - a program written elsewhere
 * that passes PROT_READ|PROT_WRITE means here what it meant there.
 */
#pragma once

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
/* M91: real. This said, from M78 until M91, "accepted and neither granted
 * nor withheld: this kernel's page tables have no NX bit set up, so every
 * mapping is executable whether or not anyone asked" - which was true and
 * is the sentence this milestone existed to delete. There is an
 * execute-disable bit now (kernel/mm/vmm.c's PTE_NX, enabled per CPU
 * through EFER.NXE), so a mapping without PROT_EXEC cannot be executed
 * and one with it can. PROT_NONE likewise stopped being a refused
 * argument and became a guard page. */
#define PROT_EXEC  0x4

#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
/* Named so that a caller passing it is refused by name rather than
 * silently handed private memory. There are no shared mappings here:
 * kernel/ipc/shm.h is what two processes share memory through, and it
 * has its own lifetime rules that a MAP_SHARED would have to duplicate
 * badly. */
#define MAP_SHARED    0x01
#define MAP_FIXED     0x10

#define MAP_FAILED ((void *)-1)

/* M91: madvise. MADV_DONTNEED is the only one that does anything, and it
 * is the only one that is not really advice - it drops the frames and
 * leaves the mapping, so the next touch reads zeroes. The rest are
 * accepted as no-op successes: there is no page cache here for
 * MADV_WILLNEED to warm, and a program that fails because it offered a
 * hint is worse off than one whose hint went nowhere. Numbers match
 * Linux's, like everything else in this header. */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
