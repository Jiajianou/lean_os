typedef unsigned long long u64;
typedef long long i64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#include "syscall.h"
#include "mman.h"
#include "proc.h"

static i64 sys(long n, long a, long b, long c) {
    i64 r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "memory");
    return r;
}

static i64 sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    i64 r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "c"(d), "r"(r8), "r"(r9)
                     : "memory");
    return r;
}

static void dl_write(const char *s) {
    long n = 0;
    while (s[n]) {
        n++;
    }
    sys(SYS_write, 2, (long)s, n);
}

static void dl_hex(u64 v) {
    static const char digits[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xf];
    }
    buf[18] = 0;
    dl_write(buf);
}

static u64 dl_recover[8];
static int dl_recovering;
static char dl_fail_text[256];

static void dl_fail_record(const char *what, const char *detail) {
    u64 n = 0;
    for (const char *p = what; *p && n < sizeof(dl_fail_text) - 1; p++) {
        dl_fail_text[n++] = *p;
    }
    if (detail) {
        const char *sep = ": ";
        for (const char *p = sep; *p && n < sizeof(dl_fail_text) - 1; p++) {
            dl_fail_text[n++] = *p;
        }
        for (const char *p = detail; *p && n < sizeof(dl_fail_text) - 1; p++) {
            dl_fail_text[n++] = *p;
        }
    }
    dl_fail_text[n] = 0;
}

void _dl_longjmp(u64 *buf, int val) __attribute__((noreturn));
int _dl_setjmp(u64 *buf);

__attribute__((noreturn))
static void dl_fail(const char *what, const char *detail) {
    if (dl_recovering) {
        dl_fail_record(what, detail);
        _dl_longjmp(dl_recover, 1);
    }
    dl_write("ld-lean: ");
    dl_write(what);
    if (detail) {
        dl_write(": ");
        dl_write(detail);
    }
    dl_write("\n");
    sys(SYS_exit, 127, 0, 0);
    for (;;) {
    }
}

static void *dl_memset(void *d, int c, u64 n) {
    u8 *p = (u8 *)d;
    for (u64 i = 0; i < n; i++) {
        p[i] = (u8)c;
    }
    return d;
}

static void *dl_memcpy(void *d, const void *s, u64 n) {
    u8 *a = (u8 *)d;
    const u8 *b = (const u8 *)s;
    for (u64 i = 0; i < n; i++) {
        a[i] = b[i];
    }
    return d;
}

static int dl_streq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static u64 dl_strlen(const char *s) {
    u64 n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_TLS     7

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_INIT     12
#define DT_FINI     13
#define DT_SONAME   14
#define DT_RPATH    15
#define DT_JMPREL   23
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_GNU_HASH 0x6ffffef5

#define R_X86_64_NONE      0
#define R_X86_64_64        1
#define R_X86_64_COPY      5
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8
#define R_X86_64_DTPMOD64  16
#define R_X86_64_DTPOFF64  17
#define R_X86_64_TPOFF64   18

#define SHN_UNDEF 0
#define STB_WEAK  2

typedef struct {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} Ehdr;

typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} Phdr;

typedef struct {
    i64 d_tag;
    u64 d_value;
} Dyn;

typedef struct {
    u32 st_name;
    u8  st_info;
    u8  st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;
} Sym;

typedef struct {
    u64 r_offset;
    u64 r_info;
    i64 r_addend;
} Rela;

#define ELF64_R_SYM(i)  ((u32)((i) >> 32))
#define ELF64_R_TYPE(i) ((u32)((i) & 0xffffffffu))
#define ELF64_ST_BIND(i) ((i) >> 4)

#define MAX_OBJECTS 96

typedef struct {
    u64 base;
    u64 tls_offset;
    u64 tls_memsz;
    u64 tls_filesz;
    const void *tls_image;
    const char *name;
    const Dyn *dyn;
    const char *strtab;
    const Sym *symbol_table;
    const u32 *gnu_hash;
    const u32 *elf_hash;
    const Rela *rela;
    u64 rela_count;
    const Rela *jmprel;
    u64 jmprel_count;
    void (**init_array)(void);
    u64 init_count;
    void (*init)(void);
    int relocated;
} Object;

static Object objects[MAX_OBJECTS];
static int object_count;

static char name_arena[8192];
static u64 name_used;

static const char *dl_strdup(const char *s) {
    u64 n = dl_strlen(s) + 1;
    if (name_used + n > sizeof(name_arena)) {
        dl_fail("no room left in the shared-object name arena for", s);
    }
    char *p = name_arena + name_used;
    dl_memcpy(p, s, n);
    name_used += n;
    return p;
}

__attribute__((no_sanitize_address))
static void self_relocate(u64 base, const Dyn *dyn) {
    const Rela *rela = 0;
    u64 relasz = 0;
    for (const Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_RELA) {
            rela = (const Rela *)(base + d->d_value);
        } else if (d->d_tag == DT_RELASZ) {
            relasz = d->d_value;
        }
    }
    if (!rela) {
        return;
    }
    for (u64 i = 0; i < relasz / sizeof(Rela); i++) {
        if (ELF64_R_TYPE(rela[i].r_info) == R_X86_64_RELATIVE) {
            *(u64 *)(base + rela[i].r_offset) = base + (u64)rela[i].r_addend;
        }
    }
}

static u64 lookup(const char *name, const Object *skip, int *found) {
    *found = 0;
    for (int i = 0; i < object_count; i++) {
        const Object *o = &objects[i];
        if (o == skip || !o->symbol_table || !o->strtab) {
            continue;
        }
        for (u32 s = 0; ; s++) {
            const Sym *symbol = &o->symbol_table[s];
            if ((const char *)symbol >= o->strtab) {
                break;
            }
            if (symbol->st_shndx == SHN_UNDEF || symbol->st_name == 0) {
                continue;
            }
            if (!dl_streq(o->strtab + symbol->st_name, name)) {
                continue;
            }
            *found = 1;
            return o->base + symbol->st_value;
        }
    }
    return 0;
}

static void apply_rela(Object *o, const Rela *r, u64 count) {
    for (u64 i = 0; i < count; i++) {
        u32 type = ELF64_R_TYPE(r[i].r_info);
        u32 symi = ELF64_R_SYM(r[i].r_info);
        u64 *where = (u64 *)(o->base + r[i].r_offset);

        if (type == R_X86_64_RELATIVE) {
            *where = o->base + (u64)r[i].r_addend;
            continue;
        }
        if (type == R_X86_64_NONE) {
            continue;
        }

        const Sym *symbol = &o->symbol_table[symi];
        const char *name = o->strtab + symbol->st_name;

        if (symi == 0 && type == R_X86_64_TPOFF64) {
            if (o->tls_offset == 0) {
                dl_fail("a thread-local relocation in an object with no TLS "
                        "block", o->name);
            }
            *where = (u64)(-(i64)o->tls_offset) + (u64)r[i].r_addend;
            continue;
        }

        int found = 0;
        u64 value = lookup(name, 0, &found);
        if (!found) {
            if (ELF64_ST_BIND(symbol->st_info) == STB_WEAK) {
                value = 0;
            } else {
                dl_fail("undefined symbol", name);
            }
        }

        switch (type) {
        case R_X86_64_64:
            *where = value + (u64)r[i].r_addend;
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            *where = value;
            break;
        case R_X86_64_COPY:
            {
                int source_found = 0;
                u64 src = lookup(name, &objects[0], &source_found);
                if (!source_found) {
                    dl_fail("undefined symbol for a copy relocation", name);
                }
                dl_memcpy(where, (const void *)src, symbol->st_size);
            }
            break;
        case R_X86_64_TPOFF64: {
            const Object *def = 0;
            for (int k = 0; k < object_count; k++) {
                if (!objects[k].symbol_table || !objects[k].strtab) {
                    continue;
                }
                for (u32 t = 0; ; t++) {
                    const Sym *cand = &objects[k].symbol_table[t];
                    if ((const char *)cand >= objects[k].strtab) {
                        break;
                    }
                    if (cand->st_shndx == SHN_UNDEF || cand->st_name == 0) {
                        continue;
                    }
                    if (dl_streq(objects[k].strtab + cand->st_name, name)) {
                        def = &objects[k];
                        value = cand->st_value;
                        break;
                    }
                }
                if (def) {
                    break;
                }
            }
            if (!def || def->tls_offset == 0) {
                dl_fail("thread-local symbol in an object with no static TLS "
                        "slice", name);
            }
            *where = (u64)(-(i64)def->tls_offset) + value + (u64)r[i].r_addend;
            break;
        }
        case R_X86_64_DTPMOD64:
        case R_X86_64_DTPOFF64:
            dl_fail("the general-dynamic TLS model is not supported here", name);
            break;
        default:
            dl_fail("unsupported relocation type in", o->name);
        }
    }
}

static void scan_dynamic(Object *o) {
    u64 relasz = 0, pltrelsz = 0, init_arraysz = 0;
    for (const Dyn *d = o->dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB: o->strtab = (const char *)(o->base + d->d_value); break;
        case DT_SYMTAB: o->symbol_table = (const Sym *)(o->base + d->d_value); break;
        case DT_RELA:   o->rela = (const Rela *)(o->base + d->d_value); break;
        case DT_RELASZ: relasz = d->d_value; break;
        case DT_JMPREL: o->jmprel = (const Rela *)(o->base + d->d_value); break;
        case DT_PLTRELSZ: pltrelsz = d->d_value; break;
        case DT_INIT:
            if (d->d_value != 0) {
                o->init = (void (*)(void))(o->base + d->d_value);
            }
            break;
        case DT_INIT_ARRAY:
            if (d->d_value != 0) {
                o->init_array = (void (**)(void))(o->base + d->d_value);
            }
            break;
        case DT_INIT_ARRAYSZ: init_arraysz = d->d_value; break;
        default: break;
        }
    }
    o->rela_count = relasz / sizeof(Rela);
    o->jmprel_count = pltrelsz / sizeof(Rela);
    o->init_count = init_arraysz / sizeof(void *);
}

static u64 next_lib_base;

static Object *load_object(const char *soname);

static const char *ld_library_path;

static int open_lib(const char *soname, char *path, u64 cap) {
    static const char *const defaults[] = {"/lib/", "/usr/lib/", 0};

    for (const char *q = soname; *q; q++) {
        if (*q != '/') {
            continue;
        }
        u64 n = 0;
        while (soname[n] && n < cap - 1) {
            path[n] = soname[n];
            n++;
        }
        path[n] = 0;
        if (soname[n]) {
            return -1;
        }
        i64 fd = sys(SYS_open, (long)path, 1  , 0);
        return fd >= 0 ? (int)fd : -1;
    }

    for (const char *p = ld_library_path; p && *p;) {
        u64 n = 0;
        while (*p && *p != ':' && n < cap - 2) {
            path[n++] = *p++;
        }
        while (*p && *p != ':') {
            p++;
        }
        if (*p == ':') {
            p++;
        }
        if (n == 0) {
            continue;
        }
        if (path[n - 1] != '/') {
            path[n++] = '/';
        }
        u64 m = 0;
        while (soname[m] && n < cap - 1) {
            path[n++] = soname[m++];
        }
        path[n] = 0;
        if (soname[m]) {
            continue;
        }
        i64 fd = sys(SYS_open, (long)path, 1  , 0);
        if (fd >= 0) {
            return (int)fd;
        }
    }

    for (int i = 0; defaults[i]; i++) {
        u64 n = 0;
        const char *d = defaults[i];
        while (d[n] && n < cap - 1) {
            path[n] = d[n];
            n++;
        }
        u64 m = 0;
        while (soname[m] && n < cap - 1) {
            path[n++] = soname[m++];
        }
        path[n] = 0;
        i64 fd = sys(SYS_open, (long)path, 1  , 0);
        if (fd >= 0) {
            return (int)fd;
        }
    }
    return -1;
}

static Object *load_object(const char *soname) {
    for (int i = 0; i < object_count; i++) {
        if (objects[i].name && dl_streq(objects[i].name, soname)) {
            return &objects[i];
        }
    }
    if (object_count >= MAX_OBJECTS) {
        dl_fail("too many shared objects", soname);
    }

    char path[256];
    int fd = open_lib(soname, path, sizeof(path));
    if (fd < 0) {
        dl_fail("cannot find shared object", soname);
    }

    static u8 header[4096];
    i64 got = sys(SYS_read, fd, (long)header, sizeof(header));
    if (got < (i64)sizeof(Ehdr)) {
        dl_fail("short read on", soname);
    }
    const Ehdr *eh = (const Ehdr *)header;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        dl_fail("not an ELF file", soname);
    }
    const Phdr *ph = (const Phdr *)(header + eh->e_phoff);

    u64 lo = ~0ull, hi = 0;
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        if (ph[i].p_vaddr < lo) {
            lo = ph[i].p_vaddr;
        }
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) {
            hi = ph[i].p_vaddr + ph[i].p_memsz;
        }
    }
    if (lo == ~0ull) {
        dl_fail("no loadable segments in", soname);
    }
    lo &= ~4095ull;
    hi = (hi + 4095ull) & ~4095ull;

    u64 base = next_lib_base;
    next_lib_base += hi - lo + 0x200000ull;

    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        u64 va = (base + ph[i].p_vaddr) & ~4095ull;
        u64 off = ph[i].p_vaddr & 4095ull;
        u64 len = (ph[i].p_memsz + off + 4095ull) & ~4095ull;
        int writable = (ph[i].p_flags & 2) != 0;
        int prot = 1  ;
        if (writable) {
            prot |= 2;
        }
        if (ph[i].p_flags & 1) {
            prot |= 4;
        }
        int flags = MAP_FIXED | (writable ? MAP_PRIVATE : MAP_SHARED);
        u64 file_off = (ph[i].p_offset - off) & ~4095ull;
        i64 r = sys6(SYS_mmap, (long)va, (long)len, prot, flags, fd, (long)file_off);
        if (r < 0) {
            dl_write("ld-lean: mmap va=");
            dl_hex(va);
            dl_write(" len=");
            dl_hex(len);
            dl_write(" off=");
            dl_hex(file_off);
            dl_write(" prot=");
            dl_hex((u64)prot);
            dl_write(" flags=");
            dl_hex((u64)flags);
            dl_write("\n");
            dl_fail("cannot map a segment of", soname);
        }
        if (writable && ph[i].p_memsz > ph[i].p_filesz) {
            u64 zfrom = base + ph[i].p_vaddr + ph[i].p_filesz;
            u64 zto = base + ph[i].p_vaddr + ph[i].p_memsz;
            dl_memset((void *)zfrom, 0, zto - zfrom);
        }
    }
    sys(SYS_close, fd, 0, 0);

    Object *o = &objects[object_count++];
    dl_memset(o, 0, sizeof(*o));
    o->base = base;
    o->name = dl_strdup(soname);
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            o->dyn = (const Dyn *)(base + ph[i].p_vaddr);
        }
    }
    if (!o->dyn) {
        dl_fail("no dynamic section in", soname);
    }
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_TLS) {
            o->tls_memsz = ph[i].p_memsz;
            o->tls_filesz = ph[i].p_filesz;
            o->tls_image = (const void *)(base + ph[i].p_vaddr);
        }
    }
    scan_dynamic(o);

    for (const Dyn *d = o->dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED) {
            load_object(o->strtab + d->d_value);
        }
    }
    return o;
}

u64 _dl_entry(u64 *arguments);

u64 _dl_entry(u64 *arguments) {
    u64 argc = arguments[0];
    u64 *envp = &arguments[1 + argc + 1];
    u64 i = 0;
    while (envp[i]) {
        i++;
    }
    u64 *aux = &envp[i + 1];

    for (u64 e = 0; envp[e]; e++) {
        const char *v = (const char *)envp[e];
        const char *k = "LD_LIBRARY_PATH=";
        u64 j = 0;
        while (k[j] && v[j] == k[j]) {
            j++;
        }
        if (!k[j]) {
            ld_library_path = v + j;
            break;
        }
    }

    u64 at_phdr = 0, at_phnum = 0, at_entry = 0, at_base = 0;
    for (u64 k = 0; aux[k] != AT_NULL; k += 2) {
        switch (aux[k]) {
        case AT_PHDR:  at_phdr = aux[k + 1]; break;
        case AT_PHNUM: at_phnum = aux[k + 1]; break;
        case AT_ENTRY: at_entry = aux[k + 1]; break;
        case AT_BASE:  at_base = aux[k + 1]; break;
        default: break;
        }
    }
    if (at_phdr == 0 || at_entry == 0) {
        dl_fail("the kernel did not describe the program", 0);
    }

    next_lib_base = 0x000000A000000000ull;

    Object *prog = &objects[object_count++];
    dl_memset(prog, 0, sizeof(*prog));
    prog->name = "";
    const Phdr *ph = (const Phdr *)at_phdr;
    for (u64 k = 0; k < at_phnum; k++) {
        if (ph[k].p_type == PT_LOAD && ph[k].p_offset == 0) {
            prog->base = at_phdr - ph[k].p_vaddr - sizeof(Ehdr);
            break;
        }
    }
    for (u64 k = 0; k < at_phnum; k++) {
        if (ph[k].p_type == PT_DYNAMIC) {
            prog->dyn = (const Dyn *)(prog->base + ph[k].p_vaddr);
        }
    }
    if (!prog->dyn) {
        dl_fail("the program has no dynamic section", 0);
    }
    for (u64 k = 0; k < at_phnum; k++) {
        if (ph[k].p_type == PT_TLS) {
            prog->tls_memsz = ph[k].p_memsz;
            prog->tls_filesz = ph[k].p_filesz;
            prog->tls_image = (const void *)(prog->base + ph[k].p_vaddr);
        }
    }
    scan_dynamic(prog);

    if (at_base != 0 && object_count < MAX_OBJECTS) {
        Object *self = &objects[object_count++];
        dl_memset(self, 0, sizeof(*self));
        self->base = at_base;
        self->name = "ld-lean.so";
        extern const Dyn _DYNAMIC[];
        self->dyn = _DYNAMIC;
        scan_dynamic(self);
        self->relocated = 1;
    }

    for (const Dyn *d = prog->dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED) {
            load_object(prog->strtab + d->d_value);
        }
    }

    {
        u64 total = 0;
        for (int k = 0; k < object_count; k++) {
            if (objects[k].tls_memsz == 0) {
                continue;
            }
            total = (total + objects[k].tls_memsz + 15u) & ~15ull;
            objects[k].tls_offset = total;
        }
        if (total > 0) {
            u64 want = (total + 15u + 64u) & ~15ull;
            i64 mem = sys6(SYS_mmap, 0, (long)want, 3  ,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mem < 0) {
                dl_fail("no memory for the thread-local block", 0);
            }
            u64 tp = (u64)mem + total;
            for (int k = 0; k < object_count; k++) {
                if (objects[k].tls_memsz == 0 || !objects[k].tls_image) {
                    continue;
                }
                dl_memcpy((void *)(tp - objects[k].tls_offset),
                          objects[k].tls_image, objects[k].tls_filesz);
            }
            *(u64 *)tp = tp;
            if (sys(SYS_arch_prctl, ARCH_SET_FS, (long)tp, 0) != 0) {
                dl_fail("the kernel refused a thread pointer", 0);
            }
        }
    }

    for (int k = object_count - 1; k >= 0; k--) {
        apply_rela(&objects[k], objects[k].rela, objects[k].rela_count);
        apply_rela(&objects[k], objects[k].jmprel, objects[k].jmprel_count);
        objects[k].relocated = 1;
    }

    for (int k = object_count - 1; k >= 0; k--) {
        if (objects[k].init) {
            objects[k].init();
        }
        for (u64 j = 0; j < objects[k].init_count; j++) {
            if (objects[k].init_array[j]) {
                objects[k].init_array[j]();
            }
        }
    }

    (void)at_base;
    return at_entry;
}

#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_GLOBAL 0x0100
#define RTLD_LOCAL  0x0000

static const char *dl_error_message;

void *dlopen(const char *file, int flags);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);

__attribute__((visibility("default")))
void *dlopen(const char *file, int flags) {
    (void)flags;
    dl_error_message = 0;
    if (!file) {
        return &objects[0];
    }
    if (object_count >= MAX_OBJECTS) {
        dl_error_message = "too many shared objects loaded";
        return 0;
    }

    volatile int before = object_count;
    volatile u64 name_mark = name_used;

    volatile int was_recovering = dl_recovering;
    volatile u64 saved_recover[8];
    for (int i = 0; i < 8; i++) {
        saved_recover[i] = dl_recover[i];
    }

    if (_dl_setjmp(dl_recover)) {
        dl_recovering = was_recovering;
        for (int i = 0; i < 8; i++) {
            dl_recover[i] = saved_recover[i];
        }
        object_count = before;
        name_used = name_mark;
        dl_error_message = dl_fail_text;
        return 0;
    }
    dl_recovering = 1;

    Object *o = load_object(file);
    if (!o) {
        dl_recovering = was_recovering;
        for (int i = 0; i < 8; i++) {
            dl_recover[i] = saved_recover[i];
        }
        dl_error_message = "cannot load shared object";
        return 0;
    }
    for (int k = object_count - 1; k >= before; k--) {
        if (objects[k].relocated) {
            continue;
        }
        apply_rela(&objects[k], objects[k].rela, objects[k].rela_count);
        apply_rela(&objects[k], objects[k].jmprel, objects[k].jmprel_count);
        objects[k].relocated = 1;
        if (objects[k].init) {
            objects[k].init();
        }
        for (u64 j = 0; j < objects[k].init_count; j++) {
            if (objects[k].init_array[j]) {
                objects[k].init_array[j]();
            }
        }
    }
    dl_recovering = was_recovering;
    for (int i = 0; i < 8; i++) {
        dl_recover[i] = saved_recover[i];
    }
    return o;
}

__attribute__((visibility("default")))
void *dlsym(void *handle, const char *name) {
    dl_error_message = 0;
    if (!handle || !name) {
        dl_error_message = "dlsym: null handle or name";
        return 0;
    }
    Object *o = (Object *)handle;
    if (!o->symbol_table || !o->strtab) {
        dl_error_message = "dlsym: that handle has no symbols";
        return 0;
    }
    for (u32 s = 0; ; s++) {
        const Sym *symbol = &o->symbol_table[s];
        if ((const char *)symbol >= o->strtab) {
            break;
        }
        if (symbol->st_shndx == SHN_UNDEF || symbol->st_name == 0) {
            continue;
        }
        if (dl_streq(o->strtab + symbol->st_name, name)) {
            return (void *)(o->base + symbol->st_value);
        }
    }
    dl_error_message = "dlsym: no such symbol";
    return 0;
}

__attribute__((visibility("default")))
int dlclose(void *handle) {
    (void)handle;
    dl_error_message = 0;
    return 0;
}

__attribute__((visibility("default")))
char *dlerror(void) {
    char *m = (char *)dl_error_message;
    dl_error_message = 0;
    return m;
}

void _dl_relocate_self(u64 base, const Dyn *dyn);

void _dl_relocate_self(u64 base, const Dyn *dyn) {
    self_relocate(base, dyn);
}
