/* user_space/bin/exectest.c - M84's fixture
 *
 * fork gave this OS two processes from one. exec is what makes that
 * useful: without it, the only thing a child can be is another copy of
 * its parent, and every "run this program" on the machine has to go
 * through SYS_spawn - which cannot set up a pipe, or a redirect, or a
 * descriptor, between deciding to run something and running it. The two
 * halves apart are the whole point.
 *
 * Modes, because most of what is checked here can only be observed from
 * the far side of an exec or the far side of a death:
 *
 *   (no argument)  the parent; runs every check and exits 0
 *   "child" A B    exec'd by the parent with two fd numbers; reports
 *                  which of them survived
 *   "sig"          dies on SIGSEGV so waitpid can be asked about it
 *   "park"         stays alive so WNOHANG has something to not-wait for
 *   "hello"        prints a marker and exits 7, for the execvp check
 *
 * Exit codes for the parent:
 *   0  everything worked
 *   2  fork or exec failed outright
 *   3  a descriptor marked close-on-exec survived the exec
 *   4  a descriptor NOT marked close-on-exec did not survive
 *   5  waitpid reported a normal exit wrongly
 *   6  waitpid could not tell a signal death from an exit
 *   7  WNOHANG blocked, or reported a live child as finished
 *   8  execvp did not find a program on PATH
 *   9  exec did not preserve the process's identity
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "syscall_wrappers.h" /* sys_getpid, sys_exit - see forktest.c */

#define SELF "/bin/exectest"

/* Exit codes the modes below use to report back through waitpid. */
#define CHILD_OK        0
#define CHILD_SURVIVED  3
#define CHILD_MISSING   4

static int mode_child(int cloexec_fd, int plain_fd) {
    /* A closed descriptor has no flags to report, so F_GETFD is how a
     * program asks "is this still mine" without needing to know what is
     * behind it. */
    if (fcntl(cloexec_fd, F_GETFD) >= 0) {
        return CHILD_SURVIVED;
    }
    if (fcntl(plain_fd, F_GETFD) < 0) {
        return CHILD_MISSING;
    }
    /* And the surviving one must still work, not merely still exist. */
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
        *nowhere = 1; /* SIGSEGV, and it must not be mistaken for exit(139) */
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "park") == 0) {
        /* Long enough that the parent's WNOHANG check certainly finds a
         * live child, and no longer: every yield here is boot time, and
         * the parent asks once, immediately. */
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
        /* Handed the pid it had *before* the exec. Same task, same pid -
         * that is what makes this an exec and not a spawn. */
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

    /* ---- the parent -------------------------------------------------- */

    /* Two descriptors on the same file, one asked to close on exec at the
     * moment it was opened and one not. Asking at open time rather than
     * with a later fcntl is the race-free way and the reason O_CLOEXEC
     * exists at all - though the fcntl path is checked too, below. */
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
    /* The flag reads back as set, which is the check that O_CLOEXEC is a
     * bit the kernel stored rather than a bit it ignored. */
    if (fcntl(cloexec_fd, F_GETFD) != FD_CLOEXEC || fcntl(plain_fd, F_GETFD) != 0) {
        return 3;
    }
    /* And the same flag set the other way, after the fact. */
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
        sys_exit(2); /* only reached if execv failed */
    }

    int status = 0;
    if (waitpid(kid, &status, 0) != kid) {
        return 5;
    }
    if (!WIFEXITED(status)) {
        return 5;
    }
    if (WEXITSTATUS(status) != 0) {
        return WEXITSTATUS(status); /* 3 or 4 - the child named which */
    }
    close(cloexec_fd);
    close(plain_fd);

    /* ---- a death by signal, told apart from an exit ------------------- */
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
    /* The whole reason waitpid exists here: SYS_wait would report 139 for
     * this and 139 for a program that called exit(139), and there is no
     * way to tell them apart from the number alone. */
    if (WIFEXITED(status) || !WIFSIGNALED(status) || WTERMSIG(status) != 11) {
        printf("exectest: a SIGSEGV death read back as status %d\n", status);
        return 6;
    }

    /* ---- WNOHANG ------------------------------------------------------ */
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
        return 7; /* a child that is plainly alive was reported finished */
    }
    if (waitpid(pk, &status, 0) != pk || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 7;
    }

    /* ---- execvp finds a program on PATH ------------------------------- */
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

    /* ---- and the identity an exec must NOT change --------------------
     *
     * Checked last because it is the claim that distinguishes exec from
     * spawn: same pid, same parent. A child that exec'd and came back
     * with a different pid would have been a spawn wearing exec's name. */
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
