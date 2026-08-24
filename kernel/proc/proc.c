#include "proc.h"

#include "arch/x86_64/gdt.h"
#include "elf.h"
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

void process_destroy_address_space(uint64_t pml4_phys) {
    if (pml4_phys == 0 || pml4_phys == vmm_kernel_pml4_phys()) {
        return; /* a plain kernel thread shares the kernel's - there is nothing private to tear down */
    }
    /* Everything from the image's load address up to and including the
     * argument page (the stack sits between them), plus the sbrk heap.
     * Deliberately *not* [USER_SHM_BASE, ...) or USER_FB_BASE - see this
     * function's declaration in proc.h. */
    static const vmm_range_t OWNED[] = {
        {USER_IMAGE_BASE, USER_ARG_ADDR + PAGE_SIZE},
        {USER_HEAP_START, USER_HEAP_LIMIT},
    };
    vmm_destroy_address_space(pml4_phys, OWNED, (int)(sizeof(OWNED) / sizeof(OWNED[0])));
}

task_t *process_spawn(const char *name, const uint8_t *image, size_t image_size, const char *arg) {
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

    /* M40: reject a bad image before an address space exists to abandon.
     * elf_load used to `panic` on anything that wasn't a valid x86-64
     * ET_EXEC, which SYS_spawn made reachable from user space as
     * sys_spawn("any_non_program_file", "") - it now returns 0 instead
     * (elf.h), and asking elf_validate first means the refusal costs
     * nothing at all: no frames, no page tables, no task slot. Worth the
     * extra pass over the program headers precisely because there is no
     * vmm_destroy_address_space in this project to clean up after the
     * other ordering. */
    if (elf_validate(image, image_size) == 0) {
        return (task_t *)0;
    }

    uint64_t pml4_phys = vmm_create_address_space();
    uint64_t entry = elf_load(pml4_phys, image, image_size);
    if (entry == 0) {
        /* Only reachable now by running out of physical memory partway
         * through mapping (elf.c). M54: whatever it did manage to build
         * goes back, where before this milestone there was nothing to
         * give it back *to* - an abandoned address space was simply lost
         * for the machine's uptime, which is the leak M29 documented and
         * M50 measured. */
        process_destroy_address_space(pml4_phys);
        return (task_t *)0;
    }

    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    for (uint64_t va = stack_bottom; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_frame();
        vmm_map_page_in(pml4_phys, va, phys, VMM_FLAG_WRITABLE | VMM_FLAG_USER);
    }

    /* lean_os's entire "argv": one NUL-terminated string, not a real
     * argc/argv array - a real C program would need to know a real ABI
     * to unpack an array from the stack; a single string RDI already
     * fits the normal SysV first-argument convention crt0 relies on. */
    uint64_t arg_phys = pmm_alloc_frame();
    k_memset((void *)arg_phys, 0, PAGE_SIZE);
    if (arg) {
        k_strlcpy((char *)arg_phys, arg, PAGE_SIZE);
    }
    vmm_map_page_in(pml4_phys, USER_ARG_ADDR, arg_phys, VMM_FLAG_WRITABLE | VMM_FLAG_USER);

    /* This kmalloc runs in the caller's (kernel) context, but the
     * resulting pointer stays valid once the new task starts running
     * under its own CR3 too - the heap lives in PML4[0], shared by every
     * address space. */
    user_launch_args_t *args = (user_launch_args_t *)kmalloc(sizeof(user_launch_args_t));
    if (!args) {
        panic("process_spawn: out of memory for launch args");
    }
    args->entry = entry;
    args->user_stack_top = USER_STACK_TOP;
    args->arg_ptr = USER_ARG_ADDR;

    task_t *t = task_spawn_in(name, pml4_phys, user_task_launcher, args,
                               USER_HEAP_START, USER_SHM_BASE);
    if (!t) {
        /* Fixed MAX_TASKS table (sched.c) is full - task_spawn_in already
         * reports this cleanly (NULL, not a panic - unlike out-of-memory
         * above, a full task table is a normal, recoverable condition a
         * caller might hit and retry from). Every caller of process_spawn
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
