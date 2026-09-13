#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "caps.h"
#include "syscall_wrappers.h"

#define FAIL(code) do { return (code); } while (0)

union cmsg_one {
    struct cmsghdr align;
    char buf[CMSG_SPACE(sizeof(int))];
};

static int send_file_descriptor(int sock, int fd, const char *payload) {
    struct iovec iov;
    iov.iov_base = (void *)payload;
    iov.iov_len = strlen(payload);
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
    return (int)sendmsg(sock, &msg, 0);
}

static int receive_file_descriptor(int sock, char *out, size_t outlen, int *msgflags) {
    struct iovec iov;
    iov.iov_base = out;
    iov.iov_len = outlen;
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
    if (msgflags) {
        *msgflags = msg.msg_flags;
    }
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (!cm || cm->cmsg_len < CMSG_LEN(sizeof(int)) || cm->cmsg_type != SCM_RIGHTS) {
        return -1;
    }
    int fd = -1;
    memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    return fd;
}

static int test_pair(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL(2);
    }
    char buf[32];
    if (write(sv[0], "ping", 4) != 4) {
        FAIL(3);
    }
    if (read(sv[1], buf, sizeof(buf)) != 4 || memcmp(buf, "ping", 4) != 0) {
        FAIL(3);
    }
    if (write(sv[1], "pong", 4) != 4 || read(sv[0], buf, sizeof(buf)) != 4 ||
        memcmp(buf, "pong", 4) != 0) {
        FAIL(3);
    }
    struct stat st;
    if (fstat(sv[0], &st) != 0 || !S_ISSOCK(st.st_mode)) {
        FAIL(3);
    }
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int test_seqpacket(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        FAIL(2);
    }
    if (write(sv[0], "aa", 2) != 2 || write(sv[0], "bbb", 3) != 3) {
        FAIL(4);
    }
    char buf[32];
    if (read(sv[1], buf, sizeof(buf)) != 2 || memcmp(buf, "aa", 2) != 0) {
        FAIL(4);
    }
    if (read(sv[1], buf, sizeof(buf)) != 3 || memcmp(buf, "bbb", 3) != 0) {
        FAIL(4);
    }
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int test_pass_descriptors(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        FAIL(8);
    }
    const char *path = "/tmp/m118-offset";
    int wf = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (wf < 0 || write(wf, "0123456789", 10) != 10) {
        FAIL(9);
    }
    close(wf);
    int rf = open(path, O_RDONLY, 0);
    char two[2];
    if (rf < 0 || read(rf, two, 2) != 2) {
        FAIL(9);
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL(2);
    }
    pid_t kid = fork();
    if (kid < 0) {
        FAIL(5);
    }
    if (kid == 0) {
        close(sv[0]);
        close(pipefd[0]);
        close(pipefd[1]);
        close(rf);
        char payload[16];
        int got_pipe = receive_file_descriptor(sv[1], payload, sizeof(payload), NULL);
        if (got_pipe < 0) {
            sys_exit(7);
        }
        if (write(got_pipe, "from-the-child", 14) != 14) {
            sys_exit(8);
        }
        close(got_pipe);
        int got_file = receive_file_descriptor(sv[1], payload, sizeof(payload), NULL);
        if (got_file < 0) {
            sys_exit(7);
        }
        char rest[16];
        memset(rest, 0, sizeof(rest));
        long n = read(got_file, rest, sizeof(rest));
        if (n != 8 || memcmp(rest, "23456789", 8) != 0) {
            sys_exit(9);
        }
        close(got_file);
        sys_exit(0);
    }
    close(sv[1]);
    if (send_file_descriptor(sv[0], pipefd[1], "pipe") < 0) {
        FAIL(6);
    }
    close(pipefd[1]);
    if (send_file_descriptor(sv[0], rf, "file") < 0) {
        FAIL(6);
    }
    close(rf);

    char heard[32];
    memset(heard, 0, sizeof(heard));
    long n = read(pipefd[0], heard, sizeof(heard));
    if (n != 14 || memcmp(heard, "from-the-child", 14) != 0) {
        FAIL(8);
    }
    close(pipefd[0]);
    long rc = sys_wait(kid);
    close(sv[0]);
    unlink(path);
    if (rc != 0) {
        FAIL((int)(rc > 0 ? rc : 7));
    }
    return 0;
}

static int test_pass_a_socket(void) {
    int sv[2], inner[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 ||
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, inner) != 0) {
        FAIL(2);
    }
    pid_t kid = fork();
    if (kid < 0) {
        FAIL(5);
    }
    if (kid == 0) {
        close(sv[0]);
        close(inner[0]);
        close(inner[1]);
        char payload[16];
        int chan = receive_file_descriptor(sv[1], payload, sizeof(payload), NULL);
        if (chan < 0) {
            sys_exit(10);
        }
        if (write(chan, "channel-works", 13) != 13) {
            sys_exit(10);
        }
        close(chan);
        sys_exit(0);
    }
    close(sv[1]);
    if (send_file_descriptor(sv[0], inner[1], "chan") < 0) {
        FAIL(6);
    }
    close(inner[1]);
    char buf[32];
    memset(buf, 0, sizeof(buf));
    if (read(inner[0], buf, sizeof(buf)) != 13 ||
        memcmp(buf, "channel-works", 13) != 0) {
        FAIL(10);
    }
    long rc = sys_wait(kid);
    close(inner[0]);
    close(sv[0]);
    return rc == 0 ? 0 : (int)(rc > 0 ? rc : 10);
}

static int test_named(const char *name, int namelen, int failcode) {
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        FAIL(failcode);
    }
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    memcpy(un.sun_path, name, (size_t)namelen);
    socklen_t alen = (socklen_t)((size_t)(((struct sockaddr_un *)0)->sun_path) + namelen);
    if (bind(srv, (struct sockaddr *)&un, alen) != 0 || listen(srv, 4) != 0) {
        FAIL(failcode);
    }
    pid_t kid = fork();
    if (kid < 0) {
        FAIL(5);
    }
    if (kid == 0) {
        close(srv);
        int cli = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cli < 0) {
            sys_exit(failcode);
        }
        if (connect(cli, (struct sockaddr *)&un, alen) != 0) {
            sys_exit(failcode);
        }
        if (write(cli, "dialled", 7) != 7) {
            sys_exit(failcode);
        }
        close(cli);
        sys_exit(0);
    }
    int conn = accept(srv, NULL, NULL);
    if (conn < 0) {
        FAIL(failcode);
    }
    char buf[16];
    memset(buf, 0, sizeof(buf));
    if (read(conn, buf, sizeof(buf)) != 7 || memcmp(buf, "dialled", 7) != 0) {
        FAIL(failcode);
    }
    if (read(conn, buf, sizeof(buf)) != 0) {
        FAIL(15);
    }
    long rc = sys_wait(kid);
    close(conn);
    close(srv);
    return rc == 0 ? 0 : failcode;
}

static int test_blocking_and_shutdown(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL(2);
    }
    pid_t kid = fork();
    if (kid < 0) {
        FAIL(5);
    }
    if (kid == 0) {
        close(sv[0]);
        char buf[16];
        long n = read(sv[1], buf, sizeof(buf));
        if (n != 5 || memcmp(buf, "woken", 5) != 0) {
            sys_exit(14);
        }
        if (shutdown(sv[1], SHUT_WR) != 0) {
            sys_exit(13);
        }
        n = read(sv[1], buf, sizeof(buf));
        if (n != 4) {
            sys_exit(13);
        }
        sys_exit(0);
    }
    close(sv[1]);
    for (int i = 0; i < 50; i++) {
        sys_yield();
    }
    if (write(sv[0], "woken", 5) != 5) {
        FAIL(14);
    }
    char buf[16];
    long n = read(sv[0], buf, sizeof(buf));
    if (n != 0) {
        FAIL(13);
    }
    if (write(sv[0], "last", 4) != 4) {
        FAIL(13);
    }
    long rc = sys_wait(kid);
    close(sv[0]);
    return rc == 0 ? 0 : (int)(rc > 0 ? rc : 13);
}

static int test_ctrunc(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL(2);
    }
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        FAIL(8);
    }
    if (send_file_descriptor(sv[0], pipefd[1], "x") < 0) {
        FAIL(6);
    }
    close(pipefd[0]);
    close(pipefd[1]);
    char buf[8];
    struct iovec iov = {buf, sizeof(buf)};
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (recvmsg(sv[1], &msg, 0) != 1) {
        FAIL(17);
    }
    if (!(msg.msg_flags & MSG_CTRUNC)) {
        FAIL(17);
    }
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int test_exhaustion(void) {
    int fds[64][2];
    int taken = 0;
    while (taken < 64 && socketpair(AF_UNIX, SOCK_STREAM, 0, fds[taken]) == 0) {
        taken++;
    }
    if (taken == 0) {
        FAIL(16);
    }
    for (int i = 0; i < taken; i++) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL(16);
    }
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int test_no_capability_needed(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL(2);
    }
    pid_t kid = fork();
    if (kid < 0) {
        FAIL(5);
    }
    if (kid == 0) {
        close(sv[0]);
        if (sys_dropcaps(0) != 0) {
            sys_exit(18);
        }
        if (socket(AF_INET, SOCK_STREAM, 0) >= 0) {
            sys_exit(18);
        }
        int inner[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, inner) != 0) {
            sys_exit(18);
        }
        if (write(inner[0], "free", 4) != 4) {
            sys_exit(18);
        }
        char buf[8];
        if (read(inner[1], buf, sizeof(buf)) != 4) {
            sys_exit(18);
        }
        char payload[8];
        int got = receive_file_descriptor(sv[1], payload, sizeof(payload), NULL);
        if (got < 0) {
            sys_exit(18);
        }
        if (write(got, "sandboxed", 9) != 9) {
            sys_exit(18);
        }
        sys_exit(0);
    }
    close(sv[1]);
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        FAIL(8);
    }
    if (send_file_descriptor(sv[0], pipefd[1], "p") < 0) {
        FAIL(6);
    }
    close(pipefd[1]);
    char buf[16];
    memset(buf, 0, sizeof(buf));
    long n = read(pipefd[0], buf, sizeof(buf));
    close(pipefd[0]);
    long rc = sys_wait(kid);
    close(sv[0]);
    if (n != 9 || memcmp(buf, "sandboxed", 9) != 0) {
        FAIL(18);
    }
    return rc == 0 ? 0 : (int)(rc > 0 ? rc : 18);
}

int main(void) {
    int rc;
    const char abstract[] = {0, 'm', '1', '1', '8'};
    if ((rc = test_pair()) != 0) return rc;
    if ((rc = test_seqpacket()) != 0) return rc;
    if ((rc = test_pass_descriptors()) != 0) return rc;
    if ((rc = test_pass_a_socket()) != 0) return rc;
    if ((rc = test_named("/tmp/m118.sock", 14, 11)) != 0) return rc;
    if ((rc = test_named(abstract, 5, 12)) != 0) return rc;
    if ((rc = test_blocking_and_shutdown()) != 0) return rc;
    if ((rc = test_ctrunc()) != 0) return rc;
    if ((rc = test_exhaustion()) != 0) return rc;
    if ((rc = test_no_capability_needed()) != 0) return rc;
    printf("unixtest: all ten sections passed\n");
    return 0;
}
