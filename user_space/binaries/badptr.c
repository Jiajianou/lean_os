#include "syscall.h"
#include "syscall_wrappers.h"

#define PAGE_SIZE 4096ULL

#define P_POINTER 0xF0F0F0F0F0F0F001ULL
#define P_LENGTH 0xF0F0F0F0F0F0F002ULL

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

static const struct {
    const char *name;
    long nr;
    unsigned long long a[3];
} ROWS[] = {
    {"SYS_write",         SYS_write,         {1, P_POINTER, P_LENGTH}},
    {"SYS_read",          SYS_read,          {0, P_POINTER, P_LENGTH}},
    {"SYS_spawn(path)",   SYS_spawn,         {P_POINTER, 0, 0}},
    {"SYS_readfile(name)",SYS_readfile,      {P_POINTER, 0, 0}},
    {"SYS_writefile(name)",SYS_writefile,    {P_POINTER, 0, 0}},
    {"SYS_listdir(path)", SYS_listdir,       {P_POINTER, 0, 0}},
    {"SYS_listdir(buf)",  SYS_listdir,       {(unsigned long long)(unsigned long)"/", P_POINTER, P_LENGTH}},
    {"SYS_mkdir(path)",   SYS_mkdir,         {P_POINTER, 0, 0}},
    {"SYS_pipe",          SYS_pipe,          {P_POINTER, 0, 0}},
    {"SYS_fb_info",       SYS_framebuffer_info,       {P_POINTER, 0, 0}},
    {"SYS_mouse_read",    SYS_mouse_read,    {P_POINTER, 0, 0}},
    {"SYS_pipe_open(name)",SYS_pipe_open,    {P_POINTER, 0, 0}},
    {"SYS_kbd_read",      SYS_keyboard_read,      {P_POINTER, 0, 0}},
    {"SYS_taskinfo",      SYS_taskinfo,      {P_POINTER, 4, 0}},
    {"SYS_clipboard_set", SYS_clipboard_set, {P_POINTER, P_LENGTH, 0}},
    {"SYS_clipboard_get", SYS_clipboard_get, {P_POINTER, P_LENGTH, 0}},
};
#define ROW_COUNT ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

static void put(const char *s) {
    long n = 0;
    while (s[n]) {
        n++;
    }
    sys_write(1, s, (unsigned long)n);
}

static void put_number(long v) {
    char buffer[24];
    int i = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) {
        buffer[i++] = '0';
    }
    while (u > 0) {
        buffer[i++] = (char)('0' + (u % 10));
        u /= 10;
    }
    if (neg) {
        buffer[i++] = '-';
    }
    char out[24];
    int o = 0;
    while (i > 0) {
        out[o++] = buffer[--i];
    }
    out[o] = '\0';
    put(out);
}

int main(int argc, char **argv) {
    (void)argc;
    unsigned long long here = (unsigned long long)(void *)&main;
    unsigned long long region_base = here & ~((1ULL << 39) - 1ULL);

    const unsigned long long ARGUMENT_REGION_PAGES = 2;
    unsigned long long argument_page =
        (unsigned long long)(void *)argv & ~(unsigned long long)(PAGE_SIZE - 1);
    unsigned long long argument_region_last_page = argument_page + (ARGUMENT_REGION_PAGES - 1) * PAGE_SIZE;

    unsigned long long bad_pointer[BAD_COUNT];
    unsigned long long bad_length[BAD_COUNT];
    bad_pointer[BAD_NULL]     = 0;                              bad_length[BAD_NULL]     = 8;
    bad_pointer[BAD_KERNEL]   = 0x100000ULL;                    bad_length[BAD_KERNEL]   = 8;
    bad_pointer[BAD_BELOW]    = region_base - 1ULL;             bad_length[BAD_BELOW]    = 8;
    bad_pointer[BAD_UNMAPPED] = region_base + (1ULL << 38);     bad_length[BAD_UNMAPPED] = 8;
    bad_pointer[BAD_STRADDLE] = argument_region_last_page + PAGE_SIZE - 1ULL;
                                                            bad_length[BAD_STRADDLE] = 2;
    bad_pointer[BAD_OVERFLOW] = argument_page;                       bad_length[BAD_OVERFLOW] = ~0ULL;

    int failures = 0;
    for (int r = 0; r < ROW_COUNT; r++) {
        for (int k = 0; k < BAD_COUNT; k++) {
            int has_length = 0;
            for (int i = 0; i < 3; i++) {
                if (ROWS[r].a[i] == P_LENGTH) {
                    has_length = 1;
                }
            }
            if (!has_length && (k == BAD_STRADDLE || k == BAD_OVERFLOW)) {
                continue;
            }

            unsigned long long a[3];
            for (int i = 0; i < 3; i++) {
                if (ROWS[r].a[i] == P_POINTER) {
                    a[i] = bad_pointer[k];
                } else if (ROWS[r].a[i] == P_LENGTH) {
                    a[i] = bad_length[k];
                } else {
                    a[i] = ROWS[r].a[i];
                }
            }
            long ret = sys_raw(ROWS[r].nr, (long)a[0], (long)a[1], (long)a[2]);
            if (ret >= 0) {
                failures++;
                put("[badptr] ");
                put(ROWS[r].name);
                put(" accepted ");
                put(BAD_NAME[k]);
                put(" and returned ");
                put_number(ret);
                put("\n");
            }
        }
    }

    put("[badptr] ");
    put_number(ROW_COUNT);
    put(" syscalls x every applicable bad-pointer shape: ");
    put_number(failures);
    put(" accepted.\n");
    return failures;
}
