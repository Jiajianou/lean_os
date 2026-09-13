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
    return open("/dev/ptmx", flags & ~O_NOCTTY);
}

int grantpt(int fd) {
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
        strcpy(name, path);
    }
    return 0;
}

int login_tty(int fd) {
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
