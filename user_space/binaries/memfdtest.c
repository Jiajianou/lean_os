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

static int test_basics(void) {
    int fd = memfd_create("basics", 0);
    if (fd < 0) {
        FAIL(2);
    }
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
    for (int i = 0; i < REGION; i++) {
        if (p[i] != 0) {
            FAIL(5);
        }
    }
    memcpy(p, "first-writer", 12);
    p[REGION - 1] = 'Z';

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

    close(fd);
    if (memcmp(p, "First-writer", 12) != 0 || p[REGION - 1] != 'Z') {
        FAIL(6);
    }
    p[1] = 'i';
    if (p[1] != 'i') {
        FAIL(6);
    }
    munmap(p, REGION);
    return 0;
}

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
        int got = recv_fd(sv[1]);
        if (got < 0) {
            sys_exit(9);
        }
        struct stat cst;
        if (fstat(got, &cst) != 0 || cst.st_size != REGION) {
            sys_exit(9);
        }
        char *theirs = (char *)mmap(0, REGION, PROT_READ | PROT_WRITE, MAP_SHARED, got, 0);
        if (theirs == MAP_FAILED) {
            sys_exit(9);
        }
        if (memcmp(theirs, "from-the-parent", 15) != 0) {
            sys_exit(10);
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
    close(fd);
    long rc = sys_wait(kid);
    if (rc != 0) {
        FAIL((int)(rc > 0 ? rc : 10));
    }
    if (memcmp(mine + 100, "from-the-child", 14) != 0 || mine[REGION - 2] != 'C') {
        FAIL(10);
    }
    munmap(mine, REGION);
    close(sv[0]);
    return 0;
}

static int test_refusals(void) {
    int fd = memfd_create("refusals", 0);
    if (fd < 0 || ftruncate(fd, 2 * 4096) != 0) {
        FAIL(2);
    }
    if (ftruncate(fd, 4096) == 0) {
        FAIL(12);
    }
    if (ftruncate(fd, (off_t)64 * 1024 * 1024) == 0) {
        FAIL(12);
    }
    if (mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0) != MAP_FAILED) {
        FAIL(12);
    }
    if (mmap(0, 8 * 4096, PROT_READ, MAP_SHARED, fd, 0) != MAP_FAILED) {
        FAIL(14);
    }
    if (mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 4 * 4096) != MAP_FAILED) {
        FAIL(14);
    }
    if (mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 100) != MAP_FAILED) {
        FAIL(14);
    }
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

static int test_seals(void) {
    int plain = memfd_create("plain", 0);
    if (plain < 0 || ftruncate(plain, 4096) != 0) {
        FAIL(2);
    }
    if (memfd_add_seals(plain, F_SEAL_WRITE) == 0) {
        FAIL(16);
    }
    if (memfd_seals(plain) != F_SEAL_SEAL) {
        FAIL(16);
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
    if (mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) != MAP_FAILED) {
        FAIL(15);
    }
    char *r = (char *)mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (r == MAP_FAILED || memcmp(r, "sealed-content", 14) != 0) {
        FAIL(15);
    }
    munmap(r, 4096);
    if (ftruncate(fd, 8192) == 0) {
        FAIL(15);
    }
    if (memfd_add_seals(fd, F_SEAL_SEAL) != 0) {
        FAIL(16);
    }
    if (memfd_add_seals(fd, F_SEAL_SHRINK) == 0) {
        FAIL(16);
    }
    close(fd);
    return 0;
}

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
        if (sys_dropcaps(0) != 0) {
            sys_exit(17);
        }
        int got = recv_fd(sv[1]);
        if (got < 0) {
            sys_exit(17);
        }
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

static int test_exhaustion(void) {
    int fds[64];
    int n = 0;
    while (n < 64) {
        int fd = memfd_create("many", 0);
        if (fd < 0) {
            break;
        }
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
