#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "syscall_wrappers.h"

#define SELF "/bin/exectest"

#define CHILD_OK        0
#define CHILD_SURVIVED  3
#define CHILD_MISSING   4

static int mode_child(int cloexec_fd, int plain_fd) {
    if (fcntl(cloexec_fd, F_GETFD) >= 0) {
        return CHILD_SURVIVED;
    }
    if (fcntl(plain_fd, F_GETFD) < 0) {
        return CHILD_MISSING;
    }
    char buf[8];
    memset(buf, 0, sizeof(buf));
    if (read(plain_fd, buf, 4) != 4 || memcmp(buf, "keep", 4) != 0) {
        return CHILD_MISSING;
    }
    return CHILD_OK;
}

static int digits(char *out, int v) {
    int n = 0;
    if (v == 0) {
        out[n++] = '0';
    }
    char tmp[12];
    int t = 0;
    while (v > 0) {
        tmp[t++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (t > 0) {
        out[n++] = tmp[--t];
    }
    out[n] = '\0';
    return n;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "sig") == 0) {
        volatile int *nowhere = (volatile int *)0;
        *nowhere = 1;
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "park") == 0) {
        for (int i = 0; i < 400; i++) {
            sys_yield();
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "hello") == 0) {
        printf("exectest: reached by execvp\n");
        return 7;
    }
    if (argc > 2 && strcmp(argv[1], "samepid") == 0) {
        int want = 0;
        for (const char *c = argv[2]; *c; c++) { want = want * 10 + (*c - '0'); }
        return (int)sys_getpid() == want ? 0 : 9;
    }
    if (argc > 3 && strcmp(argv[1], "child") == 0) {
        int a = 0, b = 0;
        for (const char *c = argv[2]; *c; c++) { a = a * 10 + (*c - '0'); }
        for (const char *c = argv[3]; *c; c++) { b = b * 10 + (*c - '0'); }
        return mode_child(a, b);
    }

    static const char body[] = "keepgoing";
    int w = open("/tmp/m84fd", O_WRONLY | O_CREAT | O_TRUNC);
    if (w < 0 || write(w, body, sizeof(body)) != (long)sizeof(body)) {
        return 2;
    }
    close(w);

    int cloexec_fd = open("/tmp/m84fd", O_RDONLY | O_CLOEXEC);
    int plain_fd = open("/tmp/m84fd", O_RDONLY);
    if (cloexec_fd < 0 || plain_fd < 0) {
        return 2;
    }
    if (fcntl(cloexec_fd, F_GETFD) != FD_CLOEXEC || fcntl(plain_fd, F_GETFD) != 0) {
        return 3;
    }
    if (fcntl(plain_fd, F_SETFD, 0) != 0 || fcntl(plain_fd, F_GETFD) != 0) {
        return 3;
    }

    char a_str[12], b_str[12];
    digits(a_str, cloexec_fd);
    digits(b_str, plain_fd);

    pid_t kid = fork();
    if (kid < 0) {
        return 2;
    }
    if (kid == 0) {
        char *const av[] = {(char *)SELF, (char *)"child", a_str, b_str, 0};
        execv(SELF, av);
        sys_exit(2);
    }

    int status = 0;
    if (waitpid(kid, &status, 0) != kid) {
        return 5;
    }
    if (!WIFEXITED(status)) {
        return 5;
    }
    if (WEXITSTATUS(status) != 0) {
        return WEXITSTATUS(status);
    }
    close(cloexec_fd);
    close(plain_fd);

    pid_t sk = fork();
    if (sk < 0) {
        return 2;
    }
    if (sk == 0) {
        char *const av[] = {(char *)SELF, (char *)"sig", 0};
        execv(SELF, av);
        sys_exit(2);
    }
    status = 0;
    if (waitpid(sk, &status, 0) != sk) {
        return 6;
    }
    if (WIFEXITED(status) || !WIFSIGNALED(status) || WTERMSIG(status) != 11) {
        printf("exectest: a SIGSEGV death read back as status %d\n", status);
        return 6;
    }

    pid_t pk = fork();
    if (pk < 0) {
        return 2;
    }
    if (pk == 0) {
        char *const av[] = {(char *)SELF, (char *)"park", 0};
        execv(SELF, av);
        sys_exit(2);
    }
    status = 0;
    if (waitpid(pk, &status, WNOHANG) != 0) {
        return 7;
    }
    if (waitpid(pk, &status, 0) != pk || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 7;
    }

    pid_t vk = fork();
    if (vk < 0) {
        return 2;
    }
    if (vk == 0) {
        char *const av[] = {(char *)"exectest", (char *)"hello", 0};
        execvp("exectest", av);
        sys_exit(8);
    }
    status = 0;
    if (waitpid(vk, &status, 0) != vk || !WIFEXITED(status) || WEXITSTATUS(status) != 7) {
        return 8;
    }

    pid_t ik = fork();
    if (ik < 0) {
        return 2;
    }
    if (ik == 0) {
        char kid_pid[12];
        digits(kid_pid, (int)sys_getpid());
        char *const av[] = {(char *)SELF, (char *)"samepid", kid_pid, 0};
        execv(SELF, av);
        sys_exit(2);
    }
    status = 0;
    if (waitpid(ik, &status, 0) != ik || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 9;
    }

    printf("exectest: all checks passed\n");
    return 0;
}
