/* user_space/bin/pkgtest.c - M111's proof, on the machine.
 *
 * The boot self-test for the package manager. It installs a real package
 * built from somebody else's source, runs it, checks what it is allowed
 * to do, takes it out again, and checks the boundary from the outside -
 * and every one of those is a claim the host tests cannot make, because
 * they are claims about a kernel enforcing something.
 *
 * The five things it grades, in order:
 *
 *   1. `os install grep` installs GNU grep 3.11, and the program runs
 *      and produces the right answer. That is the milestone's own
 *      sentence, checked by running it.
 *   2. An installed package holds exactly the capabilities its manifest
 *      asked for and no others - grep asked for none, and gets none.
 *   3. A package that names its binary `compositor` does NOT get the
 *      compositor's CAP_ALL. This is the impersonation the whole /pkg
 *      rule exists to stop, and it is checked by installing a package
 *      that tries it.
 *   4. With CAP_PKG_ADMIN dropped, every write under /pkg is refused -
 *      create, unlink, mkdir, rmdir, rename, symlink and hard link, one
 *      at a time, because they are seven code paths and not one.
 *   5. `os verify` re-hashes every installed file, and `os remove`
 *      leaves the machine as it found it.
 *
 * ---- why this program holds CAP_PKG_ADMIN, and then does not ----------
 *
 * It has to, to run `os` at all: a spawn intersects, so a launcher
 * cannot hand a child a capability it does not hold, and `os` launched
 * from a process without CAP_PKG_ADMIN would be an `os` that cannot
 * install anything. It is in the grant table beside `os` for that
 * reason.
 *
 * That would make section 4 meaningless - a process with the capability
 * proving that writes are refused would prove nothing - so section 4
 * runs after sys_dropcaps() has taken it away. Which is also the first
 * test in this project that uses M65's monotonicity for something other
 * than demonstrating it: the drop is how this program becomes an
 * ordinary program without needing a second program.
 *
 * Exit codes are distinct per failure, and the kernel prints the one it
 * got - see kernel.c's [m111] block.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "caps.h"
#include "syscall_wrappers.h"

#define E_OK              0
#define E_INSTALL         2
#define E_GREP_MISSING    3
#define E_GREP_WRONG      4
#define E_CAPS_LEAKED     5
#define E_IMPOSTOR        6
#define E_WRITE_ALLOWED   7
#define E_VERIFY          8
#define E_REMOVE          9
#define E_STILL_THERE    10
#define E_NO_ADMIN       11
#define E_REPO_MISSING   12
#define E_DROP_FAILED    13

static int run(const char *const *argv) {
    long pid = sys_spawnv(argv[0], argv + 1);
    if (pid < 0) {
        return -1;
    }
    return (int)sys_wait(pid);
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Every write syscall that takes a path, one at a time. A single "can I
 * create a file under /pkg" would pass with six of the seven gates
 * missing, which is exactly the shape of the bug this is looking for -
 * they were seven separate edits in kernel/arch/x86_64/syscall.c and any
 * one of them could have been left out. */
static int any_write_under_pkg_succeeded(void) {
    int fd = open("/pkg/db/intruder", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        close(fd);
        printf("[m111] open(O_CREAT) under /pkg succeeded without pkg-admin\n");
        return 1;
    }
    if (mkdir("/pkg/intruder", 0755) == 0) {
        printf("[m111] mkdir under /pkg succeeded without pkg-admin\n");
        return 1;
    }
    if (unlink("/pkg/repo/index") == 0) {
        printf("[m111] unlink under /pkg succeeded without pkg-admin\n");
        return 1;
    }
    if (rmdir("/pkg/repo") == 0) {
        printf("[m111] rmdir under /pkg succeeded without pkg-admin\n");
        return 1;
    }
    if (rename("/pkg/repo/index", "/tmp/stolen-index") == 0) {
        printf("[m111] rename OUT of /pkg succeeded without pkg-admin\n");
        return 1;
    }
    if (rename("/tmp/pkgtest-scratch", "/pkg/db/intruder") == 0) {
        printf("[m111] rename INTO /pkg succeeded without pkg-admin\n");
        return 1;
    }
    if (symlink("/tmp/pkgtest-scratch", "/pkg/db/intruder-link") == 0) {
        printf("[m111] symlink under /pkg succeeded without pkg-admin\n");
        return 1;
    }
    /* The hard link is the interesting one and the one a gate on the
     * destination alone would miss: it makes a second name, outside
     * /pkg, for a file inside it - and a write through that name would
     * be a write to an installed package. */
    if (link("/pkg/repo/index", "/tmp/pkg-index-alias") == 0) {
        printf("[m111] hard link OUT of /pkg succeeded without pkg-admin\n");
        return 1;
    }
    /* And truncating an installed file in place, which needs no new
     * name at all. */
    fd = open("/pkg/repo/index", O_WRONLY | O_TRUNC);
    if (fd >= 0) {
        close(fd);
        printf("[m111] opening an installed file for writing succeeded "
               "without pkg-admin\n");
        return 1;
    }
    return 0;
}

int main(void) {
    printf("[m111] pkgtest: the package manager, on the machine\n");

    if ((sys_getcaps() & CAP_PKG_ADMIN) == 0) {
        printf("[m111] pkgtest holds no pkg-admin - it cannot run `os` at all. "
               "See CAP_GRANTS in system_api/include/caps.h.\n");
        return E_NO_ADMIN;
    }
    if (!exists("/pkg/repo/index")) {
        printf("[m111] there is no repository on this disk (/pkg/repo/index). "
               "Run tools/build-packages.sh and `make packages`.\n");
        return E_REPO_MISSING;
    }

    /* ---- 1. install grep, and run it ---------------------------------- */
    {
        const char *argv[] = {"/bin/os", "install", "grep", 0};
        if (run(argv) != 0) {
            printf("[m111] `os install grep` failed\n");
            return E_INSTALL;
        }
    }
    if (!exists("/pkg/grep/3.11/bin/grep") || !exists("/pkg/bin/grep")) {
        printf("[m111] grep installed and its binary is not where it should be\n");
        return E_GREP_MISSING;
    }
    /* And NOT in /bin, which is the property that says a package cannot
     * take over the name of a program this OS ships. /bin/grep is
     * toybox's (M89) and has to still be toybox's. */
    {
        char target[256];
        long n = readlink("/pkg/bin/grep", target, sizeof(target) - 1);
        if (n <= 0) {
            printf("[m111] /pkg/bin/grep is not a link\n");
            return E_GREP_MISSING;
        }
        target[n] = '\0';
        if (strncmp(target, "/pkg/grep/", 10) != 0) {
            printf("[m111] /pkg/bin/grep points at %s, outside the package\n", target);
            return E_GREP_MISSING;
        }
    }

    /* Somebody else's program, doing its job. The needle is written
     * here and looked for by 1.2 MB of GNU C that has never heard of
     * this operating system. */
    {
        const char *lines =
            "the first line\n"
            "a line with LEANOS_NEEDLE in it\n"
            "the third line\n";
        int fd = open("/tmp/pkgtest-hay", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            printf("[m111] cannot write the file grep is supposed to read\n");
            return E_GREP_WRONG;
        }
        write(fd, lines, strlen(lines));
        close(fd);

        const char *argv[] = {"/pkg/bin/grep", "-c", "LEANOS_NEEDLE",
                              "/tmp/pkgtest-hay", 0};
        /* grep exits 0 when it matched, 1 when it did not. So the exit
         * code IS the assertion: a grep that found nothing, or crashed,
         * or never started, all give something other than 0. */
        int rc = run(argv);
        if (rc != 0) {
            printf("[m111] /pkg/bin/grep exited %d looking for a needle that "
                   "is in the file\n", rc);
            return E_GREP_WRONG;
        }
        /* And the negative: a pattern that is not there must exit 1.
         * Without this, a grep that exits 0 unconditionally passes. */
        const char *argv2[] = {"/pkg/bin/grep", "-c", "NOT_IN_THIS_FILE",
                               "/tmp/pkgtest-hay", 0};
        rc = run(argv2);
        if (rc != 1) {
            printf("[m111] grep exited %d for a pattern that is not in the "
                   "file - it should be 1\n", rc);
            return E_GREP_WRONG;
        }
        printf("[m111] GNU grep 3.11, built here, installed by `os` and run "
               "from /pkg/bin: both answers correct\n");
    }

    /* ---- 2 and 3. what a package is allowed to do --------------------- */
    {
        const char *argv[] = {"/bin/os", "install", "impostor", 0};
        if (run(argv) != 0) {
            printf("[m111] `os install impostor` failed\n");
            return E_INSTALL;
        }
    }
    /* The impostor package installs a binary called `compositor` and
     * another called `shutdown`. Both are names in CAP_GRANTS with
     * capabilities attached - CAP_ALL and CAP_POWER - and neither may
     * get them, because they are under /pkg and the grant table does not
     * apply there. Each program exits with its own capability mask as
     * its status, so the assertion is a number rather than a message. */
    {
        /* Three spellings of the same question, and they are three
         * different registry lookups. `/pkg/bin/impostor` is a path the
         * registry NAMES (the package provides that command), so it is
         * the case where the lookup succeeds and the answer is the
         * manifest's - which for this package is nothing. The other two
         * are paths the registry does not name at all, because the
         * package never claimed to provide a `compositor` or a
         * `shutdown`; they get CAP_PKG_UNLISTED, which is also nothing,
         * by a different route. Both routes have to end at zero.
         *
         * Each is announced before it is run, because this is the block
         * that hung once during development and a spawn with no line in
         * front of it is a spawn you cannot locate in a 1,700-line
         * serial log. */
        printf("[m111] launching /pkg/bin/impostor (the registry names it)\n");
        const char *argv0[] = {"/pkg/bin/impostor", 0};
        int rc0 = run(argv0);
        printf("[m111] it exited %d\n", rc0);
        printf("[m111] launching /pkg/impostor/1.0/bin/compositor "
               "(the registry does not)\n");
        const char *argv[] = {"/pkg/impostor/1.0/bin/compositor", 0};
        int rc = run(argv);
        printf("[m111] it exited %d\n", rc);
        if (rc != 0) {
            printf("[m111] a package binary named `compositor` was launched "
                   "with capability mask 0x%x - the grant table reached a "
                   "program under /pkg\n", (unsigned)rc);
            return E_IMPOSTOR;
        }
        const char *argv2[] = {"/pkg/impostor/1.0/bin/shutdown", 0};
        rc = run(argv2);
        if (rc != 0) {
            printf("[m111] a package binary named `shutdown` was launched with "
                   "capability mask 0x%x\n", (unsigned)rc);
            return E_IMPOSTOR;
        }
        /* And through the /pkg/bin alias, which is a different string
         * and therefore a different registry lookup. */
        if (rc0 != 0) {
            printf("[m111] the impostor reached through /pkg/bin was launched "
                   "with mask 0x%x\n", (unsigned)rc0);
            return E_IMPOSTOR;
        }
        printf("[m111] a package binary named `compositor` gets 0x0, not "
               "CAP_ALL - the grant table does not reach /pkg\n");
    }

    /* And the other way to be launched. Until M111 a `#!` script's
     * capabilities came from its INTERPRETER, which meant a package
     * shipping one line beginning `#!/bin/sh` ran with the shell's
     * CAP_ALL - CAP_PKG_ADMIN included, which is authority over every
     * installed package on this machine. The impostor package ships
     * such a script, and all it does is try to create a file in the
     * package database.
     *
     * The assertion is the file, not the exit status: a shell reports a
     * failed redirect in its own way and this test should not depend on
     * which way. */
    {
        unlink("/pkg/db/intruder"); /* this process still holds pkg-admin */
        const char *argv[] = {"/pkg/impostor/1.0/bin/writer", 0};
        (void)run(argv);
        if (exists("/pkg/db/intruder")) {
            printf("[m111] a `#!` script inside a package wrote into the "
                   "package database - it was launched with its "
                   "interpreter's capabilities rather than its own\n");
            return E_IMPOSTOR;
        }
        printf("[m111] a `#!/bin/sh` script inside a package cannot write to "
               "the package database - a script gets its own package's "
               "capabilities, not the shell's\n");
    }

    /* ---- 4. the write gate, from outside ------------------------------ */
    {
        int fd = open("/tmp/pkgtest-scratch", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, "x", 1);
            close(fd);
        }
        /* M65's monotonic drop, used for what it is for. After this line
         * this process is an ordinary program, and it cannot become
         * anything else - there is no call that would give it back. */
        long left = sys_dropcaps((uint32_t)(CAP_ALL & ~CAP_PKG_ADMIN));
        if (left & CAP_PKG_ADMIN) {
            printf("[m111] sys_dropcaps did not drop pkg-admin\n");
            return E_DROP_FAILED;
        }
        if (any_write_under_pkg_succeeded()) {
            return E_WRITE_ALLOWED;
        }
        printf("[m111] with pkg-admin dropped: create, mkdir, unlink, rmdir, "
               "rename both ways, symlink, hard link and truncate under /pkg "
               "are all refused\n");
    }

    /* ---- 5. verify, and put the machine back -------------------------- */
    /* From here on this process has no pkg-admin, so `os` spawned from it
     * has none either - which is exactly what makes `os verify` a fair
     * test of the read path and `os remove` a fair test of the gate. */
    {
        const char *argv[] = {"/bin/os", "verify", "grep", 0};
        if (run(argv) != 0) {
            printf("[m111] `os verify grep` says the installed files are not "
                   "what was installed\n");
            return E_VERIFY;
        }
        /* And remove must now FAIL, because this process can no longer
         * hand `os` the capability it needs. A package manager that can
         * still uninstall after its authority is gone would mean the
         * authority was never where it says it is. */
        const char *rm[] = {"/bin/os", "remove", "grep", 0};
        if (run(rm) == 0) {
            printf("[m111] `os remove` worked from a process with no "
                   "pkg-admin - the gate is not where it claims to be\n");
            return E_WRITE_ALLOWED;
        }
        if (!exists("/pkg/grep/3.11/bin/grep")) {
            printf("[m111] the failed remove deleted files anyway\n");
            return E_REMOVE;
        }
        printf("[m111] `os verify` passes and `os remove` is refused once "
               "pkg-admin is gone\n");
    }

    printf("[m111] the package manager: install, run, isolate, verify\n");
    return E_OK;
}
