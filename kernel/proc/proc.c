#include "proc.h"

#include "caps.h" /* system_api/include/caps.h - caps_for_program, M65 */

#include "arch/x86_64/gdt.h"
#include "drivers/klog.h" /* M95: a refused interpreter says why */
#include "elf.h"
#include "fs/vfs.h"       /* M95: the interpreter is read off the disk */
#include "lib/libk.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "panic.h"

/* Stack/arg/heap/shm all land in the private PML4[1]-rooted region
 * vmm_create_address_space leaves empty (PML4[0] is the only slot shared
 * with the kernel) - well clear of the ELF images this project links
 * (kept low, well under 1 MiB), so nothing collides for anything this
 * loader is expected to handle so far. See proc.h for the actual address
 * constants (USER_STACK_TOP etc.) - shared with syscall.c now that M19's
 * SYS_sbrk/SYS_shm_map need to bounds-check growth into the same layout. */

extern void enter_user_mode(uint64_t entry, uint64_t user_stack, uint64_t arg_ptr,
                             uint64_t user_data_sel, uint64_t user_code_sel);

typedef struct {
    uint64_t entry;
    uint64_t user_stack_top;
    uint64_t arg_ptr;
} user_launch_args_t;

/* Runs once, in ring 0, as the freshly spawned task's first (and only)
 * kernel-side action - task_entry_trampoline (sched.c) calls this the
 * same way it calls any other task's entry function. enter_user_mode
 * never returns: from here on, the only way back into kernel code for
 * this task is a future interrupt/syscall. */
static void user_task_launcher(void *arg) {
    user_launch_args_t *args = (user_launch_args_t *)arg;
    uint64_t entry = args->entry;
    uint64_t stack = args->user_stack_top;
    uint64_t arg_ptr = args->arg_ptr;
    kfree(args);
    enter_user_mode(entry, stack, arg_ptr, GDT_USER_DATA_SEL | 3, GDT_USER_CODE_SEL | 3);
}

/* Everything from the image's load address up to and including the
 * argument page (the stack sits between them), plus the sbrk heap and the
 * mmap arena. Deliberately *not* [USER_SHM_BASE, ...) or USER_FB_BASE -
 * see process_destroy_address_space's declaration in proc.h.
 *
 * M83: hoisted to file scope, because fork needs the identical list. It
 * takes an owner reference on every page it copies and teardown drops one
 * for every page it frees, so a page in one list and not the other is a
 * frame pinned forever. One list, one place. */
static const vmm_range_t PROCESS_OWNED[] = {
        /* M91: the image and the stack used to be one range, because the
         * stack sat 2 MiB above the image and the single span
         * [USER_IMAGE_BASE, USER_ARG_ADDR + USER_ARG_BYTES) covered both
         * with the gap between them costing nothing. They are now 496 GiB
         * apart, so they are two ranges - and the stack's low end is
         * USER_STACK_LIMIT rather than the initial mapping's bottom,
         * because a stack that grew on a fault must be freed as far down
         * as it grew. */
        {USER_IMAGE_BASE, USER_IMAGE_LIMIT},
        {USER_STACK_LIMIT, USER_ARG_ADDR + USER_ARG_BYTES},
        {USER_HEAP_START, USER_HEAP_LIMIT},
        /* M78: the mmap arena. Anonymous and private by construction -
         * there is no file-backed or MAP_SHARED mapping in this kernel -
         * so every frame in it belongs to this process alone and can be
         * given straight back, exactly like the sbrk heap above it. That
         * is precisely why the "deliberately not MAP_SHARED" line in the
         * milestone is not a limitation being apologised for: a shared
         * mapping here would make this list wrong. */
        {USER_MMAP_BASE, USER_MMAP_LIMIT},
};
#define PROCESS_OWNED_COUNT ((int)(sizeof(PROCESS_OWNED) / sizeof(PROCESS_OWNED[0])))

void process_destroy_address_space(uint64_t pml4_phys) {
    if (pml4_phys == 0 || pml4_phys == vmm_kernel_pml4_phys()) {
        return; /* a plain kernel thread shares the kernel's - there is nothing private to tear down */
    }
    vmm_destroy_address_space(pml4_phys, PROCESS_OWNED, PROCESS_OWNED_COUNT);
}

/* M83: the copy-on-write clone, over the same ranges. */
uint64_t process_fork_address_space(uint64_t src_pml4_phys) {
    if (src_pml4_phys == 0 || src_pml4_phys == vmm_kernel_pml4_phys()) {
        return 0; /* a kernel thread has no private address space to fork */
    }
    return vmm_fork_address_space(src_pml4_phys, PROCESS_OWNED, PROCESS_OWNED_COUNT);
}

/* ---- M84: building an address space, separated from making a task -----
 *
 * Everything a fresh process needs in memory - the image mapped from its
 * ELF, a stack, and the argument region argc/argv/envp live in - with no
 * task_t anywhere in it.
 *
 * Split out because `execve` needs exactly this and nothing else: it
 * already has a task, and what it wants is a new address space to point
 * that task at. Before this split the only way to get one was to spawn a
 * whole new process, which is the thing exec exists not to do.
 *
 * Returns the new PML4's physical address and writes the entry point to
 * `*out_entry`, or returns 0 having given back everything it built. The
 * stack top and the argument region are at their fixed addresses
 * (USER_STACK_TOP, USER_ARG_ADDR), so there is nothing else to report.
 */
uint64_t process_build_address_space(const uint8_t *image, size_t image_size,
                                     const char *const *argv,
                                     const char *const *envp,
                                     uint64_t *out_entry) {
    if (elf_validate(image, image_size) == 0) {
        return 0;
    }
    /* ---- M95: is this a program that needs a dynamic linker? ----------
     *
     * Read BEFORE the address space exists, because the interpreter's
     * path is a string in the file and this is the last moment the whole
     * file is in kernel memory. If there is one, the interpreter is read
     * off the disk now too, so a missing one fails here rather than
     * halfway through building a process.
     */
    static char interp_path[128];
    uint8_t *interp_image = 0;
    size_t interp_bytes = 0;
    int has_interp = elf_interp(image, image_size, interp_path, sizeof(interp_path));
    if (has_interp) {
        if (k_strcmp(interp_path, USER_INTERP_PATH) != 0) {
            /* One dynamic linker, and a program asking for a different
             * one is asking for something that is not here. Refused
             * rather than substituted: silently running a different
             * linker than the one a program was built against is how a
             * symbol resolves to the wrong definition. */
            klog_debug("[elf] refused: unknown interpreter ");
            klog_debug(interp_path);
            klog_debug("\n");
            return 0;
        }
        leanfs_stat_t ist;
        int64_t n = (vfs_stat(interp_path, &ist) == 0) ? (int64_t)ist.size : -1;
        if (n <= 0) {
            klog_debug("[elf] refused: " USER_INTERP_PATH " is not on this disk\n");
            return 0;
        }
        interp_bytes = (size_t)n;
        interp_image = (uint8_t *)kmalloc(interp_bytes);
        if (!interp_image) {
            return 0;
        }
        if (vfs_read(interp_path, interp_image, interp_bytes) != (int64_t)interp_bytes) {
            kfree(interp_image);
            return 0;
        }
        if (!elf_is_dyn(interp_image, interp_bytes)) {
            klog_debug("[elf] refused: the interpreter is not a shared object\n");
            kfree(interp_image);
            return 0;
        }
    }

    uint64_t pml4_phys = vmm_create_address_space();
    if (pml4_phys == 0) {
        /* M102: no frame even for the new PML4. The cheapest possible
         * point for a spawn on a full machine to fail - nothing has been
         * read, mapped or claimed yet. */
        return 0;
    }
    /* M95: an ET_DYN program is placed at the image base, an ET_EXEC
     * names its own. elf_load already chooses between them; the bias is
     * recomputed here because the auxiliary vector below has to report
     * it. */
    uint64_t prog_bias = elf_is_dyn(image, image_size) ? USER_IMAGE_BASE : 0;
    uint64_t entry = elf_load_at(pml4_phys, image, image_size, prog_bias);
    if (entry == 0) {
        /* Only reachable now by running out of physical memory partway
         * through mapping (elf.c). M54: whatever it did manage to build
         * goes back, where before this milestone there was nothing to
         * give it back *to* - an abandoned address space was simply lost
         * for the machine's uptime, which is the leak M29 documented and
         * M50 measured. */
        kfree(interp_image);
        process_destroy_address_space(pml4_phys);
        return 0;
    }

    /* M95: and the interpreter, at its own base, becoming the entry
     * point. The program is loaded and mapped; what runs first is the
     * linker, which finishes the job from inside the address space and
     * then jumps to AT_ENTRY. */
    uint64_t interp_entry = 0;
    if (has_interp) {
        interp_entry = elf_load_at(pml4_phys, interp_image, interp_bytes,
                                   USER_INTERP_BASE);
        kfree(interp_image);
        interp_image = 0;
        if (interp_entry == 0) {
            process_destroy_address_space(pml4_phys);
            return 0;
        }
    }

    /* M91: still eager, and still only USER_STACK_PAGES of it.
     *
     * The stack can now grow down to USER_STACK_LIMIT on a fault
     * (sched_fault_fill), which is what makes 64 MiB of stack cost
     * nothing until it is used. What is built here is the part a program
     * has before it has run an instruction - a process whose very first
     * push depended on the fault handler would be a process whose
     * simplest failure mode is the hardest one to debug.
     *
     * No VMM_FLAG_EXEC: a stack is not code. Nothing on this machine
     * needed that to be true before, because nothing could express it. */
    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    for (uint64_t va = stack_bottom; va < USER_STACK_TOP; va += PAGE_SIZE) {
        /* M102: this used to be pmm_alloc_frame, which halted the machine
         * rather than returning. A spawn that cannot get a stack is an
         * ordinary failure of one spawn. */
        uint64_t phys = pmm_try_alloc_frame();
        if (phys == 0) {
            process_destroy_address_space(pml4_phys);
            return 0;
        }
        k_memset((void *)phys, 0, PAGE_SIZE);
        if (vmm_try_map_page_in(pml4_phys, va, phys,
                                VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
            /* The frame is not in the address space, so tearing that down
             * will not find it. It has to go back by hand, here. */
            pmm_free_frame(phys);
            process_destroy_address_space(pml4_phys);
            return 0;
        }
    }

    /* ---- M60/M75: the argument region ---------------------------------
     *
     * argc, then argv, then envp, then every string - one virtually
     * contiguous USER_ARG_BYTES block at USER_ARG_ADDR. See proc.h for
     * the exact layout and for why envp sits immediately after argv's
     * NULL rather than somewhere of its own.
     *
     * It is built in a scratch buffer and then copied page by page into
     * freshly allocated frames, rather than written straight into
     * physical memory the way the one-page version could: the region is
     * two pages now, and two pmm frames are not guaranteed to be
     * adjacent - so a string that straddles the page boundary would be
     * written into the wrong place by exactly one frame's worth. The
     * scratch buffer is where "virtually contiguous" is made true before
     * anything depends on it.
     */
    /* M89: the region is 32 pages of window and only the used ones are
     * backed - see USER_ARG_PAGES. So the block is built first, the
     * bytes it actually took are counted, and the frames are allocated
     * afterwards against that count. `arg_used` is set inside the block
     * below and is at least one page, because argv[0] always exists. */
    uint64_t arg_frames[USER_ARG_PAGES];
    int arg_pages = 0;
    size_t arg_used = 0;
    char *block = (char *)kmalloc(USER_ARG_BYTES);
    if (!block) {
        process_destroy_address_space(pml4_phys);
        return 0;
    }
    k_memset(block, 0, USER_ARG_BYTES);
    {
        int argc = 0;
        if (argv) {
            while (argv[argc]) {
                argc++;
            }
        }
        int envc = 0;
        if (envp) {
            while (envp[envc]) {
                envc++;
            }
        }
        uint64_t *header = (uint64_t *)block;
        /* Both pointer arrays sit between argc and the strings, so where
         * the strings start depends on how many of each there are. Two
         * NULL terminators, one per vector. */
        /* M95: +14 for the auxiliary vector, which sits after envp's
         * NULL and before the strings. */
        size_t strings_off = sizeof(uint64_t) * (size_t)(1 + argc + 1 + envc + 1 + 14);
        size_t at = strings_off;
        if (at > USER_ARG_BYTES) {
            /* More pointers than the region holds, before a single string
             * has been copied. Nothing sane produces this - SPAWN_MAX_ARGS
             * is 16 - but the arithmetic below would index past the
             * buffer, so it is checked rather than assumed. */
            at = USER_ARG_BYTES;
            argc = 0;
            envc = 0;
            strings_off = sizeof(uint64_t) * 3;
            at = strings_off;
        }
        int stored = 0;
        for (int i = 0; i < argc; i++) {
            size_t len = k_strlen(argv[i]) + 1;
            if (at + len > USER_ARG_BYTES) {
                /* Truncated at the last whole argument that fits. Half an
                 * argument names something else, so the vector simply
                 * ends here - and the program sees a shorter argc rather
                 * than a corrupt string. */
                break;
            }
            k_memcpy(block + at, argv[i], len);
            header[1 + (size_t)stored] = USER_ARG_ADDR + at;
            at += len;
            stored++;
        }
        header[0] = (uint64_t)stored;
        header[1 + (size_t)stored] = 0; /* the NULL every argv ends with */

        /* envp starts one slot past argv's NULL - the SysV convention,
         * which is what lets crt0 compute it from argc alone. Note the
         * index is written against `stored`, not `argc`: if arguments
         * were truncated, the environment moves up with them rather than
         * leaving a hole full of zeros that a runtime would read as an
         * empty environment. */
        size_t env_base = 1 + (size_t)stored + 1;
        int env_stored = 0;
        for (int i = 0; i < envc; i++) {
            size_t len = k_strlen(envp[i]) + 1;
            if (at + len > USER_ARG_BYTES ||
                (env_base + (size_t)env_stored + 1) * sizeof(uint64_t) > strings_off) {
                break;
            }
            k_memcpy(block + at, envp[i], len);
            header[env_base + (size_t)env_stored] = USER_ARG_ADDR + at;
            at += len;
            env_stored++;
        }
        header[env_base + (size_t)env_stored] = 0;

        /* ---- M95: the auxiliary vector ------------------------------
         *
         * Immediately after envp's NULL, which is where every SysV
         * runtime looks - see system_api/include/proc.h for the list and
         * for why each entry is one the linker cannot compute itself.
         * Written for every process, not only dynamic ones: a static
         * program never reads it, and a vector that appeared only
         * sometimes would be a second layout to get right. */
        size_t aux_base = env_base + (size_t)env_stored + 1;
        uint64_t aux[14];
        int an = 0;
        aux[an++] = AT_PHDR;
        aux[an++] = elf_phdr_vaddr(image, image_size, prog_bias);
        aux[an++] = AT_PHENT;
        aux[an++] = 56; /* sizeof(Elf64_Phdr) - checked by elf_validate */
        aux[an++] = AT_PHNUM;
        aux[an++] = elf_phnum(image, image_size);
        aux[an++] = AT_PAGESZ;
        aux[an++] = PAGE_SIZE;
        aux[an++] = AT_BASE;
        aux[an++] = has_interp ? USER_INTERP_BASE : 0;
        aux[an++] = AT_ENTRY;
        aux[an++] = entry;
        aux[an++] = AT_NULL;
        aux[an++] = 0;
        for (int i = 0; i < an; i++) {
            if ((aux_base + (size_t)i + 1) * sizeof(uint64_t) > strings_off) {
                /* No room. The vector is terminated where it stands,
                 * which a runtime reads as "nothing more to say" - and a
                 * dynamic program in that state will fail to find its
                 * own headers and say so, rather than reading past the
                 * end of the region. */
                header[aux_base + (size_t)i] = AT_NULL;
                break;
            }
            header[aux_base + (size_t)i] = aux[i];
        }
        arg_used = at;
    }
    arg_pages = (int)((arg_used + PAGE_SIZE - 1) / PAGE_SIZE);
    if (arg_pages < 1) {
        arg_pages = 1;
    }
    for (int i = 0; i < arg_pages; i++) {
        /* M102: pmm_try_alloc_frame, which is what makes the check below
         * reachable. It was written against pmm_alloc_frame, which panics
         * rather than returning 0 - so this careful unwind has been dead
         * code since it was written, and the machine halted three lines
         * earlier instead. */
        arg_frames[i] = pmm_try_alloc_frame();
        if (arg_frames[i] == 0) {
            /* Out of frames partway through. Give back whatever was
             * taken and fail the spawn - the address space goes with it
             * below, and a process with half an argument region is a
             * process whose argv[0] may not exist. */
            for (int j = 0; j < i; j++) {
                pmm_free_frame(arg_frames[j]);
            }
            kfree(block);
            process_destroy_address_space(pml4_phys);
            return 0;
        }
    }
    for (int i = 0; i < arg_pages; i++) {
        k_memcpy((void *)arg_frames[i], block + (size_t)i * PAGE_SIZE, PAGE_SIZE);
        /* M102: the last mapping on this path that could halt the
         * machine. A page table is a frame like any other, and the
         * argument region is the last thing built before a spawn
         * succeeds - so this is the narrowest window in which a spawn can
         * fail, and it still has to fail rather than stop the machine. */
        if (vmm_try_map_page_in(pml4_phys, USER_ARG_ADDR + (uint64_t)i * PAGE_SIZE,
                                arg_frames[i], VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
            /* Frames not yet mapped are not reachable from the address
             * space, so they go back by hand; the ones already mapped go
             * with it. */
            for (int j = i; j < arg_pages; j++) {
                pmm_free_frame(arg_frames[j]);
            }
            kfree(block);
            process_destroy_address_space(pml4_phys);
            return 0;
        }
    }
    kfree(block);
    /* M95: the linker runs first when there is one. AT_ENTRY in the
     * auxiliary vector is where it jumps afterwards, which is why the
     * program's own entry had to be computed before this. */
    *out_entry = has_interp ? interp_entry : entry;
    return pml4_phys;
}

static task_t *spawn_common(const char *name, const uint8_t *image, size_t image_size,
                             const char *const *argv, const char *const *envp) {
    /* M40: checked up front, before anything is allocated. The
     * task-table-full check further down still exists (it has to - it's
     * the one that runs under sched_lock and is therefore the
     * authoritative one), but reaching *only* that one meant a full table
     * cost an address space and an argument frame per rejected spawn,
     * neither of which this project has a path to reclaim. Failing here
     * instead makes the common case of a full table genuinely free. */
    if (!sched_has_free_task_slot()) {
        return (task_t *)0;
    }

    /* M40's "reject a bad image before an address space exists to
     * abandon" check now lives at the top of
     * process_build_address_space, because exec needs it for the same
     * reason a spawn does and one copy is better than two. The reasoning
     * is unchanged and is recorded there. */
    uint64_t entry = 0;
    uint64_t pml4_phys = process_build_address_space(image, image_size, argv, envp, &entry);
    if (pml4_phys == 0) {
        return (task_t *)0;
    }

    /* This kmalloc runs in the caller's (kernel) context, but the
     * resulting pointer stays valid once the new task starts running
     * under its own CR3 too - the heap lives in PML4[0], shared by every
     * address space. */
    user_launch_args_t *args = (user_launch_args_t *)kmalloc(sizeof(user_launch_args_t));
    if (!args) {
        /* M102: the last panic on the spawn path, and the only one that
         * was ever reachable from a user program. Everything this
         * function built is given back and the spawn fails, which is what
         * every other failure here already did. */
        process_destroy_address_space(pml4_phys);
        return (task_t *)0;
    }
    args->entry = entry;
    args->user_stack_top = USER_STACK_TOP;
    args->arg_ptr = USER_ARG_ADDR;

    task_t *t = task_spawn_in(name, pml4_phys, user_task_launcher, args,
                               USER_HEAP_START, USER_SHM_BASE);
    if (!t) {
        /* Two reasons now, and both are ordinary. The MAX_TASKS table
         * (sched.c) is full, or there was no contiguous run of frames
         * left for the new task's kernel stack.
         *
         * M102 corrected the second half of this comment rather than
         * leaving it: it used to say "unlike out-of-memory above, a full
         * task table is a normal, recoverable condition", which was true
         * when running out of memory halted the machine and is not true
         * now. Both conditions arrive here as the same NULL, and both
         * are recovered from the same way. Every caller of process_spawn
         * (sys_spawn in syscall.c, and kernel.c's own self-tests) already
         * treats a NULL/-1 result as an ordinary failure, so propagate
         * cleanly instead of dereferencing NULL below.
         *
         * M54: and give the address space back. This path used to leak
         * the pml4, the image, the stack and the argument page - roughly
         * fifteen frames - with a comment arguing that a full table was
         * an already-degraded state not worth recovering from. That was
         * only ever true because there was nothing to recover *with*;
         * "the machine is under pressure" is precisely when leaking is
         * worst. */
        kfree(args);
        process_destroy_address_space(pml4_phys);
        return (task_t *)0;
    }
    /* M19's heap/shm starting points (USER_HEAP_START/USER_SHM_BASE) used
     * to be assigned right here, after the task was already live. M40
     * moved them into task_spawn_in's argument list instead - see its
     * declaration in sched.h, and task_spawn_common for the race that
     * made this a real panic rather than a theoretical one. */
    return t;
}

task_t *process_spawnv(const char *name, const uint8_t *image, size_t image_size,
                        const char *const *argv) {
    return process_spawnve(name, image, image_size, argv, (const char *const *)0);
}

task_t *process_spawnve(const char *name, const uint8_t *image, size_t image_size,
                        const char *const *argv, const char *const *envp) {
    return process_spawnve_capped(name, image, image_size, argv, envp,
                                   caps_for_program(name ? name : ""));
}

task_t *process_spawnv_capped(const char *name, const uint8_t *image, size_t image_size,
                              const char *const *argv, uint32_t caps) {
    return process_spawnve_capped(name, image, image_size, argv, (const char *const *)0, caps);
}

/* M75: `envp == NULL` means "inherit", and the inheriting is done here
 * rather than in spawn_common because this is the layer that can see the
 * caller. The block is read *before* the child exists so that the two
 * cases - a supplied vector and an inherited block - converge on one
 * code path that lays strings into the child's argument region.
 *
 * The inherited form is copied straight from the parent's packed block
 * into a temporary pointer vector, which costs one small allocation and
 * keeps spawn_common with exactly one input shape rather than two. */
task_t *process_spawnve_capped(const char *name, const uint8_t *image, size_t image_size,
                               const char *const *argv, const char *const *envp, uint32_t caps) {
    /* M79: through the address space's owner - a thread's own env_block
     * is empty (it was never given one), and a child spawned from a
     * thread must inherit the process's environment rather than nothing. */
    task_t *self = sched_vm_owner(sched_current());
    const char *inherited[USER_ENV_MAX_VARS + 1];
    const char *const *effective = envp;
    uint32_t inherited_len = 0;
    uint32_t inherited_count = 0;

    if (!envp && self && self->env_block && self->env_count) {
        uint32_t n = 0;
        uint32_t off = 0;
        while (off < self->env_len && n < USER_ENV_MAX_VARS) {
            inherited[n++] = self->env_block + off;
            while (off < self->env_len && self->env_block[off]) {
                off++;
            }
            off++; /* past the NUL */
        }
        inherited[n] = (const char *)0;
        effective = inherited;
    }

    task_t *t = spawn_common(name, image, image_size, argv, effective);
    if (t) {
        /* Record what the child actually got, so that *its* children can
         * inherit in turn. Packed from the same vector spawn_common laid
         * out, which is what keeps the two representations from being two
         * different environments.
         *
         * kmalloc'd rather than a static scratch buffer: two CPUs can be
         * inside a spawn at once (this kernel is preemptible and SMP -
         * M67), and a shared buffer would hand one child the other's
         * environment. */
        char *packed = (char *)kmalloc(USER_ENV_MAX_BYTES);
        if (packed) {
            if (effective) {
                for (int i = 0; effective[i] && inherited_count < USER_ENV_MAX_VARS; i++) {
                    uint32_t len = (uint32_t)k_strlen(effective[i]) + 1;
                    if (inherited_len + len > USER_ENV_MAX_BYTES) {
                        break;
                    }
                    k_memcpy(packed + inherited_len, effective[i], len);
                    inherited_len += len;
                    inherited_count++;
                }
            }
            sched_set_env(t, packed, inherited_len, inherited_count);
            kfree(packed);
        }
        /* M65: intersected, never assigned. spawn_common already gave the
         * child the caller's set; this can only clear bits. A caller
         * asking for more than it holds silently gets less, which is the
         * right failure - the alternative is a spawn that fails for a
         * reason the launcher cannot do anything about.
         *
         * And note where this is: *every* spawn in this OS goes through
         * here, so the manifest in caps.h is applied by the kernel rather
         * than by the goodwill of whichever program did the launching.
         * A launcher that forgot to ask would be a way around the whole
         * model, and the way to not have that problem is to not give
         * launchers the choice. */
        t->caps &= caps;
    }
    return t;
}

/* M79: see proc.h. The launcher is the same one a fresh process uses -
 * enter_user_mode does not care whether the address space it is dropping
 * into was made a moment ago or has been running for a while, which is
 * exactly why a thread needs no new mechanism at the ring-3 boundary. */
task_t *process_spawn_thread(const char *name, uint64_t entry, uint64_t stack_top,
                              uint64_t arg) {
    task_t *self = sched_current();
    if (!self || self->pml4_phys == vmm_kernel_pml4_phys()) {
        return (task_t *)0; /* a kernel thread has no ring-3 address space to share */
    }
    user_launch_args_t *args = (user_launch_args_t *)kmalloc(sizeof(user_launch_args_t));
    if (!args) {
        return (task_t *)0;
    }
    args->entry = entry;
    args->user_stack_top = stack_top;
    /* RDI at the first ring-3 instruction. For a process this points at
     * the argument region; for a thread it is the single `void *` its
     * start routine takes, which is the same register and the same
     * calling convention with a different thing in it. */
    args->arg_ptr = arg;

    task_t *t = task_spawn_thread(name, self, user_task_launcher, args);
    if (!t) {
        kfree(args);
        return (task_t *)0;
    }
    return t;
}

/* M60: the one-argument form, kept because almost every caller in this
 * project has exactly one thing to say ("open this file"). It builds the
 * two-element vector a real argv is - the program's own name, then the
 * argument - so nothing below this line has two ways to launch a
 * process. */
task_t *process_spawn(const char *name, const uint8_t *image, size_t image_size, const char *arg) {
    const char *argv[3];
    int n = 0;
    argv[n++] = name;
    if (arg && arg[0]) {
        argv[n++] = arg;
    }
    argv[n] = (const char *)0;
    /* Through process_spawnv, not spawn_common: the manifest lookup in
     * process_spawnv_capped is only "applied by the kernel at *every*
     * spawn" if this form takes the same door - going straight to
     * spawn_common here would hand every one-argument spawn the caller's
     * whole capability set. */
    return process_spawnv(name, image, image_size, argv);
}
