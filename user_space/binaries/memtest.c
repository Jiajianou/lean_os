#include "paths.h"
#include "malloc.h"
#include "string_utilities.h"
#include "syscall_wrappers.h"

#define SHM_TEST_SIZE 4096
#define PATTERN_BYTE  0x5A

static void write_str(const char *s) {
    sys_write(1, s, strlen(s));
}

static void fail(const char *msg) {
    write_str("[memtest] FAIL: ");
    write_str(msg);
    write_str("\n");
    sys_exit(1);
}

static void itoa_dec(long v, char *buf) {
    if (v == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    char tmp[24];
    int i = 0;
    unsigned long u = (unsigned long)v;
    while (u > 0) {
        tmp[i++] = (char)('0' + (u % 10));
        u /= 10;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = 0;
}

static long atoi_dec(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static void malloc_self_test(void) {
    char *a = (char *)malloc(100);
    char *b = (char *)malloc(200);
    char *c = (char *)malloc(50);
    if (!a || !b || !c) {
        fail("malloc returned NULL");
    }
    memset(a, 0xAA, 100);
    memset(b, 0xBB, 200);
    memset(c, 0xCC, 50);
    for (int i = 0; i < 100; i++) {
        if ((unsigned char)a[i] != 0xAA) {
            fail("block a corrupted");
        }
    }
    for (int i = 0; i < 200; i++) {
        if ((unsigned char)b[i] != 0xBB) {
            fail("block b corrupted");
        }
    }
    for (int i = 0; i < 50; i++) {
        if ((unsigned char)c[i] != 0xCC) {
            fail("block c corrupted");
        }
    }

    free(b);
    char *d = (char *)malloc(150);
    if (!d) {
        fail("malloc after free returned NULL");
    }
    memset(d, 0xDD, 150);
    for (int i = 0; i < 100; i++) {
        if ((unsigned char)a[i] != 0xAA) {
            fail("block a corrupted after free/reuse");
        }
    }
    for (int i = 0; i < 50; i++) {
        if ((unsigned char)c[i] != 0xCC) {
            fail("block c corrupted after free/reuse");
        }
    }

    write_str("[memtest] malloc/free self-test passed.\n");
}

static int run_as_creator(void) {
    long id = sys_shm_create(SHM_TEST_SIZE);
    if (id < 0) {
        fail("sys_shm_create failed");
    }
    long vaddr = sys_shm_map(id);
    if (vaddr < 0) {
        fail("sys_shm_map failed (creator)");
    }
    memset((void *)vaddr, PATTERN_BYTE, SHM_TEST_SIZE);

    char id_str[24];
    itoa_dec(id, id_str);
    long child_pid = sys_spawn(PATH_BIN_DIR "memtest", id_str);
    if (child_pid < 0) {
        fail("sys_spawn(memtest, <id>) failed");
    }
    long child_status = sys_wait(child_pid);
    if (child_status != 0) {
        fail("child (shm reader) reported failure");
    }

    write_str("[memtest] cross-process shm self-test passed (writer + reader agree).\n");
    return 0;
}

static int run_as_shm_reader(const char *arg) {
    long id = atoi_dec(arg);
    long vaddr = sys_shm_map(id);
    if (vaddr < 0) {
        fail("sys_shm_map failed (reader)");
    }
    const unsigned char *buf = (const unsigned char *)vaddr;
    for (int i = 0; i < SHM_TEST_SIZE; i++) {
        if (buf[i] != PATTERN_BYTE) {
            fail("shm pattern mismatch - not really shared");
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *arg = argc > 1 ? argv[1] : "";
    if (arg[0] == '\0') {
        malloc_self_test();
        return run_as_creator();
    }
    return run_as_shm_reader(arg);
}
