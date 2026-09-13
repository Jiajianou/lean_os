#include "proc.h"

#include "caps.h"

#include "arch/x86_64/gdt.h"
#include "drivers/klog.h"
#include "elf.h"
#include "fs/vfs.h"
#include "lib/libk.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "panic.h"

extern void enter_user_mode(uint64_t entry, uint64_t user_stack, uint64_t arg_ptr,
                             uint64_t user_data_sel, uint64_t user_code_sel);

typedef struct {
    uint64_t entry;
    uint64_t user_stack_top;
    uint64_t arg_ptr;
} user_launch_args_t;

static void user_task_launcher(void *arg) {
    user_launch_args_t *args = (user_launch_args_t *)arg;
    uint64_t entry = args->entry;
    uint64_t stack = args->user_stack_top;
    uint64_t arg_ptr = args->arg_ptr;
    kfree(args);
    enter_user_mode(entry, stack, arg_ptr, GDT_USER_DATA_SEL | 3, GDT_USER_CODE_SEL | 3);
}

static const vmm_range_t PROCESS_OWNED[] = {
        {USER_IMAGE_BASE, USER_IMAGE_LIMIT},
        {USER_STACK_LIMIT, USER_ARG_ADDR + USER_ARG_BYTES},
        {USER_HEAP_START, USER_HEAP_LIMIT},
        {USER_MMAP_BASE, USER_MMAP_LIMIT},
};
#define PROCESS_OWNED_COUNT ((int)(sizeof(PROCESS_OWNED) / sizeof(PROCESS_OWNED[0])))

void process_destroy_address_space(uint64_t pml4_phys) {
    if (pml4_phys == 0 || pml4_phys == vmm_kernel_pml4_phys()) {
        return;
    }
    vmm_destroy_address_space(pml4_phys, PROCESS_OWNED, PROCESS_OWNED_COUNT);
}

uint64_t process_fork_address_space(uint64_t src_pml4_phys) {
    if (src_pml4_phys == 0 || src_pml4_phys == vmm_kernel_pml4_phys()) {
        return 0;
    }
    return vmm_fork_address_space(src_pml4_phys, PROCESS_OWNED, PROCESS_OWNED_COUNT);
}

uint64_t process_build_address_space(const uint8_t *image, size_t image_size,
                                     const char *const *argv,
                                     const char *const *envp,
                                     uint64_t *out_entry) {
    if (elf_validate(image, image_size) == 0) {
        return 0;
    }
    static char interp_path[128];
    uint8_t *interp_image = 0;
    size_t interp_bytes = 0;
    int has_interp = elf_interp(image, image_size, interp_path, sizeof(interp_path));
    if (has_interp) {
        if (k_strcmp(interp_path, USER_INTERP_PATH) != 0) {
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
        return 0;
    }
    uint64_t prog_bias = elf_is_dyn(image, image_size) ? USER_IMAGE_BASE : 0;
    uint64_t entry = elf_load_at(pml4_phys, image, image_size, prog_bias);
    if (entry == 0) {
        kfree(interp_image);
        process_destroy_address_space(pml4_phys);
        return 0;
    }

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

    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    for (uint64_t va = stack_bottom; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t phys = pmm_try_alloc_frame();
        if (phys == 0) {
            process_destroy_address_space(pml4_phys);
            return 0;
        }
        k_memset((void *)phys, 0, PAGE_SIZE);
        if (vmm_try_map_page_in(pml4_phys, va, phys,
                                VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
            pmm_free_frame(phys);
            process_destroy_address_space(pml4_phys);
            return 0;
        }
    }

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
        size_t strings_off = sizeof(uint64_t) * (size_t)(1 + argc + 1 + envc + 1 + 14);
        size_t at = strings_off;
        if (at > USER_ARG_BYTES) {
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
                break;
            }
            k_memcpy(block + at, argv[i], len);
            header[1 + (size_t)stored] = USER_ARG_ADDR + at;
            at += len;
            stored++;
        }
        header[0] = (uint64_t)stored;
        header[1 + (size_t)stored] = 0;

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

        size_t aux_base = env_base + (size_t)env_stored + 1;
        uint64_t aux[14];
        int an = 0;
        aux[an++] = AT_PHDR;
        aux[an++] = elf_phdr_vaddr(image, image_size, prog_bias);
        aux[an++] = AT_PHENT;
        aux[an++] = 56;
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
        arg_frames[i] = pmm_try_alloc_frame();
        if (arg_frames[i] == 0) {
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
        if (vmm_try_map_page_in(pml4_phys, USER_ARG_ADDR + (uint64_t)i * PAGE_SIZE,
                                arg_frames[i], VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
            for (int j = i; j < arg_pages; j++) {
                pmm_free_frame(arg_frames[j]);
            }
            kfree(block);
            process_destroy_address_space(pml4_phys);
            return 0;
        }
    }
    kfree(block);
    *out_entry = has_interp ? interp_entry : entry;
    return pml4_phys;
}

static task_t *spawn_common(const char *name, const uint8_t *image, size_t image_size,
                             const char *const *argv, const char *const *envp) {
    if (!sched_has_free_task_slot()) {
        return (task_t *)0;
    }

    uint64_t entry = 0;
    uint64_t pml4_phys = process_build_address_space(image, image_size, argv, envp, &entry);
    if (pml4_phys == 0) {
        return (task_t *)0;
    }

    user_launch_args_t *args = (user_launch_args_t *)kmalloc(sizeof(user_launch_args_t));
    if (!args) {
        process_destroy_address_space(pml4_phys);
        return (task_t *)0;
    }
    args->entry = entry;
    args->user_stack_top = USER_STACK_TOP;
    args->arg_ptr = USER_ARG_ADDR;

    task_t *t = task_spawn_in(name, pml4_phys, user_task_launcher, args,
                               USER_HEAP_START, USER_SHM_BASE);
    if (!t) {
        kfree(args);
        process_destroy_address_space(pml4_phys);
        return (task_t *)0;
    }
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

task_t *process_spawnve_capped(const char *name, const uint8_t *image, size_t image_size,
                               const char *const *argv, const char *const *envp, uint32_t caps) {
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
            off++;
        }
        inherited[n] = (const char *)0;
        effective = inherited;
    }

    task_t *t = spawn_common(name, image, image_size, argv, effective);
    if (t) {
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
        t->caps &= caps;
    }
    return t;
}

task_t *process_spawn_thread(const char *name, uint64_t entry, uint64_t stack_top,
                              uint64_t arg) {
    task_t *self = sched_current();
    if (!self || self->pml4_phys == vmm_kernel_pml4_phys()) {
        return (task_t *)0;
    }
    user_launch_args_t *args = (user_launch_args_t *)kmalloc(sizeof(user_launch_args_t));
    if (!args) {
        return (task_t *)0;
    }
    args->entry = entry;
    args->user_stack_top = stack_top;
    args->arg_ptr = arg;

    task_t *t = task_spawn_thread(name, self, user_task_launcher, args);
    if (!t) {
        kfree(args);
        return (task_t *)0;
    }
    return t;
}

task_t *process_spawn(const char *name, const uint8_t *image, size_t image_size, const char *arg) {
    const char *argv[3];
    int n = 0;
    argv[n++] = name;
    if (arg && arg[0]) {
        argv[n++] = arg;
    }
    argv[n] = (const char *)0;
    return process_spawnv(name, image, image_size, argv);
}
