#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "capabilities.h"
#include "syscall_wrappers.h"

#define FAIL(code) do { return (code); } while (0)
#define REGION (64 * 1024)

union cmsg_one {
    struct cmsghdr align;
    char buffer[CMSG_SPACE(sizeof(int))];
};

static int send_file_descriptor(int sock, int fd) {
    char byte = 'h';
    struct iovec iov = {&byte, 1};
    union cmsg_one c;
    memset(&c, 0, sizeof(c));
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = c.buffer;
    message.msg_controllen = sizeof(c.buffer);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&message);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    return sendmsg(sock, &message, 0) < 0 ? -1 : 0;
}

static int receive_file_descriptor(int sock) {
    char byte = 0;
    struct iovec iov = {&byte, 1};
    union cmsg_one c;
    memset(&c, 0, sizeof(c));
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = c.buffer;
    message.msg_controllen = sizeof(c.buffer);
    if (recvmsg(sock, &message, 0) < 0) {
        return -1;
    }
    struct cmsghdr *cm = CMSG_FIRSTHDR(&message);
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
        int got = receive_file_descriptor(sv[1]);
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
    if (send_file_descriptor(sv[0], fd) != 0) {
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
        int got = receive_file_descriptor(sv[1]);
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
    if (send_file_descriptor(sv[0], fd) != 0) {
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
    int file_descriptors[64];
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
        file_descriptors[n++] = fd;
    }
    if (n == 0) {
        FAIL(18);
    }
    for (int i = 0; i < n; i++) {
        close(file_descriptors[i]);
    }
    int again = memfd_create("after", 0);
    if (again < 0 || ftruncate(again, 4096) != 0) {
        FAIL(18);
    }
    close(again);
    return 0;
}

/* A memfd has no name but /proc/<pid>/fd/<n>, and reopening it there is how
   a process gets a SECOND descriptor for the same pages with less access -
   the descriptor it can hand to somebody it does not trust. The checks that
   matter are the refusals: a read-only descriptor that can still be mapped
   writable, or truncated, or reopened for writing, is a claim rather than a
   boundary. */
static int test_reopen(void) {
    int fd = memfd_create("reopen", MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, 4096) != 0) {
        FAIL(19);
    }
    char *writable = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (writable == MAP_FAILED) {
        FAIL(19);
    }
    memcpy(writable, "reopened", 8);

    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    int readonly = open(path, O_RDONLY);
    if (readonly < 0) {
        FAIL(20);
    }

    if ((fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDWR) {
        FAIL(20);
    }
    if ((fcntl(readonly, F_GETFL) & O_ACCMODE) != O_RDONLY) {
        FAIL(20);
    }

    char *seen = mmap(0, 4096, PROT_READ, MAP_SHARED, readonly, 0);
    if (seen == MAP_FAILED || memcmp(seen, "reopened", 8) != 0) {
        FAIL(21);
    }
    /* The same pages, not a copy: what the writable mapping does next is
       visible through the read-only one. */
    memcpy(writable, "changed!", 8);
    if (memcmp(seen, "changed!", 8) != 0) {
        FAIL(21);
    }

    if (mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, readonly, 0) !=
        MAP_FAILED) {
        FAIL(22);
    }
    if (ftruncate(readonly, 8192) == 0) {
        FAIL(22);
    }

    /* And the rights cannot grow back. */
    char readonly_path[64];
    snprintf(readonly_path, sizeof(readonly_path), "/proc/self/fd/%d", readonly);
    if (open(readonly_path, O_RDWR) >= 0) {
        FAIL(23);
    }
    int again = open(readonly_path, O_RDONLY);
    if (again < 0) {
        FAIL(23);
    }
    close(again);

    munmap(seen, 4096);
    munmap(writable, 4096);
    close(readonly);
    close(fd);

    /* A descriptor that is not open has no name under procfs, and another
       process's table is not something a path lookup reaches into. */
    if (open(path, O_RDONLY) >= 0) {
        FAIL(24);
    }
    if (open("/proc/1/fd/0", O_RDONLY) >= 0) {
        FAIL(24);
    }
    if (open("/proc/self/fd/", O_RDONLY) >= 0 ||
        open("/proc/self/fd/x", O_RDONLY) >= 0) {
        FAIL(24);
    }

    /* A pipe end has no other name either, and the copy is the same pipe. */
    int ends[2];
    if (pipe(ends) != 0) {
        FAIL(25);
    }
    snprintf(path, sizeof(path), "/proc/self/fd/%d", ends[0]);
    int copy = open(path, O_RDONLY);
    if (copy < 0) {
        FAIL(25);
    }
    char got[6] = {0};
    if (write(ends[1], "hello", 5) != 5 || read(copy, got, 5) != 5 ||
        memcmp(got, "hello", 5) != 0) {
        FAIL(25);
    }
    close(copy);
    close(ends[0]);
    close(ends[1]);

    /* A file on the disk DOES have a name of its own, so reopening it
       through procfs gives a new file description with its own offset
       rather than a second reference to this one. */
    int file = open("/tmp/memfd-reopen", O_RDWR | O_CREAT | O_TRUNC);
    if (file < 0 || write(file, "0123456789", 10) != 10) {
        FAIL(26);
    }
    close(file);
    file = open("/tmp/memfd-reopen", O_RDONLY);
    char four[4] = {0};
    if (file < 0 || read(file, four, 4) != 4 || memcmp(four, "0123", 4) != 0) {
        FAIL(26);
    }
    snprintf(path, sizeof(path), "/proc/self/fd/%d", file);
    int fresh = open(path, O_RDONLY);
    if (fresh < 0 || read(fresh, four, 4) != 4 || memcmp(four, "0123", 4) != 0) {
        FAIL(26);
    }
    close(fresh);
    close(file);
    unlink("/tmp/memfd-reopen");
    return 0;
}

/* M187: a MAP_SHARED page stays shared across a fork from a process with
   more than one thread. fork released every shared page first and then copied
   the page tables, and a sibling thread on another core that WROTE to a shared
   page in between faulted it back in writable - so the copy made it
   copy-on-write, and the parent's next write went to a private copy that the
   memfd, and every other mapping of it, never saw. Chromium's browser forks
   for every child it launches while its threads write histograms into shared
   memory, and what it said was "corrupt".

   So a thread writes to the page as fast as it can while this one forks, and
   after every fork this one writes through one mapping and reads through a
   second mapping of the same memfd. A write that went to a private copy is a
   value the second mapping does not have. On one core the window cannot open
   and this passes either way; the four-core run is where it grades. */
static volatile int keep_touching;
static char *volatile touched;

static void *toucher(void *unused) {
    (void)unused;
    unsigned char n = 0;
    while (keep_touching) {
        touched[64] = (char)n++;
    }
    return (void *)0;
}

static int test_shared_across_a_threaded_fork(void) {
    int fd = memfd_create("fork-shared", 0);
    if (fd < 0 || ftruncate(fd, REGION) != 0) {
        FAIL(80);
    }
    char *writer = (char *)mmap(0, REGION, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char *reader = (char *)mmap(0, REGION, PROT_READ, MAP_SHARED, fd, 0);
    if (writer == MAP_FAILED || reader == MAP_FAILED) {
        FAIL(81);
    }
    touched = writer;
    keep_touching = 1;
    pthread_t thread;
    if (pthread_create(&thread, (pthread_attr_t *)0, toucher, (void *)0) != 0) {
        FAIL(82);
    }
    int code = 0;
    for (int round = 1; round <= 200 && !code; round++) {
        pid_t child = fork();
        if (child < 0) {
            code = 83;
            break;
        }
        if (child == 0) {
            _exit(0);
        }
        int status = 0;
        waitpid(child, &status, 0);
        writer[0] = (char)round;
        if (reader[0] != (char)round) {
            printf("memfdtest: after fork %d a write through one mapping did "
                   "not reach the other - the page went copy-on-write\n", round);
            code = 84;
        }
    }
    keep_touching = 0;
    pthread_join(thread, (void **)0);
    munmap(writer, REGION);
    munmap(reader, REGION);
    close(fd);
    return code;
}

int main(void) {
    int rc;
    if ((rc = test_basics()) != 0) return rc;
    if ((rc = test_across_a_channel()) != 0) return rc;
    if ((rc = test_refusals()) != 0) return rc;
    if ((rc = test_seals()) != 0) return rc;
    if ((rc = test_no_capabilities()) != 0) return rc;
    if ((rc = test_exhaustion()) != 0) return rc;
    if ((rc = test_reopen()) != 0) return rc;
    if ((rc = test_shared_across_a_threaded_fork()) != 0) return rc;
    printf("memfdtest: all eight sections passed\n");
    return 0;
}
