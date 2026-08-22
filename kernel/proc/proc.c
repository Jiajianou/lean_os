#include "proc.h"

#include "arch/x86_64/gdt.h"
#include "elf.h"
#include "lib/libk.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "panic.h"

/* All three land in the private PML4[1]-rooted region
 * vmm_create_address_space leaves empty (PML4[0] is the only slot shared
 * with the kernel) - well clear of the ELF images this project links
 * (kept low, well under 1 MiB), so code/stack/arg can't collide for
 * anything this loader is expected to handle so far. The arg page sits
 * just past the stack's top (an otherwise-unused address once the stack
 * itself is bounded to USER_STACK_PAGES below it). */
#define USER_STACK_TOP   0x0000008000200000ULL
#define USER_STACK_PAGES 4 /* 16 KiB */
#define USER_ARG_ADDR    USER_STACK_TOP
#define PAGE_SIZE        4096ULL

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

task_t *process_spawn(const uint8_t *image, size_t image_size, const char *arg) {
    uint64_t pml4_phys = vmm_create_address_space();

    uint64_t entry = elf_load(pml4_phys, image, image_size);

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

    return task_spawn_in(pml4_phys, user_task_launcher, args);
}
