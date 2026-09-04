/* user_space/ld/ld-lean.c - M95: the program that loads programs.
 *
 * ======================================================================
 * WHAT THIS IS
 * ======================================================================
 *
 * A dynamic linker. The kernel loads a program that has a PT_INTERP, and
 * then runs THIS instead - handing it, through the auxiliary vector, the
 * four facts it cannot work out for itself (see system_api/proc.h). This
 * finishes the job: it maps the shared objects the program needs,
 * applies every relocation in all of them, and jumps to the program's
 * own entry point.
 *
 * ======================================================================
 * THE THREE RULES THAT MAKE IT DIFFERENT FROM ANY OTHER PROGRAM
 * ======================================================================
 *
 * 1. **It has no libc.** It cannot: libc.so is one of the things it is
 *    about to load. Everything it uses is in this file, and the syscalls
 *    are written out as inline assembly rather than called through
 *    user_space/lib, because that library is compiled for a fixed
 *    address and this runs at whatever base the kernel chose.
 *
 * 2. **It must relocate itself before it can do anything.** It is an
 *    ET_DYN like everything else it loads, so its own global variables
 *    and string constants are at addresses that are only correct once
 *    its own R_X86_64_RELATIVE relocations have been applied. Until
 *    `self_relocate` has run, this code may not touch a global, may not
 *    call through a function pointer, and may not use a string literal
 *    that the compiler put in .rodata and referenced through the GOT.
 *    That is why `_dl_start` does exactly one thing.
 *
 * 3. **It cannot fail gracefully.** There is nowhere to report to: the
 *    program has not started, and stdout may be anything. A failure
 *    writes a line to fd 2 and exits with a distinctive code, which is
 *    the whole of its error handling.
 *
 * ======================================================================
 * WHAT IT DOES AND DOES NOT IMPLEMENT
 * ======================================================================
 *
 * Relocations: R_X86_64_RELATIVE, GLOB_DAT, JUMP_SLOT, 64, COPY and
 * TPOFF64. That is every type a static-model x86-64 shared object
 * produces.
 *
 * Binding is EAGER - every JUMP_SLOT is resolved at load time and the
 * PLT is never entered lazily. M95's bullet lists lazy binding, and this
 * is a deliberate refusal rather than an omission: lazy binding buys
 * start-up time in exchange for a resolver that runs on an arbitrary
 * stack in the middle of a call, and nothing here has measured a
 * start-up cost. M69's rule. What it costs is resolving symbols a
 * program never calls, which for the objects on this machine is
 * microseconds.
 *
 * Not here, each with a reason:
 *   - symbol versioning: one libc, built with the program.
 *   - LD_PRELOAD: an interposition mechanism with no user.
 *   - initial-exec TLS in a dlopen'ed object: needs a TLS module list,
 *     which is M96's general-dynamic case and is written down there.
 */

typedef unsigned long long u64;
typedef long long i64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

/* ---- syscalls, written out because there is no library to call -------
 *
 * The numbers are system_api/include/syscall.h's, and this file cannot
 * include it - not because of anything about the header, but because
 * every line here must be position-independent and a header of constants
 * is fine. It DOES include it; the numbers below are named, not
 * repeated. */
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

/* Six arguments: rdi, rsi, rdx, rcx, r8, r9 - and the fourth is RCX,
 * not r10.
 *
 * That is this OS's ABI rather than Linux's, and system_api's own header
 * says why: `int 0x80` does not clobber rcx and r11 the way `syscall`
 * does, "so there's no need to shuffle arg 4 into r10 the way the raw
 * Linux syscall ABI has to". Writing r10 here instead - which is what
 * anyone who has written a Linux syscall stub does from memory - passes
 * garbage as the fourth argument, and the first thing that goes wrong is
 * an mmap whose `flags` are whatever was in rcx. */
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

/* A hex number, for the failure messages. There is no printf here and a
 * failure that cannot say WHICH address it was about costs an hour. */
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

/* ---- M99: where a failure goes ---------------------------------------
 *
 * At startup, nowhere: a program whose libraries cannot be loaded has
 * not begun, there is nobody to return an error to, and printing and
 * exiting is the whole of the right answer.
 *
 * Inside dlopen it is a different question with a different answer, and
 * conflating them was a bug rather than a simplification. POSIX says
 * dlopen returns NULL and leaves a message for dlerror(); this loader
 * called exit(127) from eighteen places, so `dlopen` of anything that
 * did not work killed the caller. The fixture that found it is
 * tests/dynamic/manydyn.c, whose last check is the one that fails - and
 * the reason it matters is not the missing file, which a program can
 * stat for itself. It is that **an interpreter dlopens a module and
 * expects to be told no**: CPython turns a failed dlopen into an
 * ImportError, and against a loader that exits, one unresolved symbol in
 * one extension module ends the process with no traceback and no
 * message.
 *
 * `dl_recovering` is set only for the span of a dlopen. The recovery
 * point is _dl_setjmp's (see ld-start.S, which has this linker's own
 * sixteen-instruction copy, because the C library that would have
 * provided one is a thing this file loads).
 */
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
        /* Recorded rather than printed: the caller asked a question and
         * is going to be told the answer through dlerror(). A loader
         * that also wrote to stderr would make every probe an
         * interpreter makes look like a fault in the log. */
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

/* ---- ELF, again, and deliberately its own copy -----------------------
 *
 * kernel/proc/elf.c has these structures too. They are not shared,
 * because this file cannot include a kernel header and the kernel must
 * not include a user-space one - and because the two read different
 * parts of the format: the kernel reads program headers to map a file,
 * and this reads the dynamic section to relocate one. Two readers of one
 * format is not duplication; two copies of one reader would be.
 */
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
    u64 d_val;
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

/* ---- the loaded objects ----------------------------------------------
 *
 * A flat array rather than a list. The order matters and is the ELF
 * search scope: the program is first, then its DT_NEEDED objects in the
 * order they were named, breadth first. A symbol resolves to the FIRST
 * definition found, which is what makes a program able to override a
 * library's.
 *
 * ---- M99: why this stopped being sixteen ------------------------------
 *
 * M95 wrote sixteen and said why: "this machine's programs link against
 * libc.so and nothing else". That was true of every program that
 * existed. It is not true of an interpreter, which opens one object per
 * C extension module it imports - CPython's standard library has more
 * than forty - and the condition dlclose's note names for revisiting
 * this is exactly "a program that dlopens more than MAX_OBJECTS things
 * over its life". Python is that program.
 *
 * Ninety-six, and the number is not a guess: 40-odd extension modules,
 * the program, libc.so, the loader itself, and room for the same again.
 * The cost is `96 * sizeof(Object)` of .bss - about 13 KiB - in a
 * process that has already mapped an interpreter. The search stays a
 * readable loop because it is still linear over objects that are
 * actually loaded, not over the array.
 */
#define MAX_OBJECTS 96

typedef struct {
    u64 base;          /* where this object was loaded */
    /* M95/M96: this object's slice of the static TLS block, as an offset
     * BELOW the thread pointer - so a variable at st_value inside it is
     * at `tp - tls_offset + st_value`, which is what a TPOFF64
     * relocation stores. 0 means the object has no PT_TLS. */
    u64 tls_offset;
    u64 tls_memsz;
    u64 tls_filesz;
    const void *tls_image;
    const char *name;
    const Dyn *dyn;
    const char *strtab;
    const Sym *symtab;
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

/* ---- a bump allocator, because there is no malloc --------------------
 *
 * Only two things are allocated: the names of objects being loaded and
 * nothing else. 4 KiB of .bss is more than enough and costs nothing that
 * is not already in the image. */
/* M99: 8 KiB rather than 1 KiB, for the same reason MAX_OBJECTS grew.
 * A CPython extension module's file name is its own - the arena holds
 * "_multiprocessing.cpython-312-x86_64-lean_os.so" and its forty
 * siblings, which is sixty bytes each rather than the nine "libc.so"
 * costs. */
static char name_arena[8192];
static u64 name_used;

static const char *dl_strdup(const char *s) {
    u64 n = dl_strlen(s) + 1;
    if (name_used + n > sizeof(name_arena)) {
        /* Named for what actually ran out: "too many shared objects" is
         * MAX_OBJECTS's message and sent the first reader of this to the
         * wrong constant. */
        dl_fail("no room left in the shared-object name arena for", s);
    }
    char *p = name_arena + name_used;
    dl_memcpy(p, s, n);
    name_used += n;
    return p;
}

/* ---- step one: relocate this file ------------------------------------
 *
 * Called from _dl_start before anything else, with `base` read out of
 * the auxiliary vector - and note the argument, because it cannot be a
 * global: a global's address is exactly what has not been fixed up yet.
 *
 * Only R_X86_64_RELATIVE, and that is not a simplification: a
 * self-contained ET_DYN with no undefined symbols produces nothing else.
 * If this ever produced a GLOB_DAT it would mean this file had started
 * referring to something outside itself, which is the bug rather than
 * the missing feature.
 */
__attribute__((no_sanitize_address))
static void self_relocate(u64 base, const Dyn *dyn) {
    const Rela *rela = 0;
    u64 relasz = 0;
    for (const Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_RELA) {
            rela = (const Rela *)(base + d->d_val);
        } else if (d->d_tag == DT_RELASZ) {
            relasz = d->d_val;
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

/* ---- symbol lookup ---------------------------------------------------
 *
 * Linear over each object's symbol table, in scope order. Every real
 * dynamic linker uses the GNU hash table here and this does not, which
 * is a decision worth naming: a hash lookup is O(1) against O(n), and n
 * on this machine is the few hundred symbols in libc.so. A linear scan
 * of a few hundred entries, a few hundred times, at program start, is
 * microseconds - and a hash implementation is a second way to be wrong
 * about which definition wins. The day a program links against something
 * with thirty thousand symbols, this is the line that changes, and it
 * will change because a measurement said so.
 */
static u64 lookup(const char *name, const Object *skip, int *found) {
    *found = 0;
    for (int i = 0; i < object_count; i++) {
        const Object *o = &objects[i];
        if (o == skip || !o->symtab || !o->strtab) {
            continue;
        }
        /* The symbol table's length is not in the dynamic section
         * directly; it is bounded by the string table that follows it in
         * every layout a linker produces. Walked until a symbol's name
         * offset would run past the string table, which is the bound
         * that is actually available. */
        for (u32 s = 0; ; s++) {
            const Sym *sym = &o->symtab[s];
            if ((const char *)sym >= o->strtab) {
                break;
            }
            if (sym->st_shndx == SHN_UNDEF || sym->st_name == 0) {
                continue;
            }
            if (!dl_streq(o->strtab + sym->st_name, name)) {
                continue;
            }
            *found = 1;
            return o->base + sym->st_value;
        }
    }
    return 0;
}

/* ---- applying one object's relocations ------------------------------- */

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

        const Sym *sym = &o->symtab[symi];
        const char *name = o->strtab + sym->st_name;

        /* ---- a relocation with no symbol -------------------------------
         *
         * Symbol index 0 is STN_UNDEF and names nothing. For most types
         * that would be a malformed object, but TPOFF64 uses it
         * routinely: a `static __thread` variable is LOCAL, so the
         * linker resolves it at link time and leaves a relocation that
         * says only "this object's TLS block, at this addend". That is
         * the common case for a library with any thread-local of its
         * own - libc.so's errno is exactly one - and it has to be
         * handled where the symbol lookup would otherwise be asked for
         * the empty string.
         *
         * Found the hard way: `errno` became a `static __thread int`
         * behind `__errno_location()` in M97, its TPOFF64 lost its
         * symbol name, and the loader failed with "undefined symbol: "
         * and nothing after the colon. */
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
            if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                value = 0; /* a weak undefined symbol is legitimately 0 */
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
            /* Eager: the JUMP_SLOT is filled now rather than pointing at
             * a resolver stub. See the header for why lazy binding is
             * refused rather than missing. */
            *where = value;
            break;
        case R_X86_64_COPY:
            /* The definition's BYTES are copied into the executable's
             * own .bss, and every other reference then resolves to the
             * copy rather than to the library's. This is how a program
             * that names a library's global variable gets one address
             * for it - and it is why lookup() below must skip the
             * executable when resolving a COPY's source, or it would
             * copy the destination onto itself. */
            {
                int src_found = 0;
                u64 src = lookup(name, &objects[0], &src_found);
                if (!src_found) {
                    dl_fail("undefined symbol for a copy relocation", name);
                }
                dl_memcpy(where, (const void *)src, sym->st_size);
            }
            break;
        case R_X86_64_TPOFF64: {
            /* Thread-local, initial-exec model: the offset of this
             * variable from the thread pointer, which is NEGATIVE
             * because the block sits below it (see M96's tls.c, which
             * draws the picture).
             *
             * Only objects present at startup have a slice of the static
             * block, so this resolves for them and refuses for anything
             * dlopen'ed - a newly loaded object's TLS would have to go
             * somewhere the running threads' blocks do not reach, which
             * is what the general-dynamic model and __tls_get_addr
             * exist for and is written down as not being here. */
            const Object *def = 0;
            for (int k = 0; k < object_count; k++) {
                if (!objects[k].symtab || !objects[k].strtab) {
                    continue;
                }
                for (u32 t = 0; ; t++) {
                    const Sym *cand = &objects[k].symtab[t];
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

/* ---- reading an object's dynamic section ----------------------------- */

static void scan_dynamic(Object *o) {
    u64 relasz = 0, pltrelsz = 0, init_arraysz = 0;
    for (const Dyn *d = o->dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB: o->strtab = (const char *)(o->base + d->d_val); break;
        case DT_SYMTAB: o->symtab = (const Sym *)(o->base + d->d_val); break;
        case DT_RELA:   o->rela = (const Rela *)(o->base + d->d_val); break;
        case DT_RELASZ: relasz = d->d_val; break;
        case DT_JMPREL: o->jmprel = (const Rela *)(o->base + d->d_val); break;
        case DT_PLTRELSZ: pltrelsz = d->d_val; break;
        case DT_INIT:
            /* Only when it is a real address. ld emits DT_INIT with a
             * value of 0 for an object linked without crti.o - there is
             * no .init section, so `_init` is 0 - and adding the base to
             * that gives the object's own ELF header. Calling it
             * executes `\x7fELF` as instructions, which faults at the
             * first byte after the magic and looks like a corrupt
             * library rather than like a missing guard. */
            if (d->d_val != 0) {
                o->init = (void (*)(void))(o->base + d->d_val);
            }
            break;
        case DT_INIT_ARRAY:
            if (d->d_val != 0) {
                o->init_array = (void (**)(void))(o->base + d->d_val);
            }
            break;
        case DT_INIT_ARRAYSZ: init_arraysz = d->d_val; break;
        default: break;
        }
    }
    o->rela_count = relasz / sizeof(Rela);
    o->jmprel_count = pltrelsz / sizeof(Rela);
    o->init_count = init_arraysz / sizeof(void *);
}

/* ---- loading one shared object --------------------------------------- */

static u64 next_lib_base;

static Object *load_object(const char *soname);

/* Set from the environment by _dl_entry, before anything is loaded. A
 * colon-separated list, tried before the built-in directories - which is
 * what makes it useful and is the only thing anybody uses it for: a
 * program under test pointing at a library that is not installed yet. */
static const char *ld_library_path;

/* Finds the file: LD_LIBRARY_PATH first, then /lib, then /usr/lib.
 * Returns an open descriptor, or -1.
 *
 * ---- M99: a name with a slash in it is a pathname --------------------
 *
 * This is what every dynamic linker does and what POSIX says dlopen
 * means, and leaving it out was a real bug rather than a simplification.
 * M95's programs asked for "libdyn.so" and got /lib/libdyn.so, so
 * nothing here ever noticed. CPython's dynload_shlib.c hands dlopen the
 * *full path* of the module it found on sys.path -
 * /usr/lib/python3.12/lib-dynload/_socket.cpython-312-x86_64-lean_os.so
 * - and this function turned that into "/lib//usr/lib/python3.12/..."
 * and then reported "cannot find shared object", naming a file that was
 * sitting on the disk at the name it had been given.
 *
 * A search path is for a SONAME. A pathname is already the answer, and
 * searching for it is how a loader ends up looking everywhere except
 * where it was told.
 */
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
            return -1; /* a path too long to open is not a path we have */
        }
        i64 fd = sys(SYS_open, (long)path, 1 /* OPEN_READ */, 0);
        return fd >= 0 ? (int)fd : -1;
    }

    for (const char *p = ld_library_path; p && *p;) {
        u64 n = 0;
        while (*p && *p != ':' && n < cap - 2) {
            path[n++] = *p++;
        }
        while (*p && *p != ':') {
            p++; /* an element too long to try is skipped, not truncated */
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
            continue; /* the whole name did not fit - a different file */
        }
        i64 fd = sys(SYS_open, (long)path, 1 /* OPEN_READ */, 0);
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
        i64 fd = sys(SYS_open, (long)path, 1 /* OPEN_READ */, 0);
        if (fd >= 0) {
            return (int)fd;
        }
    }
    return -1;
}

static Object *load_object(const char *soname) {
    for (int i = 0; i < object_count; i++) {
        if (objects[i].name && dl_streq(objects[i].name, soname)) {
            return &objects[i]; /* already loaded - the DAG, flattened */
        }
    }
    if (object_count >= MAX_OBJECTS) {
        dl_fail("too many shared objects", soname);
    }

    /* M99: 256 rather than 128. A SONAME is short; the full pathname of
     * a CPython extension module under /usr/lib/python3.12/lib-dynload
     * is 76 characters before anyone nests a virtual environment inside
     * it, and a truncated path is a "cannot find" for a file that is
     * there. */
    char path[256];
    int fd = open_lib(soname, path, sizeof(path));
    if (fd < 0) {
        dl_fail("cannot find shared object", soname);
    }

    /* The header first, to find out how big the mapping has to be. Two
     * reads rather than a stat plus a whole-file map, because the
     * program headers are all that decides the layout and they are in
     * the first page. */
    static u8 hdr[4096];
    i64 got = sys(SYS_read, fd, (long)hdr, sizeof(hdr));
    if (got < (i64)sizeof(Ehdr)) {
        dl_fail("short read on", soname);
    }
    const Ehdr *eh = (const Ehdr *)hdr;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        dl_fail("not an ELF file", soname);
    }
    const Phdr *ph = (const Phdr *)(hdr + eh->e_phoff);

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
    next_lib_base += hi - lo + 0x200000ull; /* a 2 MiB gap, so a stray
                                             * write past one object's
                                             * end lands on nothing */

    /* ---- and here is where M91 pays for itself ----------------------
     *
     * Every segment is mapped from the FILE. A read-only segment - which
     * is text, and is most of a library - is mapped MAP_SHARED, so two
     * processes that load the same library get the same physical frames
     * (kernel/mm/filemap.c holds the one copy). A writable segment is
     * MAP_PRIVATE, because relocations write into it and one process's
     * relocated .data must not be another's.
     *
     * That split is the entire mechanism behind M95's own test: "two
     * different programs running at once share exactly one copy of
     * libc.so's text, and the proof is the PMM's frame count".
     */
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        u64 va = (base + ph[i].p_vaddr) & ~4095ull;
        u64 off = ph[i].p_vaddr & 4095ull;
        u64 len = (ph[i].p_memsz + off + 4095ull) & ~4095ull;
        int writable = (ph[i].p_flags & 2) != 0;
        int prot = 1 /* PROT_READ */;
        if (writable) {
            prot |= 2; /* PROT_WRITE */
        }
        if (ph[i].p_flags & 1) {
            prot |= 4; /* PROT_EXEC */
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
        /* A segment whose memsz exceeds its filesz has .bss at the end,
         * and the bytes past the file must read as zeros. A file-backed
         * mapping gives zeros past the end of the FILE, which is not the
         * same boundary - so the tail inside the last mapped page is
         * cleared by hand. Only for writable segments; a read-only one
         * with a .bss does not exist. */
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
    /* M95: its slice of the static TLS block, assigned now - see
     * assign_tls. Only objects loaded at startup get one; a dlopen'ed
     * object with TLS is refused, which is the honest half of what this
     * linker supports. */
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_TLS) {
            o->tls_memsz = ph[i].p_memsz;
            o->tls_filesz = ph[i].p_filesz;
            o->tls_image = (const void *)(base + ph[i].p_vaddr);
        }
    }
    scan_dynamic(o);

    /* Its own DT_NEEDED, breadth first - which is the ELF search order
     * and is what makes "the first definition wins" mean something a
     * person can predict. */
    for (const Dyn *d = o->dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED) {
            load_object(o->strtab + d->d_val);
        }
    }
    return o;
}

/* ---- the entry point --------------------------------------------------
 *
 * `_dl_entry` receives the same pointer the kernel hands every program:
 * the argument region (proc.h's layout), which holds argc, argv, envp
 * and - M95 - the auxiliary vector after envp's NULL.
 *
 * Returns the address to jump to. The assembly stub in ld-start.S calls
 * this and then jumps, with RDI still pointing at the argument region so
 * that the program's own crt0 sees exactly what it would have seen if
 * there had been no linker at all. That last property is what makes a
 * dynamic program and a static one indistinguishable from crt0 down.
 */
u64 _dl_entry(u64 *args);

u64 _dl_entry(u64 *args) {
    u64 argc = args[0];
    u64 *envp = &args[1 + argc + 1];
    u64 i = 0;
    while (envp[i]) {
        i++;
    }
    u64 *aux = &envp[i + 1];

    /* LD_LIBRARY_PATH, before anything is loaded. Read out of the same
     * envp the program will see - this linker gets the environment the
     * way any program does, from the argument region the kernel built. */
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

    /* Where shared libraries go: the mmap arena, which is where
     * SYS_mmap hands out addresses anyway. Starting at its base rather
     * than letting mmap choose, because MAP_FIXED is what places a
     * segment at a computed offset from an object's base and the base
     * has to be decided first. */
    next_lib_base = 0x000000A000000000ull; /* USER_MMAP_BASE */

    /* The program is object 0 and is the head of the search scope. It is
     * already mapped - the kernel did that - so this only describes it. */
    Object *prog = &objects[object_count++];
    dl_memset(prog, 0, sizeof(*prog));
    prog->name = "";
    const Phdr *ph = (const Phdr *)at_phdr;
    /* The program's own base: for a PIE the kernel placed it at
     * USER_IMAGE_BASE, and p_vaddr values are offsets from there. Found
     * by subtracting the first PT_LOAD's p_vaddr from where the headers
     * actually are, which needs no agreement with the kernel about a
     * constant. */
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

    /* ---- M95: the linker registers ITSELF before anything is loaded --
     *
     * It has to be in `objects` before the DT_NEEDED walk below, not
     * after, and the reason is a bug that took an hour: a program that
     * links against dlopen has `ld-lean.so` among its NEEDED entries, so
     * load_object() would find no such object already present and load a
     * SECOND copy of the linker at a fresh base. The program's dlopen
     * would then resolve to that copy - whose `next_lib_base` global has
     * never been set - and the first mmap it attempted was at address
     * zero.
     *
     * Registered second, after the program: the program is the head of
     * the search scope and must be able to override anything. Ahead of
     * the libraries is harmless because everything in this file is
     * hidden except the four dl* entry points - see the build script's
     * -fvisibility=hidden - so there is nothing here for a library's
     * symbol to collide with. */
    if (at_base != 0 && object_count < MAX_OBJECTS) {
        Object *self = &objects[object_count++];
        dl_memset(self, 0, sizeof(*self));
        self->base = at_base;
        self->name = "ld-lean.so";
        extern const Dyn _DYNAMIC[];
        self->dyn = _DYNAMIC;
        scan_dynamic(self);
        self->relocated = 1; /* it relocated itself before anything else */
    }

    for (const Dyn *d = prog->dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED) {
            load_object(prog->strtab + d->d_val);
        }
    }

    /* ---- M95/M96: the static TLS block -----------------------------
     *
     * Every object loaded at startup that has a PT_TLS gets a slice of
     * one block, and the thread pointer is set to its top - which is
     * the layout M96's tls.c already established for the static case
     * and is the same picture with more than one contributor.
     *
     * Assigned here, after everything is loaded and before anything is
     * relocated, because a TPOFF64 relocation resolves to an offset in
     * this layout and cannot be applied before the layout exists.
     *
     * libc's own __lean_tls_setup sees a thread pointer already set and
     * stands down - see its note. The two mechanisms cannot both run,
     * and which one does is decided by whether there is a linker.
     */
    {
        u64 total = 0;
        for (int k = 0; k < object_count; k++) {
            if (objects[k].tls_memsz == 0) {
                continue;
            }
            total = (total + objects[k].tls_memsz + 15u) & ~15ull;
            objects[k].tls_offset = total; /* distance BELOW the tp */
        }
        if (total > 0) {
            u64 want = (total + 15u + 64u) & ~15ull;
            i64 mem = sys6(SYS_mmap, 0, (long)want, 3 /* RW */,
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
            *(u64 *)tp = tp; /* the self-pointer at %fs:0 */
            if (sys(SYS_arch_prctl, ARCH_SET_FS, (long)tp, 0) != 0) {
                dl_fail("the kernel refused a thread pointer", 0);
            }
        }
    }

    /* Relocate in REVERSE order - dependencies before dependents - so
     * that a library's own globals are in place before anything that
     * refers to them runs an initializer. */
    for (int k = object_count - 1; k >= 0; k--) {
        apply_rela(&objects[k], objects[k].rela, objects[k].rela_count);
        apply_rela(&objects[k], objects[k].jmprel, objects[k].jmprel_count);
        objects[k].relocated = 1;
    }

    /* And the initializers, in the same order and for the same reason. A
     * library's constructors run before the program's.
     *
     * The PROGRAM's own are run too, at the end - and that is not
     * optional here. In a static image libc's __lean_start walks
     * .init_array itself, using symbols the linker script defines; in a
     * dynamic one those symbols live in the executable and libc.so
     * cannot see them, so the job moves here, where the program's
     * DT_INIT_ARRAY is already in hand. Same information, other route. */
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

/* ======================================================================
 * dlopen / dlsym / dlclose / dlerror
 * ======================================================================
 *
 * These live HERE, in the linker, and not in libc - which is where a
 * program looks for them. The reason is that they are the linker's own
 * data structures: `objects`, `next_lib_base`, the search scope. A libc
 * copy would be a second loader.
 *
 * How a program reaches them: they are exported from ld-lean.so as
 * ordinary global symbols, and the program's relocations resolve against
 * it like any other loaded object. That works because the linker IS in
 * the search scope - it is one of the objects, added by _dl_entry.
 *
 * ---- what is and is not supported -----------------------------------
 *
 * RTLD_NOW only, because binding is eager everywhere here (see the file
 * header). RTLD_LAZY is accepted and behaves as RTLD_NOW, which is
 * always a legal answer - a lazy binding that happens early is still
 * correct - and is the one place in this file where accepting a flag
 * and doing something else is not a lie.
 *
 * dlclose does NOT unmap. The object stays loaded and its refcount is
 * not tracked. That is a leak of address space, and it is deliberate
 * rather than unnoticed: unmapping an object whose function pointers a
 * program may still hold turns a bug in that program into a fault with
 * no explanation, and nothing on this machine loads and unloads in a
 * loop. The condition for changing it: a program that dlopens more than
 * MAX_OBJECTS things over its life.
 */
#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_GLOBAL 0x0100
#define RTLD_LOCAL  0x0000

static const char *dl_error_msg;

void *dlopen(const char *file, int flags);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);

__attribute__((visibility("default")))
void *dlopen(const char *file, int flags) {
    (void)flags; /* RTLD_LAZY behaves as RTLD_NOW - see above */
    dl_error_msg = 0;
    if (!file) {
        /* dlopen(NULL) is a handle for the main program, which is
         * object 0 - the one case where the answer is already loaded. */
        return &objects[0];
    }
    if (object_count >= MAX_OBJECTS) {
        dl_error_msg = "too many shared objects loaded";
        return 0;
    }

    /* M99: volatile because they are read after a _dl_setjmp that can
     * return twice, which is the one place C says a local may not be in
     * a register. */
    volatile int before = object_count;
    volatile u64 name_mark = name_used;

    /* dlopen nests: an object's init_array runs inside this call and may
     * dlopen something of its own. There is one recovery buffer, so the
     * inner call saves the outer one and puts it back - otherwise the
     * inner longjmp lands in the outer call's handler, which would
     * abandon an object that had loaded perfectly well, and the inner
     * call's own `return 0` would never happen. */
    volatile int was_recovering = dl_recovering;
    volatile u64 saved_recover[8];
    for (int i = 0; i < 8; i++) {
        saved_recover[i] = dl_recover[i];
    }

    if (_dl_setjmp(dl_recover)) {
        /* Arrived from dl_fail somewhere below. Everything this call
         * added is discarded: the object slots, and the names bump-
         * allocated after the mark, which belong to exactly those slots
         * and nothing older.
         *
         * The MAPPINGS are not discarded, and that is the same deliberate
         * leak dlclose's note describes rather than a new one - a
         * half-relocated object's pages are address space nobody can
         * reach any more, and unmapping them here would mean unmapping
         * on a path where the reason for failing might have been that
         * the object was not what it claimed to be. */
        dl_recovering = was_recovering;
        for (int i = 0; i < 8; i++) {
            dl_recover[i] = saved_recover[i];
        }
        object_count = before;
        name_used = name_mark;
        dl_error_msg = dl_fail_text;
        return 0;
    }
    dl_recovering = 1;

    Object *o = load_object(file);
    if (!o) {
        dl_recovering = was_recovering;
        for (int i = 0; i < 8; i++) {
            dl_recover[i] = saved_recover[i];
        }
        dl_error_msg = "cannot load shared object";
        return 0;
    }
    /* Relocate whatever this load added, dependencies first - the same
     * order and the same reason as the initial load. An object already
     * present is not relocated twice, which is what `relocated` is for:
     * applying a JUMP_SLOT a second time is harmless, and applying a
     * COPY relocation a second time overwrites a variable the program
     * has been using. */
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
    dl_error_msg = 0;
    if (!handle || !name) {
        dl_error_msg = "dlsym: null handle or name";
        return 0;
    }
    Object *o = (Object *)handle;
    if (!o->symtab || !o->strtab) {
        dl_error_msg = "dlsym: that handle has no symbols";
        return 0;
    }
    for (u32 s = 0; ; s++) {
        const Sym *sym = &o->symtab[s];
        if ((const char *)sym >= o->strtab) {
            break;
        }
        if (sym->st_shndx == SHN_UNDEF || sym->st_name == 0) {
            continue;
        }
        if (dl_streq(o->strtab + sym->st_name, name)) {
            return (void *)(o->base + sym->st_value);
        }
    }
    dl_error_msg = "dlsym: no such symbol";
    return 0;
}

__attribute__((visibility("default")))
int dlclose(void *handle) {
    (void)handle;
    dl_error_msg = 0;
    return 0; /* nothing is unmapped - see the note above */
}

__attribute__((visibility("default")))
char *dlerror(void) {
    /* Reading it clears it, which is the contract every dlerror has and
     * is what lets a caller tell "no error since last time" from "no
     * error ever". */
    char *m = (char *)dl_error_msg;
    dl_error_msg = 0;
    return m;
}

/* The self-relocation entry, called from ld-start.S with the base and
 * the address of _DYNAMIC. It is separate from _dl_entry because it must
 * run before any global in this file is readable - see rule 2 in the
 * header. */
void _dl_relocate_self(u64 base, const Dyn *dyn);

void _dl_relocate_self(u64 base, const Dyn *dyn) {
    self_relocate(base, dyn);
}
