/* user_space/bin/ptytest.c - M85's second fixture
 *
 * A pseudo-terminal, driven from both ends by one program, which is the
 * only way to test one: a pty has two halves and a test that holds only
 * one of them is testing a pipe.
 *
 * What it proves, in the order the checks run:
 *
 *   1. Opening /dev/ptmx produces a terminal, and ptsname says which
 *      one. There is no other way to find out - the master descriptor
 *      does not carry the name.
 *   2. The slave is a path that exists and can be opened. /dev/pts/<n>
 *      appears when the pair is made, which makes it the first directory
 *      on this machine whose contents change.
 *   3. The LINE DISCIPLINE is on this path. A line typed at the master
 *      with a backspace in it arrives at the slave edited, whole, and
 *      only after Enter - which is the property that distinguishes a
 *      terminal from a pipe, and the property a shell depends on.
 *   4. The ECHO comes back to the master. That is what a terminal
 *      emulator draws, and it is not the same bytes as the slave's own
 *      output.
 *   5. The slave's output reaches the master, with ONLCR applied.
 *   6. ^C typed at the master raises SIGINT on the child in the
 *      foreground group, which is the whole reason a build running in a
 *      terminal can be stopped.
 *   7. ^Z stops the child rather than being read as a byte, waitpid
 *      with WUNTRACED reports the stop without reaping it, and SIGCONT
 *      puts it back. This is the half M85 shipped untested - see
 *      kernel/kernel.c's note in the [m85] block for why it is graded
 *      here rather than from the kernel.
 *   8. Closing the master hangs the slave up: a read returns 0 rather
 *      than blocking forever. This is the check that would otherwise
 *      show up as "the machine hangs on shutdown".
 *
 * Exit codes, so the kernel self-test can say which one failed:
 *   0  every check passed
 *   2  /dev/ptmx would not open
 *   3  ptsname gave a name that is not /dev/pts/<n>
 *   4  the slave would not open
 *   5  the line discipline did not edit or did not wait for Enter
 *   6  the echo did not come back to the master
 *   7  the slave's output did not reach the master
 *   8  ^C did not reach the child in the foreground group
 *   9  closing the master did not hang the slave up
 *  10  fork failed
 *  11  the ^Z fixture child never announced itself
 *  12  SIGCONT did not put the stopped child back
 *  13  login_tty did not make the child's group the foreground one
 *  14  waitpid(WUNTRACED) did not return when the child stopped
 *  15  it returned, but not as a stop by SIGTSTP
 *  16  it reported the same stop twice
 */
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* Reads until `want` bytes have arrived or the descriptor ends. Every
 * read here is on a descriptor that blocks, so a short read is the
 * ordinary case rather than a failure - which is exactly the contract
 * SYS_read has had since M14 and the reason this loop exists. */
static int read_n(int fd, char *buf, int want) {
    int got = 0;
    while (got < want) {
        int n = (int)read(fd, buf + got, (size_t)(want - got));
        if (n <= 0) {
            break;
        }
        got += n;
    }
    buf[got] = '\0';
    return got;
}

int main(void) {
    char name[64];
    char buf[256];

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

    /* ---- 3 and 4: the discipline, and the echo -------------------------
     *
     * "abX", one erase, "c", Enter. Nothing is readable at the slave
     * until the Enter, and what arrives then is "abc\n" - the same
     * assertion the kernel-side M85 self-test makes about the console,
     * made here about a terminal that has no hardware anywhere near it. */
    if (write(m, "abX\177c", 5) != 5) {
        return 5;
    }
    /* The echo of what was typed, which arrives before anything the
     * program writes: 'a', 'b', 'X', then ECHOE's backspace-space-
     * backspace for the erase, then 'c'. Seven bytes for five typed, and
     * asserting the count is what proves the erase was echoed as an
     * erase rather than as a DEL character. */
    if (read_n(m, buf, 7) != 7 || strcmp(buf, "abX\b \bc") != 0) {
        return 6;
    }
    /* Deliberately NOT asserting the slave has nothing readable by
     * calling read() on it - that would block forever and hang the boot,
     * which is precisely the mistake M85's first attempt recorded. The
     * Enter below is what makes the line appear, and a discipline that
     * delivered early would put more than four bytes in front of it. */
    if (write(m, "\n", 1) != 1) {
        return 5;
    }
    if (read_n(s, buf, 4) != 4 || strcmp(buf, "abc\n") != 0) {
        return 5;
    }
    /* And the Enter's own echo, drained here rather than left in the
     * queue - a test that leaves bytes behind makes the next check read
     * somebody else's output, which is exactly how this one first
     * failed. */
    if (read_n(m, buf, 2) != 2 || strcmp(buf, "\r\n") != 0) {
        return 6;
    }

    /* ---- 5: the slave's output reaches the master ---------------------- */
    if (write(s, "hi\n", 3) != 3) {
        return 7;
    }
    {
        /* ONLCR: three bytes written, four arrive. That is output
         * processing happening on this path and not somewhere else. */
        int n = read_n(m, buf, 4);
        if (n != 4 || strcmp(buf, "hi\r\n") != 0) {
            return 7;
        }
    }

    /* ---- 6: ^C reaches the foreground group ---------------------------
     *
     * A child, in its own process group, holding the slave as its
     * controlling terminal. ^C written at the master must kill it - and
     * "kill" rather than "be read as a byte" is the whole distinction. */
    pid_t pid = fork();
    if (pid < 0) {
        return 10;
    }
    if (pid == 0) {
        close(m);
        if (login_tty(s) != 0) {
            _exit(30);
        }
        /* Say so before waiting, so the parent knows the child has the
         * terminal rather than guessing with a sleep. */
        write(1, "R\n", 2);
        for (;;) {
            char c;
            if (read(0, &c, 1) <= 0) {
                _exit(31); /* end of file, not the signal that was aimed here */
            }
        }
    }
    close(s);
    {
        int n = read_n(m, buf, 3); /* "R\r\n" - ONLCR again */
        if (n != 3 || buf[0] != 'R') {
            return 8;
        }
    }
    /* No TIOCSPGRP here, and that is the correct shape rather than a
     * shortcut: this process is in a different session from the child,
     * and a terminal's job control belongs to the session that owns it.
     * login_tty's TIOCSCTTY is what put the child's group in the
     * foreground, which is exactly what happens when a terminal emulator
     * starts a shell. */
    if (write(m, "\003", 1) != 1) { /* ^C */
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

    /* ---- 7: ^Z stops a job, and SIGCONT puts it back -------------------
     *
     * The same shape as the ^C check and a different outcome, which is
     * the point: one of these two characters ends the child and the
     * other suspends it, and a terminal that treated either as input
     * would pass no version of this test. */
    /* A fresh pair, and not a shortcut: the first one has no slave end
     * open any more - the parent closed its copy and the child took its
     * own to the grave - so its master reads end-of-file forever. That is
     * the hangup rule working, and a second job needs a second terminal
     * exactly as a second window would. */
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
        /* Reads a line and exits with what it read. If ^Z were delivered
         * as a byte rather than as a signal this read would return it
         * and the exit code would say so, which is how this test can
         * tell "stopped" from "read the wrong thing". */
        char c = 0;
        if (read(0, &c, 1) != 1) {
            _exit(32);
        }
        _exit(c == 'k' ? 0 : 33);
    }
    close(s);
    {
        int n = read_n(m, buf, 3);
        if (n != 3 || buf[0] != 'S') {
            return 11;
        }
    }
    /* The terminal is the child's now, and its group is the foreground
     * one - which login_tty's TIOCSCTTY arranged. Checked rather than
     * assumed, because a ^Z that reaches nobody looks exactly like a ^Z
     * that was read as a byte. */
    {
        int fg = 0;
        if (ioctl(m, TIOCGPGRP, &fg) != 0 || fg != (int)pid) {
            return 13;
        }
    }
    if (write(m, "\032", 1) != 1) { /* ^Z */
        return 11;
    }
    {
        int status = 0;
        /* Blocks until the child stops - and returns without reaping it,
         * which is the whole difference between this and every other
         * return from waitpid. */
        pid_t w = waitpid(pid, &status, WUNTRACED);
        if (w != pid) {
            return 14;
        }
        if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTSTP) {
            return 15;
        }
        /* Once. A child that is still stopped must not be reported
         * again, or a shell's wait loop prints "Stopped" forever. */
        if (waitpid(pid, &status, WUNTRACED | WNOHANG) != 0) {
            return 16;
        }
    }
    if (kill(pid, SIGCONT) != 0) {
        return 12;
    }
    if (write(m, "k\n", 2) != 2) {
        return 12;
    }
    {
        int status = 0;
        pid_t w = waitpid(pid, &status, WUNTRACED);
        if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            return 12;
        }
    }
    printf("ptytest: ^Z stopped the job and SIGCONT put it back\n");

    /* ---- 7: closing the master hangs the slave up ---------------------- */
    {
        int m2 = -1, s2 = -1;
        if (openpty(&m2, &s2, NULL, NULL, NULL) != 0) {
            return 9;
        }
        close(m2);
        char c;
        /* Zero, not a block. A slave whose master has gone can never
         * receive another byte, and a read that waited for one would be
         * the hang this check exists to rule out. */
        if (read(s2, &c, 1) != 0) {
            return 9;
        }
        close(s2);
    }

    printf("ptytest: all eight checks passed\n");
    return 0;
}
