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
/* Accepted and neither granted nor withheld: this kernel's page tables
 * have no NX bit set up, so every mapping is executable whether or not
 * anyone asked. Refusing PROT_EXEC would be a refusal with no meaning
 * behind it, and reporting it as enforced would be a fiction - so it is
 * simply accepted, and this comment is the honest version. */
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
