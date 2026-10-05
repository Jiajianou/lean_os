#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int read_n(int fd, char *buffer, int want) {
    int got = 0;
    while (got < want) {
        int n = (int)read(fd, buffer + got, (size_t)(want - got));
        if (n <= 0) {
            break;
        }
        got += n;
    }
    buffer[got] = '\0';
    return got;
}

int main(void) {
    char name[64];
    char buffer[256];

    int m = posix_openpt(O_RDWR);
    if (m < 0) {
        return 2;
    }
    if (grantpt(m) != 0 || unlockpt(m) != 0) {
        return 2;
    }
    if (ptsname_r(m, name, sizeof(name)) != 0 ||
        strncmp(name, "/dev/pts/", 9) != 0 || name[9] < '0' || name[9] > '9') {
        return 3;
    }
    printf("ptytest: master open, slave is %s\n", name);

    int s = open(name, O_RDWR);
    if (s < 0) {
        return 4;
    }

    if (write(m, "abX\177c", 5) != 5) {
        return 5;
    }
    if (read_n(m, buffer, 7) != 7 || strcmp(buffer, "abX\b \bc") != 0) {
        return 6;
    }
    if (write(m, "\n", 1) != 1) {
        return 5;
    }
    if (read_n(s, buffer, 4) != 4 || strcmp(buffer, "abc\n") != 0) {
        return 5;
    }
    if (read_n(m, buffer, 2) != 2 || strcmp(buffer, "\r\n") != 0) {
        return 6;
    }

    if (write(s, "hi\n", 3) != 3) {
        return 7;
    }
    {
        int n = read_n(m, buffer, 4);
        if (n != 4 || strcmp(buffer, "hi\r\n") != 0) {
            return 7;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        return 10;
    }
    if (pid == 0) {
        close(m);
        if (login_tty(s) != 0) {
            _exit(30);
        }
        write(1, "R\n", 2);
        for (;;) {
            char c;
            if (read(0, &c, 1) <= 0) {
                _exit(31);
            }
        }
    }
    close(s);
    {
        int n = read_n(m, buffer, 3);
        if (n != 3 || buffer[0] != 'R') {
            return 8;
        }
    }
    if (write(m, "\003", 1) != 1) {
        return 8;
    }
    {
        int status = 0;
        pid_t w = waitpid(pid, &status, 0);
        if (w != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGINT) {
            return 8;
        }
    }
    printf("ptytest: ^C at the master killed the child in the foreground group\n");

    if (openpty(&m, &s, NULL, NULL, NULL) != 0) {
        return 2;
    }
    pid = fork();
    if (pid < 0) {
        return 10;
    }
    if (pid == 0) {
        close(m);
        if (login_tty(s) != 0) {
            _exit(30);
        }
        write(1, "S\n", 2);
        char c = 0;
        if (read(0, &c, 1) != 1) {
            _exit(32);
        }
        _exit(c == 'k' ? 0 : 33);
    }
    close(s);
    {
        int n = read_n(m, buffer, 3);
        if (n != 3 || buffer[0] != 'S') {
            return 11;
        }
    }
    {
        int fg = 0;
        if (ioctl(m, TIOCGPGRP, &fg) != 0 || fg != (int)pid) {
            return 13;
        }
    }
    if (write(m, "\032", 1) != 1) {
        return 11;
    }
    {
        int status = 0;
        pid_t w = waitpid(pid, &status, WUNTRACED);
        if (w != pid) {
            return 14;
        }
        if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTSTP) {
            return 15;
        }
        if (waitpid(pid, &status, WUNTRACED | WNOHANG) != 0) {
            return 16;
        }
    }
    if (kill(pid, SIGCONT) != 0) {
        printf("ptytest: SIGCONT to the stopped job failed (errno %d)\n", errno);
        return 12;
    }
    if (write(m, "k\n", 2) != 2) {
        printf("ptytest: the master would not take the line that ends the job\n");
        return 12;
    }
    {
        int status = 0;
        pid_t w = waitpid(pid, &status, WUNTRACED);
        if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("ptytest: after SIGCONT, waitpid said %d with status 0x%x, wanted %d "
                   "exiting 0\n", (int)w, status, (int)pid);
            return 12;
        }
    }
    printf("ptytest: ^Z stopped the job and SIGCONT put it back\n");

    {
        int m2 = -1, s2 = -1;
        if (openpty(&m2, &s2, NULL, NULL, NULL) != 0) {
            return 9;
        }
        close(m2);
        char c;
        if (read(s2, &c, 1) != 0) {
            return 9;
        }
        close(s2);
    }

    printf("ptytest: all eight checks passed\n");
    return 0;
}
