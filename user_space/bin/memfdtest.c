/* user_space/bin/memfdtest.c - M120's fixture
 *
 * Shared memory a descriptor names, on the machine - and with M118 and
 * M119 in place, this is the first test in this project that performs the
 * whole shape a multi-process browser engine is built out of:
 *
 *   a socketpair, a child, a memfd created and sized by the parent, the
 *   descriptor sent over the channel, the child mapping it and writing
 *   through it, and the parent reading what the child wrote - with the
 *   child holding no capabilities at all.
 *
 * tests/test_memfd.c grades the object off the machine, where a frame can
 * be handed out dirty on purpose and a slot's generation can be rolled
 * over in four lines. What only this can have is a second address space:
 * "the same frames" is a claim about two page tables, and a machine is the
 * only place it can be made.
 *
 * Exit codes, so a failure names itself:
 *   0  everything worked
 *   2  memfd_create failed
 *   3  ftruncate did not size it, or fstat did not report the size
 *   4  mmap of a memfd failed
 *   5  what was written through the mapping did not read back
 *   6  the mapping did not survive the descriptor being closed
 *   7  fork failed
 *   8  sending the descriptor failed
 *   9  the child could not map what it was sent
 *  10  the parent did not see what the child wrote - not the same frames
 *  11  a second mapping in one process did not alias the first
 *  12  a shrink, an oversize or a MAP_PRIVATE was not refused
 *  13  read(2) or write(2) on a memfd was not refused
 *  14  a mapping past the end of the object was not refused
 *  15  F_SEAL_WRITE did not stop a writable mapping
 *  16  a seal was added after F_SEAL_SEAL, or to a descriptor without
 *      MFD_ALLOW_SEALING
 *  17  a child holding no capabilities could not use shared memory
 *  18  exhaustion did not refuse, or did not recover
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "caps.h"
#include "syscall_wrappers.h"

#define FAIL(code) do { return (code); } while (0)
#define REGION (64 * 1024)

/* Sending and receiving one descriptor - the same shape M118's fixture
 * uses, repeated here rather than shared because a test that depends on
 * another test's helper is a test that fails for two reasons. */
union cmsg_one {
    struct cmsghdr align;
    char buf[CMSG_SPACE(sizeof(int))];
};

static int send_fd(int sock, int fd) {
    char byte = 'h';
    struct iovec iov = {&byte, 1};
    union cmsg_one c;
    memset(&c, 0, sizeof(c));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = c.buf;
    msg.msg_controllen = sizeof(c.buf);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

static int recv_fd(int sock) {
    char byte = 0;
    struct iovec iov = {&byte, 1};
    union cmsg_one c;
    memset(&c, 0, sizeof(c));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = c.buf;
    msg.msg_controllen = sizeof(c.buf);
    if (recvmsg(sock, &msg, 0) < 0) {
        return -1;
    }
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (!cm || cm->cmsg_type != SCM_RIGHTS) {
        return -1;
    }
    int fd = -1;
    memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    return fd;
}

/* ---- 1: create, size, map, write, read back --------------------------- */
static int test_basics(void) {
    int fd = memfd_create("basics", 0);
    if (fd < 0) {
        FAIL(2);
    }
    /* Empty until somebody sizes it, which is also what stops a mapping
     * of an unsized object from succeeding. */
    if (mmap(0, REGION, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) != MAP_FAILED) {
        FAIL(4);
    }
    if (ftruncate(fd, REGION) != 0) {
        FAIL(3);
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size != REGION) {
        FAIL(3);
    }
    char *p = (char *)mmap(0, REGION, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        FAIL(4);
    }
    /* Zeroed, which is the claim tests/test_memfd.c makes against a
     * poisoned allocator and this one makes against the real machine. */
    for (int i = 0; i < REGION; i++) {
        if (p[i] != 0) {
            FAIL(5);
        }
    }
    memcpy(p, "first-writer", 12);
    p[REGION - 1] = 'Z';

    /* A second mapping of the same object in the same process: the same
     * memory, not a copy. */
    char *q = (char *)mmap(0, REGION, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (q == MAP_FAILED || q == p) {
        FAIL(11);
    }
    if (memcmp(q, "first-writer", 12) != 0 || q[REGION - 1] != 'Z') {
        FAIL(11);
    }
    q[0] = 'F';
    if (p[0] != 'F') {
        FAIL(11);
    }
    munmap(q, REGION);

    /* **The lifetime rule.** Close the descriptor and keep using the
     * mapping: that is POSIX, it is what every program that shares memory
     * does, and it is the reason the kernel's object is refcounted by
     * mappings as well as by descriptors. */
    close(fd);
    if (memcmp(p, "First-writer", 12) != 0 || p[REGION - 1] != 'Z') {
        FAIL(6);
    }
    p[1] = 'i'; /* and still writable */
    if (p[1] != 'i') {
        FAIL(6);
    }
    munmap(p, REGION);
    return 0;
}

/* ---- 2: the whole engine shape, in two processes ---------------------- */
static int test_across_a_channel(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL(8);
    }
    int fd = memfd_create("channel", 0);
    if (fd < 0 || ftruncate(fd, REGION) != 0) {
        FAIL(2);
    }
    char *mine = (char *)mmap(0, REGION, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mine == MAP_FAILED) {
        FAIL(4);
    }
    memcpy(mine, "from-the-parent", 15);

    pid_t kid = fork();
    if (kid < 0) {
        FAIL(7);
    }
    if (kid == 0) {
        close(sv[0]);
        close(fd);
        munmap(mine, REGION);
        /* The child holds nothing from before: the memory it is about to
         * write arrives entirely as a descriptor over a socket. */
        int got = recv_fd(sv[1]);
        if (got < 0) {
            sys_exit(9);
        }
        struct stat cst;
        if (fstat(got, &cst) != 0 || cst.st_size != REGION) {
            sys_exit(9); /* the size, which a receiver has to be able to ask */
        }
        char *theirs = (char *)mmap(0, REGION, PROT_READ | PROT_WRITE, MAP_SHARED, got, 0);
        if (theirs == MAP_FAILED) {
            sys_exit(9);
        }
        if (memcmp(theirs, "from-the-parent", 15) != 0) {
            sys_exit(10); /* not the same frames */
        }
        memcpy(theirs + 100, "from-the-child", 14);
        theirs[REGION - 2] = 'C';
        close(got);
        munmap(theirs, REGION);
        sys_exit(0);
    }
    close(sv[1]);
    if (send_fd(sv[0], fd) != 0) {
        FAIL(8);
    }
    close(fd); /* the parent's own descriptor, closed immediately */
    long rc = sys_wait(kid);
    if (rc != 0) {
        FAIL((int)(rc > 0 ? rc : 10));
    }
    /* What the child wrote, through a mapping the parent made before the
     * child existed, of an object whose descriptor neither of them still
     * holds. */
    if (memcmp(mine + 100, "from-the-child", 14) != 0 || mine[REGION - 2] != 'C') {
        FAIL(10);
    }
    munmap(mine, REGION);
    close(sv[0]);
    return 0;
}

/* ---- 3: what is refused ---------------------------------------------- */
static int test_refusals(void) {
    int fd = memfd_create("refusals", 0);
    if (fd < 0 || ftruncate(fd, 2 * 4096) != 0) {
        FAIL(2);
    }
    /* A shrink: refused, because a mapping whose frames were freed under
     * it would read whatever they became next. */
    if (ftruncate(fd, 4096) == 0) {
        FAIL(12);
    }
    /* Past the cap. */
    if (ftruncate(fd, (off_t)64 * 1024 * 1024) == 0) {
        FAIL(12);
    }
    /* MAP_PRIVATE of a memfd: refused rather than silently shared or
     * silently copied. */
    if (mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0) != MAP_FAILED) {
        FAIL(12);
    }
    /* A mapping longer than the object. Linux lets this exist and answers
     * the touch with SIGBUS; this kernel has no such machinery, so it is
     * refused at the call - which the caller finds out about immediately
     * rather than three functions later. */
    if (mmap(0, 8 * 4096, PROT_READ, MAP_SHARED, fd, 0) != MAP_FAILED) {
        FAIL(14);
    }
    /* An offset past the end, and an unaligned one. */
    if (mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 4 * 4096) != MAP_FAILED) {
        FAIL(14);
    }
    if (mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 100) != MAP_FAILED) {
        FAIL(14);
    }
    /* read(2) and write(2): refused by name. Linux allows both; nothing
     * that uses shared memory does it, and a second path to the same bytes
     * with different rules is worth less than the refusal. */
    char byte = 0;
    if (read(fd, &byte, 1) >= 0) {
        FAIL(13);
    }
    if (write(fd, "x", 1) >= 0) {
        FAIL(13);
    }
    close(fd);
    return 0;
}

/* ---- 4: seals, which are what make read-only sharing possible -------- */
static int test_seals(void) {
    /* Without MFD_ALLOW_SEALING a descriptor can never be sealed, which is
     * how a program that hands over memory stops the receiver from sealing
     * it further. */
    int plain = memfd_create("plain", 0);
    if (plain < 0 || ftruncate(plain, 4096) != 0) {
        FAIL(2);
    }
    if (memfd_add_seals(plain, F_SEAL_WRITE) == 0) {
        FAIL(16);
    }
    if (memfd_seals(plain) != F_SEAL_SEAL) {
        FAIL(16); /* sealed against sealing, which is what "not allowed" means here */
    }
    close(plain);

    int fd = memfd_create("sealable", MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, 4096) != 0) {
        FAIL(2);
    }
    if (memfd_seals(fd) != 0) {
        FAIL(16);
    }
    char *w = (char *)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (w == MAP_FAILED) {
        FAIL(4);
    }
    memcpy(w, "sealed-content", 14);
    munmap(w, 4096);

    if (memfd_add_seals(fd, F_SEAL_WRITE | F_SEAL_GROW) != 0) {
        FAIL(15);
    }
    if (memfd_seals(fd) != (F_SEAL_WRITE | F_SEAL_GROW)) {
        FAIL(15);
    }
    /* **The point of the whole seal mechanism**: a writable mapping is now
     * impossible, so a receiver handed this descriptor can verify rather
     * than trust. */
    if (mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) != MAP_FAILED) {
        FAIL(15);
    }
    char *r = (char *)mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (r == MAP_FAILED || memcmp(r, "sealed-content", 14) != 0) {
        FAIL(15);
    }
    munmap(r, 4096);
    if (ftruncate(fd, 8192) == 0) {
        FAIL(15); /* F_SEAL_GROW */
    }
    /* And nothing more can be promised once sealing is sealed. */
    if (memfd_add_seals(fd, F_SEAL_SEAL) != 0) {
        FAIL(16);
    }
    if (memfd_add_seals(fd, F_SEAL_SHRINK) == 0) {
        FAIL(16);
    }
    close(fd);
    return 0;
}

/* ---- 5: the renderer shape - a child that holds nothing -------------- */
static int test_no_capabilities(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL(8);
    }
    int fd = memfd_create("sandboxed", 0);
    if (fd < 0 || ftruncate(fd, 4096) != 0) {
        FAIL(2);
    }
    char *mine = (char *)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mine == MAP_FAILED) {
        FAIL(4);
    }
    pid_t kid = fork();
    if (kid < 0) {
        FAIL(7);
    }
    if (kid == 0) {
        close(sv[0]);
        close(fd);
        munmap(mine, 4096);
        /* Nothing at all: no filesystem write, no network, no framebuffer.
         * This is what a browser's renderer is, and the three milestones
         * M118-M120 exist so that it can still be talked to, woken, and
         * handed a buffer to paint into. */
        if (sys_dropcaps(0) != 0) {
            sys_exit(17);
        }
        int got = recv_fd(sv[1]);
        if (got < 0) {
            sys_exit(17);
        }
        /* And it can size and map memory of its own, which needs no
         * capability because the authority is the descriptor. */
        int own = memfd_create("renderers-own", 0);
        if (own < 0 || ftruncate(own, 4096) != 0) {
            sys_exit(17);
        }
        char *scratch = (char *)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, own, 0);
        if (scratch == MAP_FAILED) {
            sys_exit(17);
        }
        memcpy(scratch, "painted", 7);
        char *shared = (char *)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, got, 0);
        if (shared == MAP_FAILED) {
            sys_exit(17);
        }
        memcpy(shared, scratch, 7);
        munmap(scratch, 4096);
        munmap(shared, 4096);
        close(own);
        close(got);
        sys_exit(0);
    }
    close(sv[1]);
    if (send_fd(sv[0], fd) != 0) {
        FAIL(8);
    }
    close(fd);
    long rc = sys_wait(kid);
    close(sv[0]);
    if (rc != 0) {
        FAIL((int)(rc > 0 ? rc : 17));
    }
    if (memcmp(mine, "painted", 7) != 0) {
        FAIL(17);
    }
    munmap(mine, 4096);
    return 0;
}

/* ---- 6: exhaustion, and the frames coming back ----------------------- */
static int test_exhaustion(void) {
    int fds[64];
    int n = 0;
    while (n < 64) {
        int fd = memfd_create("many", 0);
        if (fd < 0) {
            break;
        }
        /* Sized, so that running out of objects also means having claimed
         * and released real frames - the kernel counts both either side of
         * this program. */
        if (ftruncate(fd, 4096) != 0) {
            close(fd);
            break;
        }
        fds[n++] = fd;
    }
    if (n == 0) {
        FAIL(18);
    }
    for (int i = 0; i < n; i++) {
        close(fds[i]);
    }
    int again = memfd_create("after", 0);
    if (again < 0 || ftruncate(again, 4096) != 0) {
        FAIL(18);
    }
    close(again);
    return 0;
}

int main(void) {
    int rc;
    if ((rc = test_basics()) != 0) return rc;
    if ((rc = test_across_a_channel()) != 0) return rc;
    if ((rc = test_refusals()) != 0) return rc;
    if ((rc = test_seals()) != 0) return rc;
    if ((rc = test_no_capabilities()) != 0) return rc;
    if ((rc = test_exhaustion()) != 0) return rc;
    printf("memfdtest: all six sections passed\n");
    return 0;
}
