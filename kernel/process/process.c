#include "process.h"

#include "capabilities.h"

#include "architecture/x86_64/global_descriptor_table.h"
#include "drivers/kernel_log.h"
#include "elf.h"
#include "file_system/virtual_file_system.h"
#include "library/kernel_library.h"
#include "memory_management/heap.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"
#include "panic.h"

extern void enter_user_mode(uint64_t entry, uint64_t user_stack, uint64_t argument_pointer,
                             uint64_t user_data_sel, uint64_t user_code_sel);

typedef struct {
    uint64_t entry;
    uint64_t user_stack_top;
    uint64_t argument_pointer;
} user_launch_arguments_t;

static void user_task_launcher(void *arg) {
    user_launch_arguments_t *arguments = (user_launch_arguments_t *)arg;
    uint64_t entry = arguments->entry;
    uint64_t stack = arguments->user_stack_top;
    uint64_t argument_pointer = arguments->argument_pointer;
    kfree(arguments);
    enter_user_mode(entry, stack, argument_pointer, GDT_USER_DATA_SEL | 3, GDT_USER_CODE_SEL | 3);
}

static const virtual_memory_range_t PROCESS_OWNED[] = {
        {USER_IMAGE_BASE, USER_IMAGE_LIMIT},
        {USER_STACK_LIMIT, USER_ARGUMENT_ADDRESS + USER_ARGUMENT_BYTES},
        {USER_HEAP_START, USER_HEAP_LIMIT},
        {USER_MMAP_BASE, USER_MMAP_LIMIT},
};
#define PROCESS_OWNED_COUNT ((int)(sizeof(PROCESS_OWNED) / sizeof(PROCESS_OWNED[0])))

void process_destroy_address_space(uint64_t pml4_phys) {
    if (pml4_phys == 0 || pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return;
    }
    virtual_memory_destroy_address_space(pml4_phys, PROCESS_OWNED, PROCESS_OWNED_COUNT);
}

uint64_t process_fork_address_space(uint64_t source_pml4_phys) {
    if (source_pml4_phys == 0 || source_pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return 0;
    }
    return virtual_memory_fork_address_space(source_pml4_phys, PROCESS_OWNED, PROCESS_OWNED_COUNT);
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
            kernel_log_debug("[elf] refused: unknown interpreter ");
            kernel_log_debug(interp_path);
            kernel_log_debug("\n");
            return 0;
        }
        leanfs_stat_t ist;
        int64_t n = (virtual_file_system_stat(interp_path, &ist) == 0) ? (int64_t)ist.size : -1;
        if (n <= 0) {
            kernel_log_debug("[elf] refused: " USER_INTERP_PATH " is not on this disk\n");
            return 0;
        }
        interp_bytes = (size_t)n;
        interp_image = (uint8_t *)kmalloc(interp_bytes);
        if (!interp_image) {
            return 0;
        }
        if (virtual_file_system_read(interp_path, interp_image, interp_bytes) != (int64_t)interp_bytes) {
            kfree(interp_image);
            return 0;
        }
        if (!elf_is_dyn(interp_image, interp_bytes)) {
            kernel_log_debug("[elf] refused: the interpreter is not a shared object\n");
            kfree(interp_image);
            return 0;
        }
    }

    uint64_t pml4_phys = virtual_memory_create_address_space();
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
        uint64_t phys = physical_memory_try_alloc_frame();
        if (phys == 0) {
            process_destroy_address_space(pml4_phys);
            return 0;
        }
        k_memset((void *)phys, 0, PAGE_SIZE);
        if (virtual_memory_try_map_page_in(pml4_phys, va, phys,
                                VIRTUAL_MEMORY_FLAG_WRITABLE | VIRTUAL_MEMORY_FLAG_USER) != 0) {
            physical_memory_free_frame(phys);
            process_destroy_address_space(pml4_phys);
            return 0;
        }
    }

    uint64_t argument_frames[USER_ARGUMENT_PAGES];
    int argument_pages = 0;
    size_t argument_used = 0;
    char *block = (char *)kmalloc(USER_ARGUMENT_BYTES);
    if (!block) {
        process_destroy_address_space(pml4_phys);
        return 0;
    }
    k_memset(block, 0, USER_ARGUMENT_BYTES);
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
        if (at > USER_ARGUMENT_BYTES) {
            at = USER_ARGUMENT_BYTES;
            argc = 0;
            envc = 0;
            strings_off = sizeof(uint64_t) * 3;
            at = strings_off;
        }
        int stored = 0;
        for (int i = 0; i < argc; i++) {
            size_t length = k_strlen(argv[i]) + 1;
            if (at + length > USER_ARGUMENT_BYTES) {
                break;
            }
            k_memcpy(block + at, argv[i], length);
            header[1 + (size_t)stored] = USER_ARGUMENT_ADDRESS + at;
            at += length;
            stored++;
        }
        header[0] = (uint64_t)stored;
        header[1 + (size_t)stored] = 0;

        size_t env_base = 1 + (size_t)stored + 1;
        int env_stored = 0;
        for (int i = 0; i < envc; i++) {
            size_t length = k_strlen(envp[i]) + 1;
            if (at + length > USER_ARGUMENT_BYTES ||
                (env_base + (size_t)env_stored + 1) * sizeof(uint64_t) > strings_off) {
                break;
            }
            k_memcpy(block + at, envp[i], length);
            header[env_base + (size_t)env_stored] = USER_ARGUMENT_ADDRESS + at;
            at += length;
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
        argument_used = at;
    }
    argument_pages = (int)((argument_used + PAGE_SIZE - 1) / PAGE_SIZE);
    if (argument_pages < 1) {
        argument_pages = 1;
    }
    for (int i = 0; i < argument_pages; i++) {
        argument_frames[i] = physical_memory_try_alloc_frame();
        if (argument_frames[i] == 0) {
            for (int j = 0; j < i; j++) {
                physical_memory_free_frame(argument_frames[j]);
            }
            kfree(block);
            process_destroy_address_space(pml4_phys);
            return 0;
        }
    }
    for (int i = 0; i < argument_pages; i++) {
        k_memcpy((void *)argument_frames[i], block + (size_t)i * PAGE_SIZE, PAGE_SIZE);
        if (virtual_memory_try_map_page_in(pml4_phys, USER_ARGUMENT_ADDRESS + (uint64_t)i * PAGE_SIZE,
                                argument_frames[i], VIRTUAL_MEMORY_FLAG_WRITABLE | VIRTUAL_MEMORY_FLAG_USER) != 0) {
            for (int j = i; j < argument_pages; j++) {
                physical_memory_free_frame(argument_frames[j]);
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
    if (!scheduler_has_free_task_slot()) {
        return (task_t *)0;
    }

    uint64_t entry = 0;
    uint64_t pml4_phys = process_build_address_space(image, image_size, argv, envp, &entry);
    if (pml4_phys == 0) {
        return (task_t *)0;
    }

    user_launch_arguments_t *arguments = (user_launch_arguments_t *)kmalloc(sizeof(user_launch_arguments_t));
    if (!arguments) {
        process_destroy_address_space(pml4_phys);
        return (task_t *)0;
    }
    arguments->entry = entry;
    arguments->user_stack_top = USER_STACK_TOP;
    arguments->argument_pointer = USER_ARGUMENT_ADDRESS;

    task_t *t = task_spawn_in(name, pml4_phys, user_task_launcher, arguments,
                               USER_HEAP_START, USER_SHARED_MEMORY_BASE);
    if (!t) {
        kfree(arguments);
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

task_t *process_spawnve_capped(const char *name, const uint8_t *image, size_t image_size,
                               const char *const *argv, const char *const *envp, uint32_t caps) {
    task_t *self = scheduler_vm_owner(scheduler_current());
    const char *inherited[USER_ENV_MAX_VARS + 1];
    const char *const *effective = envp;
    uint32_t inherited_length = 0;
    uint32_t inherited_count = 0;

    if (!envp && self && self->env_block && self->env_count) {
        uint32_t n = 0;
        uint32_t off = 0;
        while (off < self->env_length && n < USER_ENV_MAX_VARS) {
            inherited[n++] = self->env_block + off;
            while (off < self->env_length && self->env_block[off]) {
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
                    uint32_t length = (uint32_t)k_strlen(effective[i]) + 1;
                    if (inherited_length + length > USER_ENV_MAX_BYTES) {
                        break;
                    }
                    k_memcpy(packed + inherited_length, effective[i], length);
                    inherited_length += length;
                    inherited_count++;
                }
            }
            scheduler_set_env(t, packed, inherited_length, inherited_count);
            kfree(packed);
        }
        t->caps &= caps;
    }
    return t;
}

task_t *process_spawn_thread(const char *name, uint64_t entry, uint64_t stack_top,
                              uint64_t arg) {
    task_t *self = scheduler_current();
    if (!self || self->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return (task_t *)0;
    }
    user_launch_arguments_t *arguments = (user_launch_arguments_t *)kmalloc(sizeof(user_launch_arguments_t));
    if (!arguments) {
        return (task_t *)0;
    }
    arguments->entry = entry;
    arguments->user_stack_top = stack_top;
    arguments->argument_pointer = arg;

    task_t *t = task_spawn_thread(name, self, user_task_launcher, arguments);
    if (!t) {
        kfree(arguments);
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
