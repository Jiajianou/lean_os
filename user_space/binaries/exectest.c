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

/* M167. Both of these are about what a child of exec KEEPS, and neither
   could be seen without exec'ing: a descriptor that should still be open,
   and a path that has to be followed rather than read. */
#define SELF_LINK       "/proc/self/exe"
#define REACHED_BY_LINK 21
#define DUP_LOST        22
#define DUP_WRONG_FILE  23

static int mode_child(int cloexec_file_descriptor, int plain_file_descriptor) {
    if (fcntl(cloexec_file_descriptor, F_GETFD) >= 0) {
        return CHILD_SURVIVED;
    }
    if (fcntl(plain_file_descriptor, F_GETFD) < 0) {
        return CHILD_MISSING;
    }
    char buffer[8];
    memset(buffer, 0, sizeof(buffer));
    if (read(plain_file_descriptor, buffer, 4) != 4 || memcmp(buffer, "keep", 4) != 0) {
        return CHILD_MISSING;
    }
    return CHILD_OK;
}

static int digits(char *out, int v) {
    int n = 0;
    if (v == 0) {
        out[n++] = '0';
    }
    char temporary[12];
    int t = 0;
    while (v > 0) {
        temporary[t++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (t > 0) {
        out[n++] = temporary[--t];
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
    if (argc > 1 && strcmp(argv[1], "vialink") == 0) {
        /* Reached by exec'ing /proc/self/exe, which is how every process
           Chromium starts is started. Until M167 execve read the link
           instead of following it, and got a path where an ELF header
           should be. */
        return REACHED_BY_LINK;
    }
    if (argc > 2 && strcmp(argv[1], "dupkeep") == 0) {
        int fd = 0;
        for (const char *c = argv[2]; *c; c++) { fd = fd * 10 + (*c - '0'); }
        if (fcntl(fd, F_GETFD) < 0) {
            return DUP_LOST;
        }
        char buffer[8];
        memset(buffer, 0, sizeof(buffer));
        if (read(fd, buffer, 4) != 4 || memcmp(buffer, "keep", 4) != 0) {
            return DUP_WRONG_FILE;
        }
        return CHILD_OK;
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

    /* execve(2) follows symbolic links. /proc/self/exe is the only link this
       machine has and it is the one that matters: content::ChildProcessHost
       asks for the path of the running executable and re-runs it, so a
       renderer, a utility process and the network service are all started
       this way. */
    {
        pid_t linked = fork();
        if (linked < 0) {
            return 2;
        }
        if (linked == 0) {
            char *const av[] = {(char *)SELF, (char *)"vialink", 0};
            execv(SELF_LINK, av);
            sys_exit(2);
        }
        int status = 0;
        if (waitpid(linked, &status, 0) != linked || !WIFEXITED(status)) {
            return 5;
        }
        if (WEXITSTATUS(status) != REACHED_BY_LINK) {
            printf("exectest: exec of %s gave %d, wanted %d\n", SELF_LINK,
                   WEXITSTATUS(status), REACHED_BY_LINK);
            return 10;
        }
    }

    static const char body[] = "keepgoing";
    int w = open("/tmp/m84fd", O_WRONLY | O_CREAT | O_TRUNC);
    if (w < 0 || write(w, body, sizeof(body)) != (long)sizeof(body)) {
        return 2;
    }
    close(w);

    int cloexec_file_descriptor = open("/tmp/m84fd", O_RDONLY | O_CLOEXEC);
    int plain_file_descriptor = open("/tmp/m84fd", O_RDONLY);
    if (cloexec_file_descriptor < 0 || plain_file_descriptor < 0) {
        return 2;
    }
    if (fcntl(cloexec_file_descriptor, F_GETFD) != FD_CLOEXEC || fcntl(plain_file_descriptor, F_GETFD) != 0) {
        return 3;
    }
    if (fcntl(plain_file_descriptor, F_SETFD, 0) != 0 || fcntl(plain_file_descriptor, F_GETFD) != 0) {
        return 3;
    }

    char a_string[12], b_string[12];
    digits(a_string, cloexec_file_descriptor);
    digits(b_string, plain_file_descriptor);

    pid_t kid = fork();
    if (kid < 0) {
        return 2;
    }
    if (kid == 0) {
        char *const av[] = {(char *)SELF, (char *)"child", a_string, b_string, 0};
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
    /* POSIX: "The FD_CLOEXEC flag associated with the new file descriptor
       shall be cleared." This kernel copied the whole descriptor across, so
       a dup2 of a close-on-exec descriptor produced another close-on-exec
       one - and the copy vanished at the exec that was about to use it.
       Chromium's launcher does this for every child it starts, which is why
       every child died holding nothing. Both halves are checked, because the
       flag being clear is not the same claim as the descriptor surviving. */
    {
        int duplicate = dup2(cloexec_file_descriptor, 20);
        if (duplicate != 20) {
            return 11;
        }
        if (fcntl(duplicate, F_GETFD) != 0) {
            printf("exectest: dup2 of a close-on-exec descriptor came back "
                   "close-on-exec\n");
            return 12;
        }
        char number[12];
        digits(number, duplicate);
        pid_t keeper = fork();
        if (keeper < 0) {
            return 2;
        }
        if (keeper == 0) {
            char *const av[] = {(char *)SELF, (char *)"dupkeep", number, 0};
            execv(SELF, av);
            sys_exit(2);
        }
        int status = 0;
        if (waitpid(keeper, &status, 0) != keeper || !WIFEXITED(status)) {
            return 5;
        }
        if (WEXITSTATUS(status) != CHILD_OK) {
            printf("exectest: a dup2'd descriptor did not survive the exec "
                   "(child exited %d)\n", WEXITSTATUS(status));
            return 13;
        }
        close(duplicate);
    }

    close(cloexec_file_descriptor);
    close(plain_file_descriptor);

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
