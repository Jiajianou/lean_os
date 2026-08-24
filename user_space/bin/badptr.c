/* user_space/bin/badptr.c
 *
 * M52's garbage-argument matrix, and the reason it is a program rather
 * than a block in kernel.c: the checks it tests only apply to ring-3
 * callers. syscall.c's user_range_ok returns immediately for a task
 * sharing the kernel's address space - a kernel thread passing kernel
 * pointers is doing exactly what it is supposed to - so every row of a
 * matrix run from kernel_main would take that early return and prove
 * nothing at all.
 *
 * The matrix is a table rather than prose, which is the point: adding a
 * syscall that takes a pointer without adding a row here is a visible
 * omission, and every row is exercised against every shape of bad
 * pointer rather than against whichever one somebody thought of.
 *
 * The five shapes, all derived here rather than taken from kernel
 * headers - kernel/proc/proc.h is not on a user program's include path,
 * and a program that could only be written by reading the kernel's own
 * constants would not be testing the interface:
 *
 *   BAD_NULL       0
 *   BAD_KERNEL     an address in the kernel's identity map
 *   BAD_BELOW      one byte below the private region's base
 *   BAD_UNMAPPED   inside the private region, nothing mapped there
 *   BAD_STRADDLE   a real, mapped byte whose range runs one past the end
 *                  of that page into an unmapped one
 *   BAD_OVERFLOW   a real pointer with a length that overflows the range
 *
 * Exit code is the number of rows that were *not* refused, so the [m52]
 * self-test can assert 0 and the log says which ones if not.
 */
#include "syscall.h" /* system_api/include/syscall.h - SYS_* numbers, used raw here */
#include "syscall_wrappers.h"

#define PAGE_SIZE 4096ULL

/* Substituted per row - see run_row. Chosen so they cannot collide with
 * a real argument value any row wants to pass. */
#define P_PTR 0xF0F0F0F0F0F0F001ULL /* this slot takes the bad pointer under test */
#define P_LEN 0xF0F0F0F0F0F0F002ULL /* this slot takes the length that goes with it */

typedef enum {
    BAD_NULL = 0,
    BAD_KERNEL,
    BAD_BELOW,
    BAD_UNMAPPED,
    BAD_STRADDLE,
    BAD_OVERFLOW,
    BAD_COUNT,
} bad_kind_t;

static const char *const BAD_NAME[BAD_COUNT] = {
    "null",
    "a kernel address",
    "one byte below the user region",
    "an unmapped address inside the user region",
    "a mapped byte straddling into an unmapped page",
    "a valid pointer with a length that overflows",
};

/* Every syscall that dereferences at least one pointer the caller chose.
 * `a[]` is the raw three-argument tuple, with P_PTR marking the pointer
 * slot and P_LEN (where there is one) the length that goes with it.
 *
 * Deliberately absent, and worth naming so their absence reads as a
 * decision rather than an oversight: SYS_shm_free's `vaddr` is an
 * address but never dereferenced (it is unmapped, and its own range
 * check is tested by the kernel-side [m52] block), and SYS_fb_map /
 * SYS_shm_map / SYS_sbrk *return* addresses without taking any. */
static const struct {
    const char *name;
    long nr;
    unsigned long long a[3];
} ROWS[] = {
    {"SYS_write",         SYS_write,         {1, P_PTR, P_LEN}},
    {"SYS_read",          SYS_read,          {0, P_PTR, P_LEN}},
    {"SYS_spawn(path)",   SYS_spawn,         {P_PTR, 0, 0}},
    {"SYS_readfile(name)",SYS_readfile,      {P_PTR, 0, 0}},
    {"SYS_writefile(name)",SYS_writefile,    {P_PTR, 0, 0}},
    {"SYS_listdir(path)", SYS_listdir,       {P_PTR, 0, 0}},
    {"SYS_listdir(buf)",  SYS_listdir,       {(unsigned long long)(unsigned long)"/", P_PTR, P_LEN}},
    {"SYS_mkdir(path)",   SYS_mkdir,         {P_PTR, 0, 0}},
    {"SYS_pipe",          SYS_pipe,          {P_PTR, 0, 0}},
    {"SYS_fb_info",       SYS_fb_info,       {P_PTR, 0, 0}},
    {"SYS_mouse_read",    SYS_mouse_read,    {P_PTR, 0, 0}},
    {"SYS_pipe_open(name)",SYS_pipe_open,    {P_PTR, 0, 0}},
    {"SYS_kbd_read",      SYS_kbd_read,      {P_PTR, 0, 0}},
    {"SYS_taskinfo",      SYS_taskinfo,      {P_PTR, 4, 0}},
    {"SYS_clipboard_set", SYS_clipboard_set, {P_PTR, P_LEN, 0}},
    {"SYS_clipboard_get", SYS_clipboard_get, {P_PTR, P_LEN, 0}},
};
#define ROW_COUNT ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

static void put(const char *s) {
    long n = 0;
    while (s[n]) {
        n++;
    }
    sys_write(1, s, (unsigned long)n);
}

static void put_num(long v) {
    char buf[24];
    int i = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) {
        buf[i++] = '0';
    }
    while (u > 0) {
        buf[i++] = (char)('0' + (u % 10));
        u /= 10;
    }
    if (neg) {
        buf[i++] = '-';
    }
    char out[24];
    int o = 0;
    while (i > 0) {
        out[o++] = buf[--i];
    }
    out[o] = '\0';
    put(out);
}

int main(const char *arg) {
    /* The private region's base is PML4 entry 1, so it is this program's
     * own load address with the low 39 bits (everything one PML4 entry
     * covers) masked off - derived from where the linker actually put
     * this code rather than restated from a kernel header. */
    unsigned long long here = (unsigned long long)(void *)&main;
    unsigned long long region_base = here & ~((1ULL << 39) - 1ULL);

    /* The argument page is exactly one page and the page above it is
     * never mapped (kernel/proc/proc.c maps one frame at USER_ARG_ADDR
     * and the stack grows *below* it), which is what makes a read that
     * starts on its last byte cross into nothing. */
    unsigned long long arg_page = (unsigned long long)(void *)arg;

    unsigned long long bad_ptr[BAD_COUNT];
    unsigned long long bad_len[BAD_COUNT];
    bad_ptr[BAD_NULL]     = 0;                              bad_len[BAD_NULL]     = 8;
    bad_ptr[BAD_KERNEL]   = 0x100000ULL;                    bad_len[BAD_KERNEL]   = 8;
    bad_ptr[BAD_BELOW]    = region_base - 1ULL;             bad_len[BAD_BELOW]    = 8;
    bad_ptr[BAD_UNMAPPED] = region_base + (1ULL << 38);     bad_len[BAD_UNMAPPED] = 8;
    bad_ptr[BAD_STRADDLE] = arg_page + PAGE_SIZE - 1ULL;    bad_len[BAD_STRADDLE] = 2;
    bad_ptr[BAD_OVERFLOW] = arg_page;                       bad_len[BAD_OVERFLOW] = ~0ULL;

    int failures = 0;
    for (int r = 0; r < ROW_COUNT; r++) {
        for (int k = 0; k < BAD_COUNT; k++) {
            /* BAD_STRADDLE and BAD_OVERFLOW only mean anything for a row
             * whose length the caller chooses. A row with a fixed-size
             * payload (a struct, an fd pair) has no length to overflow,
             * and its own straddle case is BAD_UNMAPPED already. */
            int has_len = 0;
            for (int i = 0; i < 3; i++) {
                if (ROWS[r].a[i] == P_LEN) {
                    has_len = 1;
                }
            }
            if (!has_len && (k == BAD_STRADDLE || k == BAD_OVERFLOW)) {
                continue;
            }

            unsigned long long a[3];
            for (int i = 0; i < 3; i++) {
                if (ROWS[r].a[i] == P_PTR) {
                    a[i] = bad_ptr[k];
                } else if (ROWS[r].a[i] == P_LEN) {
                    a[i] = bad_len[k];
                } else {
                    a[i] = ROWS[r].a[i];
                }
            }
            long ret = sys_raw(ROWS[r].nr, (long)a[0], (long)a[1], (long)a[2]);
            /* Every one of these must be an error return. Negative is
             * the contract for all fourteen; the one that matters more
             * is that we are still here to read it at all, since before
             * M52 half of these would have faulted the kernel. */
            if (ret >= 0) {
                failures++;
                put("[badptr] ");
                put(ROWS[r].name);
                put(" accepted ");
                put(BAD_NAME[k]);
                put(" and returned ");
                put_num(ret);
                put("\n");
            }
        }
    }

    put("[badptr] ");
    put_num(ROW_COUNT);
    put(" syscalls x every applicable bad-pointer shape: ");
    put_num(failures);
    put(" accepted.\n");
    return failures;
}
