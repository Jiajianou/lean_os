/* kernel/proc/proc.h
 *
 * Turns an in-memory ELF64 image into a running ring-3 task: a fresh
 * private address space (vmm_create_address_space), the image's segments
 * (elf.h), a user stack, an optional single string argument (M13's
 * entire "argv" - see proc.c), and a task (sched.h) whose first action is
 * to drop to ring 3 at the image's entry point.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sched/sched.h"

/* Per-process virtual address space layout - every private (PML4[1])
 * region a process gets, in one place so proc.c (which sets it up) and
 * syscall.c (which bounds-checks growth into it - SYS_sbrk, SYS_shm_map)
 * can't drift apart. Generous gaps between regions rather than packed
 * tightly: nothing here needs to be dense, and it avoids ever having to
 * reason precisely about exact boundaries.
 *
 * ---- M91: the whole map moved, and the numbers are the milestone ------
 *
 * What was here until M91 fitted an entire process into the first gigabyte
 * of a 512 GiB private region: the image at 512 GiB with a 2 MiB ceiling,
 * the stack immediately above it, a 256 MiB heap, a 160 MiB mmap arena,
 * and the shm and framebuffer windows at 512 MiB and 1 GiB pinning the
 * arena's top. Every one of those numbers was chosen when the largest
 * program on this machine was a few hundred kilobytes.
 *
 * `cc1plus` is a hundred megabytes of text. The image window alone was
 * fifty times too small, and because the stack sat directly above the
 * image, growing one meant moving the other. So the map is rebuilt around
 * what the region actually is - half a terabyte of address space, of which
 * this OS was using a fifth of a percent:
 *
 *     512 GiB  +   0        image           64 GiB
 *              +  64 GiB    sbrk heap       64 GiB
 *              + 128 GiB    mmap arena     256 GiB
 *              + 384 GiB    shm window      64 GiB
 *              + 448 GiB    framebuffer
 *              + 496 GiB    stack top, growing down; argv/envp just above
 *     1 TiB                 end of the private region
 *
 * Address space is free until it is touched (M82) and the page tables
 * that describe it are built on demand, so a 256 GiB arena costs exactly
 * what a 160 MiB one did until something reserves inside it. That is the
 * property that makes this an edit to a header rather than a memory
 * budget. */
#define PAGE_SIZE        4096ULL
#define GIB              (1024ULL * 1024 * 1024)
/* The stack grows down from here and the argument region sits just above
 * it, exactly as before - what changed is where "here" is, and that the
 * pages below it arrive on a fault rather than all at spawn time. */
#define USER_STACK_TOP   0x000000FC00000000ULL /* 512 GiB + 496 GiB */
/* M81: 4 -> 16. PATH_MAX_LEN became 4096 this milestone (system_api/
 * include/paths.h), and the programs that walk a filesystem - the file
 * manager above all - hold two or three paths in locals at once. Three
 * of them is three quarters of a 16 KiB stack before anything else, which
 * is the same cliff the kernel's own stacks went over in this milestone
 * and worth not going over twice. 64 KiB costs 48 KiB of frames per
 * process. */
#define USER_STACK_PAGES 16                     /* 64 KiB */
/* M91: how far down the stack may grow before a fault is a fault again.
 *
 * USER_STACK_PAGES is now the *initial* mapping - what a process starts
 * with, still built eagerly at spawn so that a program's very first push
 * does not depend on the fault handler - and this is the ceiling on what
 * sched_fault_fill will add to it. 64 MiB is eight times a Linux default
 * `ulimit -s` and costs nothing until it is used; GCC recurses deeply on
 * generated code and is the reason the number is generous rather than
 * merely adequate. */
#define USER_STACK_MAX_BYTES (64ULL * 1024 * 1024)
#define USER_STACK_LIMIT (USER_STACK_TOP - USER_STACK_MAX_BYTES)
#define USER_ARG_ADDR    USER_STACK_TOP
/* M75: the argument region is two pages, not one, because it now holds
 * an environment as well as an argument vector - and they live in ONE
 * virtually contiguous block on purpose (see process_spawnve's layout
 * note in proc.c): the SysV convention every C runtime already knows is
 * that envp is argv's NULL terminator plus one, so crt0 finds it with an
 * lea rather than with a second address it would have to be told. One
 * page held both only until an environment of any size existed. */
/* ---- M89: thirty-two pages of window, and only the used ones backed ---
 *
 * Two pages was enough for every program this project wrote and is not
 * enough for a program somebody else wrote. `xargs` sizes its batches
 * from sysconf(_SC_ARG_MAX) and refuses to run at all - "command too
 * long" - when the limit is smaller than one command plus the
 * environment, which at 8 KiB it was. That is how this was found: by the
 * middle stage of M89's own five-program pipeline.
 *
 * The window is 128 KiB of *address space*, which costs nothing. What
 * would cost something is 32 frames per spawn, so process_spawnve maps
 * only the pages the vector actually fills - which for every program on
 * this machine is one, making the common case cheaper than the two pages
 * it replaces rather than sixteen times more expensive. See the
 * allocation loop in proc.c, where the count is computed rather than
 * constant. */
#define USER_ARG_PAGES   32
#define USER_ARG_BYTES   (USER_ARG_PAGES * PAGE_SIZE)

/* M75: the ceiling on an environment. Both numbers are about the region
 * above rather than about taste: 8 KiB holds argc, both pointer arrays
 * and every string, so an environment cannot be allowed to claim all of
 * it and leave no room for argv. 64 variables and 4 KiB of text is
 * comfortably more than anything on this machine sets and less than half
 * the region; past either, the environment is truncated at the last
 * whole variable that fits - the same rule argv has, and for the same
 * reason (half a "NAME=value" is a different variable). */
#define USER_ENV_MAX_VARS  64
#define USER_ENV_MAX_BYTES 4096
#define USER_HEAP_START  0x0000009000000000ULL /* 512 GiB + 64 GiB - M19, moved by M91 */
#define USER_HEAP_LIMIT  0x000000A000000000ULL /* 512 GiB + 128 GiB ceiling */
/* M78: the mmap arena - the second memory primitive next to M19's
 * growth-only sbrk. Placed in the gap the layout already left between
 * the heap's ceiling and the shm window rather than beside either, for
 * the same "generous gaps" reason every other constant here is spaced
 * out: nothing needs it to be dense, and it means neither neighbour has
 * to be reasoned about precisely.
 *
 * 128 MiB of *address space*, which is not 128 MiB of memory - mappings
 * are backed by real frames at the moment they are made (this kernel has
 * no page-fault handler that could fill one in later), so the real
 * ceiling is physical and the arena is only ever the room to arrange
 * things in. */
#define USER_MMAP_BASE   0x000000A000000000ULL /* 512 GiB + 128 GiB - M78, moved by M91 */
/* M82: 448 MiB -> 480 MiB, taking the arena from 128 MiB to 160 MiB.
 *
 * M78 sized this when every mapping was backed by a real frame at the
 * moment of the call, so an arena bigger than the machine's memory would
 * have been address space nothing could ever use. Demand paging makes
 * reserved address space nearly free, and the number that matters becomes
 * "can a program reserve more than this machine has?" - which it must be
 * able to, because that is what every program that mmaps assumes.
 *
 * 160 MiB against 128 MiB of RAM is enough to make the point and to test
 * it. The ceiling is USER_SHM_BASE at 512 MiB with a 32 MiB gap left
 * deliberately, so this is now bounded by the next thing in the address
 * space rather than by a policy - growing it further means moving shm and
 * the framebuffer window, which is a bigger change than this milestone
 * needs. */
/* M91: 160 MiB -> 256 GiB.
 *
 * M82's own note explains why the old number was what it was: "going
 * further means moving the shm window and the framebuffer's fixed mapping
 * address, which is a bigger change than this milestone needs." This is
 * the milestone that needs it, and the two windows moved. */
#define USER_MMAP_LIMIT  0x000000E000000000ULL /* 512 GiB + 384 GiB */
#define USER_SHM_BASE    0x000000E000000000ULL /* 512 GiB + 384 GiB - M19, moved by M91 */
#define USER_SHM_LIMIT   0x000000F000000000ULL /* 512 GiB + 448 GiB */
#define USER_FB_BASE     0x000000F000000000ULL /* 512 GiB + 448 GiB - M20, SYS_fb_map's fixed target address */

/* M52: the whole private region, which is exactly what PML4 entry 1
 * covers - [512 GiB, 1 TiB). Every constant above lives inside it, and
 * nothing a process may legitimately hand the kernel a pointer to lives
 * outside it: PML4[0] is the shared kernel map (the identity map and the
 * kernel heap), so an address below this base is by definition either a
 * kernel address or a null-ish one, and either way not the caller's to
 * name. syscall.c's copy_from_user/copy_to_user check this first and the
 * page tables second - the range test is what makes "a kernel address"
 * and "one byte before the user region" errors rather than reads. */
#define USER_REGION_BASE  0x0000008000000000ULL
#define USER_REGION_LIMIT 0x0000010000000000ULL

/* M40: the window an ELF image's own PT_LOAD segments have to fit inside
 * - from the load address user_space/lib/user.ld links every program at,
 * up to (but not into) the stack this file places just above it. elf.c
 * checks every segment against these before mapping anything, because
 * until M40 it checked nothing at all: SYS_spawn will happily hand it any
 * file on disk, and a p_vaddr pointing into the identity-mapped low
 * memory would have mapped kernel pages into a user address space (or
 * panicked the machine on a huge-page collision). Not a security boundary
 * - nothing else in this project is either - but "a user program can take
 * down the kernel by spawning a text file" is a bug at any threat model. */
#define USER_IMAGE_BASE  0x0000008000000000ULL /* 512 GiB - matches user_space/lib/user.ld */
/* M91: was `USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE`, which put a
 * 2 MiB ceiling on a program image because the stack sat immediately
 * above it. The stack is now 496 GiB away and this is a window of its
 * own: 64 GiB, which is not a number anything will reach and is chosen
 * for that reason - an image limit that has to be revisited per program
 * is the thing being fixed, not a number to tune. */
#define USER_IMAGE_LIMIT 0x0000009000000000ULL /* 512 GiB + 64 GiB */

/* M54: hands `pml4_phys` and every frame the process itself owns back to
 * the allocator. Lives here rather than in vmm.c because the decision it
 * makes is entirely about *this* file's layout: the image, the stack and
 * the argument page are the process's, the heap is the process's, and
 * the shm and framebuffer windows are emphatically not - one holds
 * segments another process may still have mapped, the other holds device
 * physical addresses that were never pmm frames and would poison the
 * free list. vmm.c has no business knowing any of that.
 *
 * Never call this on the address space the calling CPU is running on;
 * task_exit_with_code switches to the kernel's own first. */
void process_destroy_address_space(uint64_t pml4_phys);

/* M83: a copy-on-write clone of `src_pml4_phys`, over exactly the ranges
 * process_destroy_address_space frees. Returns the new PML4's physical
 * address, or 0. */
uint64_t process_fork_address_space(uint64_t src_pml4_phys);

/* M84: an address space holding `image`, a stack and an argument region
 * built from `argv`/`envp` - everything a process needs in memory, with
 * no task attached. Returns the PML4's physical address and writes the
 * ELF entry point to *out_entry, or 0. Used by a spawn to make a new
 * process and by execve to replace an existing one's memory without
 * touching its identity. */
uint64_t process_build_address_space(const uint8_t *image, size_t image_size,
                                     const char *const *argv,
                                     const char *const *envp,
                                     uint64_t *out_entry);

/* M60: the real argument vector. `argv` is a NULL-terminated array of
 * NUL-terminated strings, argv[0] conventionally the program's own path -
 * the same shape every C program on every system expects, and the reason
 * this milestone had to happen before a program nobody here wrote could
 * be run at all.
 *
 * It is copied into the USER_ARG_BYTES region mapped at USER_ARG_ADDR,
 * laid out so crt0 can find it with a handful of instructions and without
 * the kernel having to agree with the assembler about a stack frame:
 *
 *     [USER_ARG_ADDR + 0]        uint64_t argc
 *     [USER_ARG_ADDR + 8]        char *argv[argc]   (user addresses)
 *     [.. + 8 + 8*argc]          NULL terminator
 *     [.. + 8 + 8*(argc+1)]      char *envp[envc]   (M75)
 *     [.. + NULL terminator]
 *     [after that]               the strings themselves
 *
 * RDI still points at USER_ARG_ADDR, exactly as it did when this was one
 * string - so enter_user_mode is untouched and the change is entirely in
 * what the region holds. A vector that will not fit is truncated at the
 * last whole argument that does, which is refusing rather than
 * corrupting: a half-copied argument names something else.
 *
 * M75 put envp immediately after argv's NULL rather than in a region of
 * its own, because that is where every C runtime written since V7 Unix
 * already looks for it - `envp = argv + argc + 1` is the convention, not
 * an invention here, and it means crt0 needs no second address. */
task_t *process_spawnv(const char *name, const uint8_t *image, size_t image_size,
                        const char *const *argv);

/* M75: the same thing, with an environment.
 *
 * `envp` is a NULL-terminated array of "NAME=value" strings. NULL means
 * "give the child a copy of the spawning task's own environment", which
 * is what makes inheritance the default rather than something every
 * launcher on this machine has to remember to do - the same argument
 * process_spawnv_capped makes about the capability manifest.
 *
 * The child's environment is also recorded kernel-side (task_t.env_block,
 * sched.h) so that *its* children can inherit in turn without the kernel
 * having to read a page a process is free to scribble on. */
task_t *process_spawnve(const char *name, const uint8_t *image, size_t image_size,
                        const char *const *argv, const char *const *envp);

/* arg may be NULL (equivalent to an empty string) for a program that
 * doesn't take one.
 *
 * M45: `name` is what this process will be listed as (task_t.name,
 * sched.h) - the path it was loaded from, which is the only thing at
 * this layer that resembles a human-readable identity. Passed in rather
 * than derived here because this function is handed an in-memory image,
 * not a path: sys_spawn (which read the file) and kernel_main's own
 * self-tests (which read it via vfs_read) are the two callers that
 * actually know it. May be NULL. */
task_t *process_spawn(const char *name, const uint8_t *image, size_t image_size, const char *arg);

/* M65: spawn with an explicit capability set, which is intersected with
 * the caller's rather than assigned - a process cannot hand out
 * authority it does not hold, and that is enforced here rather than
 * asked of callers, because a rule a caller can get wrong is a rule.
 * `caps` is what the child should keep; system_api/include/caps.h's
 * caps_for_program() is what every launcher in this OS passes. */
task_t *process_spawnv_capped(const char *name, const uint8_t *image, size_t image_size,
                              const char *const *argv, uint32_t caps);

/* M79: a second schedulable context inside the CALLER's address space.
 *
 * Not a spawn: there is no image, no ELF, no fresh page table and no
 * argument region - the thread runs code that is already mapped, on a
 * stack the caller allocated out of M78's mmap arena and passes in. The
 * only things this makes are a task and a kernel stack.
 *
 * The user stack comes from the caller rather than from here because the
 * per-process layout has room for exactly one stack (proc.h's
 * USER_STACK_TOP / USER_STACK_PAGES), and inventing a second fixed
 * location would put a ceiling on how many threads a process may have
 * into the address map. A stack a program allocated is also a stack a
 * program can free.
 *
 * Returns the new task, or NULL. */
task_t *process_spawn_thread(const char *name, uint64_t entry, uint64_t stack_top,
                              uint64_t arg);

/* M75: the full form - argv, envp and an explicit capability set. Every
 * other entry point above is this one with a default filled in. */
task_t *process_spawnve_capped(const char *name, const uint8_t *image, size_t image_size,
                               const char *const *argv, const char *const *envp, uint32_t caps);
