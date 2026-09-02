#include "elf.h"

#include "drivers/klog.h"
#include "lib/libk.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/proc.h" /* USER_IMAGE_BASE/USER_IMAGE_LIMIT - the window a segment must fit in */

#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3

#define EI_CLASS  4
#define ELFCLASS64 2

#define ET_EXEC   2
/* M95: a shared object, which is what a position-independent executable
 * and every shared library is. The difference from ET_EXEC is entirely
 * one of interpretation: its p_vaddr values are offsets from wherever it
 * is placed rather than addresses, so loading one means choosing a base
 * and adding it to everything. */
#define ET_DYN    3
#define EM_X86_64 62
#define PT_LOAD   1
/* M95: the path of the program that should be run INSTEAD of this one,
 * and handed this one to finish loading. That program is the dynamic
 * linker. */
#define PT_INTERP 3
/* Segment permission bits from the ELF spec (M91). */
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

/* One rejection reason, logged and turned into elf_load's 0 sentinel.
 * Logged rather than silent because a refused spawn is otherwise
 * indistinguishable from an app that started and immediately exited -
 * the same "make the failure visible" reasoning behind the compositor's
 * refuse_window (user_space/bin/compositor.c). KLOG_DEBUG, not an error:
 * a user program spawning garbage is an ordinary, recoverable event. */
static uint64_t reject(const char *why) {
    klog_debug("[elf] rejected image: ");
    klog_debug(why);
    klog_debug("\n");
    return 0;
}

/* M95: the common half of validate, parameterised by the load bias.
 *
 * `bias` is what is added to every p_vaddr and to the entry: 0 for an
 * ET_EXEC, and the chosen base for an ET_DYN. Everything else - the
 * magic, the class, the machine, the bounds - is the same question
 * either way, which is why there is one function rather than two that
 * would drift.
 */
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
    /* Overflow-safe bounds check on the program-header table itself:
     * e_phoff and e_phnum both come straight out of the file, so
     * `e_phoff + e_phnum * e_phentsize` computed naively could wrap and
     * pass a comparison it should fail. Subtracting from image_size
     * instead can't. */
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
    /* An ET_DYN validated with no bias is validated as though it were
     * loaded at 0, which its entry point (usually a small offset) will
     * fail. So the default bias for one is the image base - the same
     * place elf_load_at puts it when a caller does not choose. */
    if (image_size >= sizeof(elf64_ehdr_t) &&
        ((const elf64_ehdr_t *)image)->e_type == ET_DYN) {
        return elf_validate_biased(image, image_size, USER_IMAGE_BASE);
    }
    return elf_validate_biased(image, image_size, 0);
}

/* M95: is this image a shared object rather than a fixed executable? */
int elf_is_dyn(const uint8_t *image, size_t image_size) {
    if (image_size < sizeof(elf64_ehdr_t)) {
        return 0;
    }
    return ((const elf64_ehdr_t *)image)->e_type == ET_DYN;
}

/* M95: the interpreter this image asks for, if it asks for one.
 *
 * Returns 1 and fills `out` with the NUL-terminated path, or 0. The
 * string lives in the file rather than in a section this loader has
 * mapped, so it is copied out here where the whole image is still in
 * kernel memory - by the time the address space exists, the only thing
 * that could read it is the program itself.
 */
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
        /* NUL-terminated in the file by convention; terminated here
         * regardless, because "by convention" is not a bound. */
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

/* M95: where this image's program headers end up in the address space,
 * so the interpreter can find them - which is what AT_PHDR is for.
 *
 * The header table is inside the first PT_LOAD on every image any
 * toolchain produces, so this finds the segment containing e_phoff and
 * reports the corresponding virtual address. 0 if no segment covers it,
 * which is an image no dynamic linker could load anyway. */
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
    /* Validate the whole image before mapping any of it, so a rejected
     * one never allocates a frame or leaves a half-populated address
     * space behind (there is no unmap-and-free path to undo one with -
     * see vmm.h). Callers that want the answer *before* committing to an
     * address space at all call elf_validate directly; doing it again
     * here costs one pass over a handful of program headers and means
     * elf_load is never unsafe on its own. */
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
            /* M40: pmm_try_alloc_frame, not pmm_alloc_frame - SYS_spawn is
             * reachable from any user program, so one process asking for
             * more physical memory than exists must not panic every other
             * task's along with it (pmm.h spells out this same rule for
             * SYS_sbrk and shm_create). Frames already mapped into this
             * doomed address space are left behind rather than unwound:
             * same tradeoff process_spawn's own task-table-full path
             * documents, and only reachable on genuine exhaustion. */
            uint64_t phys = pmm_try_alloc_frame();
            if (phys == 0) {
                return reject("out of physical memory mapping a segment");
            }
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

            /* M91: the segment's own p_flags, honoured rather than
             * ignored. Every page of every program was mapped writable
             * and (before this milestone there was no other option)
             * executable; a program's text is now read-only and its data
             * is not executable, which is the first W^X boundary this OS
             * has had. user_space/lib/user.ld is the other half - a
             * single RWE segment, which is what it produced until now,
             * gives this nothing to honour. */
            uint64_t seg_flags = VMM_FLAG_USER;
            if (ph[i].p_flags & PF_W) {
                seg_flags |= VMM_FLAG_WRITABLE;
            }
            if (ph[i].p_flags & PF_X) {
                seg_flags |= VMM_FLAG_EXEC;
            }
            vmm_map_page_in(pml4_phys, page_va, phys, seg_flags);
        }
    }

    klog_debug("[elf] loaded, entry = 0x");
    klog_log_hex64(KLOG_DEBUG, eh->e_entry + bias);
    klog_debug("\n");

    return eh->e_entry + bias;
}
