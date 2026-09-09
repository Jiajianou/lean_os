/* user_space/bin/dirtest.c - M112's proof, on the machine.
 *
 * The boot self-test for user_space/lib/fsutil.c's two tree walks.
 *
 * tests/test_fsutil.c already grades them, and grades them harder: it
 * reaches the depth ceiling, the batch boundary and the empty cases in
 * microseconds, against the host's own filesystem. So why this too.
 *
 * Because that test runs against tests/fakes/fake_user_fs.c, and a fake
 * agrees with whatever it was written to agree with. What it cannot tell
 * anybody is whether SYS_getdents on *leanfs* reports a directory as
 * OS_DT_DIR, whether a cookie means what the walk assumes it means after
 * the entries under it have been unlinked, or whether a path built by
 * appending sixteen components is still a path this resolver accepts.
 * Every one of those is a question about this machine, and the only
 * instrument that can ask it is this machine. It is the same split
 * CLAUDE.md writes down: no instrument here subsumes another.
 *
 * What it does, in order:
 *
 *   1. Builds a tree with SYS_mkdir and SYS_open: three levels, eight
 *      files, one of the directories holding more than one SYS_getdents
 *      buffer's worth of names.
 *   2. Counts it with fsutil_count_tree and requires the exact numbers
 *      the build wrote down - entries and bytes both.
 *   3. Requires SYS_rmdir to refuse the top of it, which is the kernel
 *      rule this whole user-space recursion exists because of.
 *   4. Removes it with fsutil_remove_tree and requires SYS_stat to say
 *      every level is gone - asked of the filesystem, not of the walk.
 *   5. Requires the sibling directory beside it to be untouched, which
 *      is the failure that would matter most and the one a count cannot
 *      see.
 *
 * Exit codes are distinct per failure and the kernel prints the one it
 * got - see kernel.c's [m112] block.
 */
#include "fsutil.h"
#include "paths.h"
#include "syscall_wrappers.h"

#define E_OK          0
#define E_MKDIR       2
#define E_CREATE      3
#define E_COUNT       4
#define E_COUNT_WRONG 5
#define E_RMDIR_TOOK_A_FULL_ONE 6
#define E_REMOVE      7
#define E_STILL_THERE 8
#define E_TOOK_A_NEIGHBOUR 9
#define E_NAME_RULE   10

#define ROOT     "/home/m112tree"
#define NEIGHBOUR "/home/m112keep"

/* Eight files of known size, so the byte total is a number this program
 * wrote rather than one it read back and agreed with. Twelve names in
 * one directory is past what a single 1024-byte SYS_getdents buffer
 * returns for names this length, which is the point of having twelve. */
#define WIDE_FILES 12
#define WIDE_BYTES 7

static int make_file(const char *path, int bytes) {
    long fd = sys_open(path, OPEN_WRITE | OPEN_CREATE | OPEN_EXCL);
    if (fd < 0) {
        return -1;
    }
    static const char FILL[64] = {0};
    long n = sys_write((int)fd, FILL, (size_t)bytes);
    sys_close((int)fd);
    return n == bytes ? 0 : -1;
}

static int gone(const char *path) {
    os_stat_t st;
    return sys_stat(path, &st) != 0;
}

int main(void) {
    /* A previous run's tree, if this image has been booted with the
     * self-tests before. Removed with the thing under test, which is a
     * small extra use of it rather than a check. */
    fsutil_remove_tree(ROOT);
    fsutil_remove_tree(NEIGHBOUR);

    if (sys_mkdir(ROOT) != 0 || sys_mkdir(ROOT "/mid") != 0 ||
        sys_mkdir(ROOT "/mid/deep") != 0 || sys_mkdir(ROOT "/wide") != 0) {
        return E_MKDIR;
    }
    if (sys_mkdir(NEIGHBOUR) != 0 || make_file(NEIGHBOUR "/keep", 5) != 0) {
        return E_MKDIR;
    }
    if (make_file(ROOT "/top", 3) != 0 || make_file(ROOT "/mid/a", 10) != 0 ||
        make_file(ROOT "/mid/deep/b", 20) != 0) {
        return E_CREATE;
    }
    for (int i = 0; i < WIDE_FILES; i++) {
        char path[64];
        int n = 0;
        for (const char *s = ROOT "/wide/file"; *s; s++) {
            path[n++] = *s;
        }
        path[n++] = (char)('0' + i / 10);
        path[n++] = (char)('0' + i % 10);
        path[n] = '\0';
        if (make_file(path, WIDE_BYTES) != 0) {
            return E_CREATE;
        }
    }

    /* mid, deep, wide, top, a, b, and WIDE_FILES under wide. */
    fsutil_tree_t t;
    if (fsutil_count_tree(ROOT, &t) != 0) {
        return E_COUNT;
    }
    if (t.entries != (uint32_t)(6 + WIDE_FILES) ||
        t.bytes != (uint32_t)(3 + 10 + 20 + WIDE_FILES * WIDE_BYTES) || t.deep) {
        return E_COUNT_WRONG;
    }

    /* The kernel rule this recursion exists because of. If SYS_rmdir
     * ever starts taking a directory with things in it, the confirm
     * dialog in file_manager.c is asking the wrong question - so this is
     * checked here rather than assumed from a comment. */
    if (sys_rmdir(ROOT) == 0) {
        return E_RMDIR_TOOK_A_FULL_ONE;
    }

    if (fsutil_remove_tree(ROOT) != 0) {
        return E_REMOVE;
    }
    if (!gone(ROOT) || !gone(ROOT "/mid") || !gone(ROOT "/mid/deep") ||
        !gone(ROOT "/mid/deep/b") || !gone(ROOT "/wide") || !gone(ROOT "/top")) {
        return E_STILL_THERE;
    }

    /* The one that would matter most. A recursive delete that takes a
     * sibling with it removes data nobody asked about, and every count
     * above would still have been right. */
    os_stat_t st;
    if (sys_stat(NEIGHBOUR, &st) != 0 || sys_stat(NEIGHBOUR "/keep", &st) != 0 ||
        st.size != 5) {
        return E_TOOK_A_NEIGHBOUR;
    }
    fsutil_remove_tree(NEIGHBOUR);

    /* And the rule that keeps a typed name inside the folder the window
     * is showing. Graded here as well as on the host because the whole
     * point of it is what it refuses to hand to *this* resolver. */
    if (fsutil_name_ok("../elsewhere") || fsutil_name_ok("a/b") ||
        fsutil_name_ok("..") || fsutil_name_ok("") || !fsutil_name_ok("ordinary.txt")) {
        return E_NAME_RULE;
    }
    return E_OK;
}
