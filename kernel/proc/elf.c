#include "elf.h"

#include "drivers/klog.h"
#include "lib/libk.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/proc.h"

#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3

#define EI_CLASS  4
#define ELFCLASS64 2

#define ET_EXEC   2
#define ET_DYN    3
#define EM_X86_64 62
#define PT_LOAD   1
#define PT_INTERP 3
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

static uint64_t align_down(uint64_t x, uint64_t a) {
    return x & ~(a - 1);
}

static uint64_t align_up(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}

static uint64_t reject(const char *why) {
    klog_debug("[elf] rejected image: ");
    klog_debug(why);
    klog_debug("\n");
    return 0;
}

static uint64_t elf_validate_biased(const uint8_t *image, size_t image_size,
                                    uint64_t bias) {
    if (image_size < sizeof(elf64_ehdr_t)) {
        return reject("smaller than an ELF header");
    }
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
    if (eh->e_ident[EI_MAG0] != 0x7F || eh->e_ident[EI_MAG1] != 'E' ||
        eh->e_ident[EI_MAG2] != 'L' || eh->e_ident[EI_MAG3] != 'F') {
        return reject("bad magic - not an ELF file");
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        return reject("not ELFCLASS64");
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        return reject("not ET_EXEC or ET_DYN");
    }
    if (eh->e_type == ET_EXEC && bias != 0) {
        return reject("an ET_EXEC cannot be relocated - it names its own addresses");
    }
    if (eh->e_machine != EM_X86_64) {
        return reject("not an x86_64 image");
    }
    if (eh->e_entry + bias < USER_IMAGE_BASE ||
        eh->e_entry + bias >= USER_IMAGE_LIMIT) {
        return reject("entry point outside the user image window");
    }
    if (eh->e_phnum != 0 && eh->e_phentsize != sizeof(elf64_phdr_t)) {
        return reject("unexpected program-header entry size");
    }
    uint64_t ph_bytes = (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t);
    if (eh->e_phoff > image_size || ph_bytes > (uint64_t)image_size - eh->e_phoff) {
        return reject("program-header table runs past the end of the image");
    }

    const elf64_phdr_t *ph = (const elf64_phdr_t *)(image + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        uint64_t vaddr = ph[i].p_vaddr + bias;
        uint64_t filesz = ph[i].p_filesz;
        uint64_t memsz = ph[i].p_memsz;
        uint64_t offset = ph[i].p_offset;

        if (filesz > memsz) {
            return reject("segment p_filesz exceeds p_memsz");
        }
        if (offset > image_size || filesz > (uint64_t)image_size - offset) {
            return reject("segment file contents run past the end of the image");
        }
        if (vaddr < USER_IMAGE_BASE) {
            return reject("segment loads below the user image window");
        }
        if (memsz > USER_IMAGE_LIMIT - vaddr) {
            return reject("segment runs past the end of the user image window");
        }
    }

    return eh->e_entry + bias;
}

uint64_t elf_validate(const uint8_t *image, size_t image_size) {
    if (image_size >= sizeof(elf64_ehdr_t) &&
        ((const elf64_ehdr_t *)image)->e_type == ET_DYN) {
        return elf_validate_biased(image, image_size, USER_IMAGE_BASE);
    }
    return elf_validate_biased(image, image_size, 0);
}

int elf_is_dyn(const uint8_t *image, size_t image_size) {
    if (image_size < sizeof(elf64_ehdr_t)) {
        return 0;
    }
    return ((const elf64_ehdr_t *)image)->e_type == ET_DYN;
}

int elf_interp(const uint8_t *image, size_t image_size, char *out, size_t cap) {
    if (image_size < sizeof(elf64_ehdr_t) || !out || cap == 0) {
        return 0;
    }
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
    if (eh->e_phentsize != sizeof(elf64_phdr_t)) {
        return 0;
    }
    uint64_t ph_bytes = (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t);
    if (eh->e_phoff > image_size || ph_bytes > (uint64_t)image_size - eh->e_phoff) {
        return 0;
    }
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(image + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_INTERP) {
            continue;
        }
        if (ph[i].p_offset > image_size ||
            ph[i].p_filesz > (uint64_t)image_size - ph[i].p_offset ||
            ph[i].p_filesz == 0 || ph[i].p_filesz > cap) {
            return 0;
        }
        const char *src = (const char *)(image + ph[i].p_offset);
        size_t n = 0;
        while (n < ph[i].p_filesz - 1 && src[n]) {
            out[n] = src[n];
            n++;
        }
        out[n] = '\0';
        return n > 0;
    }
    return 0;
}

uint64_t elf_phdr_vaddr(const uint8_t *image, size_t image_size, uint64_t bias) {
    if (image_size < sizeof(elf64_ehdr_t)) {
        return 0;
    }
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
    if (eh->e_phentsize != sizeof(elf64_phdr_t)) {
        return 0;
    }
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(image + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        if (eh->e_phoff >= ph[i].p_offset &&
            eh->e_phoff < ph[i].p_offset + ph[i].p_filesz) {
            return ph[i].p_vaddr + bias + (eh->e_phoff - ph[i].p_offset);
        }
    }
    return 0;
}

uint16_t elf_phnum(const uint8_t *image, size_t image_size) {
    if (image_size < sizeof(elf64_ehdr_t)) {
        return 0;
    }
    return ((const elf64_ehdr_t *)image)->e_phnum;
}

uint64_t elf_load(uint64_t pml4_phys, const uint8_t *image, size_t image_size) {
    return elf_load_at(pml4_phys, image, image_size,
                       elf_is_dyn(image, image_size) ? USER_IMAGE_BASE : 0);
}

uint64_t elf_load_at(uint64_t pml4_phys, const uint8_t *image, size_t image_size,
                     uint64_t bias) {
    uint64_t entry = elf_validate_biased(image, image_size, bias);
    if (entry == 0) {
        return 0;
    }
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(image + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }

        uint64_t vaddr = ph[i].p_vaddr + bias;
        uint64_t filesz = ph[i].p_filesz;
        uint64_t memsz = ph[i].p_memsz;
        uint64_t offset = ph[i].p_offset;

        uint64_t seg_start = align_down(vaddr, PAGE_SIZE);
        uint64_t seg_end = align_up(vaddr + memsz, PAGE_SIZE);

        for (uint64_t page_va = seg_start; page_va < seg_end; page_va += PAGE_SIZE) {
            uint64_t phys = pmm_try_alloc_frame();
            if (phys == 0) {
                return reject("out of physical memory mapping a segment");
            }
            k_memset((void *)phys, 0, PAGE_SIZE);

            uint64_t file_start = vaddr;
            uint64_t file_end = vaddr + filesz;
            uint64_t overlap_start = page_va > file_start ? page_va : file_start;
            uint64_t page_end = page_va + PAGE_SIZE;
            uint64_t overlap_end = page_end < file_end ? page_end : file_end;
            if (overlap_start < overlap_end) {
                uint64_t file_off = offset + (overlap_start - vaddr);
                uint64_t page_off = overlap_start - page_va;
                k_memcpy((void *)(phys + page_off), image + file_off, overlap_end - overlap_start);
            }

            uint64_t seg_flags = VMM_FLAG_USER;
            if (ph[i].p_flags & PF_W) {
                seg_flags |= VMM_FLAG_WRITABLE;
            }
            if (ph[i].p_flags & PF_X) {
                seg_flags |= VMM_FLAG_EXEC;
            }
            if (vmm_try_map_page_in(pml4_phys, page_va, phys, seg_flags) != 0) {
                klog_debug("[elf] out of page tables mapping a segment\n");
                return 0;
            }
        }
    }

    klog_debug("[elf] loaded, entry = 0x");
    klog_log_hex64(KLOG_DEBUG, eh->e_entry + bias);
    klog_debug("\n");

    return eh->e_entry + bias;
}
