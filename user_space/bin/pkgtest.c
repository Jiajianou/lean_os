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
    if (link("/pkg/repo/index", "/tmp/pkg-index-alias") == 0) {
        printf("[m111] hard link OUT of /pkg succeeded without pkg-admin\n");
        return 1;
    }
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
        int rc = run(argv);
        if (rc != 0) {
            printf("[m111] /pkg/bin/grep exited %d looking for a needle that "
                   "is in the file\n", rc);
            return E_GREP_WRONG;
        }
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

    {
        const char *argv[] = {"/bin/os", "install", "impostor", 0};
        if (run(argv) != 0) {
            printf("[m111] `os install impostor` failed\n");
            return E_INSTALL;
        }
    }
    {
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
        if (rc0 != 0) {
            printf("[m111] the impostor reached through /pkg/bin was launched "
                   "with mask 0x%x\n", (unsigned)rc0);
            return E_IMPOSTOR;
        }
        printf("[m111] a package binary named `compositor` gets 0x0, not "
               "CAP_ALL - the grant table does not reach /pkg\n");
    }

    {
        unlink("/pkg/db/intruder");
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

    {
        int fd = open("/tmp/pkgtest-scratch", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, "x", 1);
            close(fd);
        }
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

    {
        const char *argv[] = {"/bin/os", "verify", "grep", 0};
        if (run(argv) != 0) {
            printf("[m111] `os verify grep` says the installed files are not "
                   "what was installed\n");
            return E_VERIFY;
        }
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
