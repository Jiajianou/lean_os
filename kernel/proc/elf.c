#include "elf.h"

#include "drivers/klog.h"
#include "lib/libk.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL

#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3

#define ET_EXEC   2
#define EM_X86_64 62
#define PT_LOAD   1

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

uint64_t elf_load(uint64_t pml4_phys, const uint8_t *image, size_t image_size) {
    if (image_size < sizeof(elf64_ehdr_t)) {
        panic("elf_load: image smaller than an ELF header");
    }
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
    if (eh->e_ident[EI_MAG0] != 0x7F || eh->e_ident[EI_MAG1] != 'E' ||
        eh->e_ident[EI_MAG2] != 'L' || eh->e_ident[EI_MAG3] != 'F') {
        panic("elf_load: bad magic - not an ELF file");
    }
    if (eh->e_type != ET_EXEC) {
        panic("elf_load: only ET_EXEC (static, non-PIE) executables are supported");
    }
    if (eh->e_machine != EM_X86_64) {
        panic("elf_load: not an x86_64 image");
    }

    const elf64_phdr_t *ph = (const elf64_phdr_t *)(image + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }

        uint64_t vaddr = ph[i].p_vaddr;
        uint64_t filesz = ph[i].p_filesz;
        uint64_t memsz = ph[i].p_memsz;
        uint64_t offset = ph[i].p_offset;

        uint64_t seg_start = align_down(vaddr, PAGE_SIZE);
        uint64_t seg_end = align_up(vaddr + memsz, PAGE_SIZE);

        for (uint64_t page_va = seg_start; page_va < seg_end; page_va += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_frame();
            /* Frames pmm hands out always live inside the always-identity-
             * mapped first 1 GiB (pmm.h), so this raw physical write is
             * safe regardless of which CR3 is currently loaded. */
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

            vmm_map_page_in(pml4_phys, page_va, phys, VMM_FLAG_WRITABLE | VMM_FLAG_USER);
        }
    }

    klog_debug("[elf] loaded, entry = 0x");
    klog_log_hex64(KLOG_DEBUG, eh->e_entry);
    klog_debug("\n");

    return eh->e_entry;
}
