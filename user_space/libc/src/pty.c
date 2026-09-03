/* user_space/libc/src/pty.c - M85, second attempt
 *
 * The five calls a program makes to get a terminal for a child, over
 * `/dev/ptmx` and `/dev/pts/<n>`.
 *
 * <pty.h> has said since M89 that all of these are refusals and named
 * the milestone that would build them. This is that milestone finishing
 * its own last bullet: the kernel side is kernel/dev/pty.c, and
 * everything here is the standard shape written over it.
 *
 * ---- grantpt and unlockpt, and why they do nothing ---------------------
 *
 * On a system with more than one user, `grantpt` changes the slave's
 * owner to the caller and `unlockpt` clears a bit that stops anybody
 * opening the slave before the master has finished setting it up. This
 * machine has one principal (M65) and its ptys have no lock bit, so both
 * of those are checks with nothing behind them.
 *
 * They return 0 rather than -1 anyway, and that is the opposite call to
 * the one <pty.h> made when it refused openpty - so it is worth saying
 * why. openpty returning a descriptor would have been a *lie about a
 * capability*: the program gets an fd that is not a terminal and fails
 * somewhere else. grantpt returning 0 is a true statement about this
 * machine: the permissions the caller asked for are the permissions the
 * slave already has. The difference is whether the caller is misled, and
 * a program that gets 0 here and then opens the slave successfully has
 * been told the truth.
 */
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int posix_openpt(int flags) {
    /* O_NOCTTY is the usual second flag and it is already the behaviour:
     * opening a terminal on this machine never makes it a controlling
     * one - TIOCSCTTY is the only thing that does (M89). So the flag is
     * accepted and has nothing to do, which is a truthful no-op rather
     * than an ignored request. */
    return open("/dev/ptmx", flags & ~O_NOCTTY);
}

int grantpt(int fd) {
    /* Not a no-op that ignores its argument: a descriptor that is not a
     * pty master has no permissions to grant, and saying yes to that
     * would be the lie this file's header distinguishes. */
    int n = 0;
    if (ioctl(fd, TIOCGPTN, &n) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int unlockpt(int fd) {
    int n = 0;
    if (ioctl(fd, TIOCGPTN, &n) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int ptsname_r(int fd, char *buf, size_t len) {
    int n = 0;
    if (ioctl(fd, TIOCGPTN, &n) != 0) {
        errno = ENOTTY;
        return ENOTTY;
    }
    if (len < sizeof("/dev/pts/") + 2) {
        errno = ERANGE;
        return ERANGE;
    }
    snprintf(buf, len, "/dev/pts/%d", n);
    return 0;
}

char *ptsname(int fd) {
    /* A static buffer, which is what the interface specifies and is the
     * reason ptsname_r exists beside it. Written down rather than left
     * as a surprise: two threads calling this share one answer. */
    static char name[32];
    if (ptsname_r(fd, name, sizeof(name)) != 0) {
        return NULL;
    }
    return name;
}

int openpty(int *primary, int *secondary, char *name,
            const struct termios *tio, const struct winsize *ws) {
    char path[32];
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) {
        return -1;
    }
    if (grantpt(m) != 0 || unlockpt(m) != 0 ||
        ptsname_r(m, path, sizeof(path)) != 0) {
        close(m);
        return -1;
    }
    int s = open(path, O_RDWR | O_NOCTTY);
    if (s < 0) {
        close(m);
        return -1;
    }
    if (tio) {
        tcsetattr(s, TCSANOW, tio);
    }
    if (ws) {
        ioctl(m, TIOCSWINSZ, (void *)ws);
    }
    if (primary) {
        *primary = m;
    }
    if (secondary) {
        *secondary = s;
    }
    if (name) {
        /* The interface's own worst corner: `name` is a char* with no
         * length, so the caller has to have provided enough room and
         * there is no way to check. Every implementation copies and hopes;
         * this one copies a string whose maximum length is known here
         * (/dev/pts/ plus one digit, because PTY_MAX is 8), which is the
         * best that can be done with the signature. */
        strcpy(name, path);
    }
    return 0;
}

int login_tty(int fd) {
    /* The three things that make a descriptor *the* terminal for this
     * process: a new session, the terminal claimed by it, and the three
     * standard descriptors pointing at it. Doing any two of the three is
     * the classic half-detached daemon. */
    if (setsid() < 0) {
        return -1;
    }
    if (ioctl(fd, TIOCSCTTY, 0) != 0) {
        return -1;
    }
    if (dup2(fd, 0) < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) {
        return -1;
    }
    if (fd > 2) {
        close(fd);
    }
    return 0;
}

pid_t forkpty(int *primary, char *name,
              const struct termios *tio, const struct winsize *ws) {
    int m = -1, s = -1;
    if (openpty(&m, &s, name, tio, ws) != 0) {
        return (pid_t)-1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(m);
        close(s);
        return (pid_t)-1;
    }
    if (pid == 0) {
        /* The child keeps the slave and must not keep the master: a
         * child holding the master end is a child that keeps its own
         * terminal alive after the parent has closed it, and the parent
         * then never sees end-of-file. */
        close(m);
        if (login_tty(s) != 0) {
            _exit(127);
        }
        return 0;
    }
    close(s);
    if (primary) {
        *primary = m;
    }
    return pid;
}
