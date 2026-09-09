/* user_space/lib/fsutil.h - M112
 *
 * The parts of a file manager that are not drawing: how a size is
 * written down, whether a name may be created, and what it takes to
 * remove a directory that has things in it.
 *
 * Its own file rather than more static functions inside
 * user_space/bin/file_manager.c, for the reason user_space/lib/symtab.h
 * gives about the profiler's symbol resolver: these are the parts of
 * that program whose failure mode is a *plausible wrong answer*, and a
 * booted machine cannot see one. "8.4M" is as convincing as "8.4K"
 * beside a name; a name check that lets "../compositor" through creates
 * a file in a directory nobody was looking at; and a tree delete that
 * stops one level down leaves a folder that looks deleted in a window
 * that has already refreshed. None of the three announces itself.
 *
 * Split the way the host tier can grade it: everything above
 * fsutil_name_ok is a pure function over its arguments and is tested
 * directly (tests/test_fsutil.c), and the two tree walks below are
 * tested against a real directory tree through
 * tests/fakes/fake_user_fs.c - the host's own filesystem standing in for
 * this machine's, so what grades "the folder is gone" is `stat`, not
 * this code agreeing with itself.
 */
#pragma once

#include <stdint.h>

#include "syscall.h" /* system_api/include/syscall.h - os_dirent_t, OS_NAME_MAX */

/* Buffer sizes for the three formatters. Named rather than written as
 * literals at the call sites, because a formatter that outgrows its
 * caller's array is the one bug in this file that is not a wrong answer
 * but a corrupted one. */
#define FSUTIL_SIZE_MAX  12 /* "1023.9M" and room to spare */
#define FSUTIL_EXACT_MAX 16 /* "4,294,967,295" + NUL */
#define FSUTIL_DATE_MAX  12 /* "MM-DD HH:MM" + NUL */

/* "1234", "12.3K", "8.4M" - three significant figures and a suffix,
 * which is what a size *column* is for. An exact byte count is the wrong
 * answer there: nobody compares 1048576 to 999999 at a glance. Where the
 * exact number is the point, fsutil_format_exact is. */
void fsutil_format_size(uint32_t bytes, char *out);

/* "1,048,576" - the exact count, grouped in threes so the magnitude is
 * still readable. This is what the status strip and the info box show,
 * and it is the answer to "view file sizes" that a rounded column cannot
 * give: two files that both say "8.4K" are not the same size. */
void fsutil_format_exact(uint32_t n, char *out);

/* "MM-DD HH:MM", or "-" for a file written before this machine could
 * know the date. Deliberately not a friendly "3 minutes ago": that needs
 * a second clock reading per row and says less the moment anything is
 * more than a day old. */
void fsutil_format_date(uint32_t mtime, char *out);

/* Whether `name` is a single directory entry this program may create or
 * act on. 1 if it is, 0 if not.
 *
 * Refuses: empty, "." and "..", anything containing '/', anything longer
 * than OS_NAME_MAX, and any control character.
 *
 * The '/' rule is the one that matters and it is not cosmetic. Every
 * path this program builds is `cwd + '/' + name`, so a name carrying its
 * own separators is a name that reaches outside the directory the window
 * is showing - "../../etc/settings.conf" typed into a New File box would
 * be created where nobody was looking, and a rename would move a file
 * there. The window promises "in this folder"; this is what makes that
 * true rather than merely likely. The control-character rule is smaller
 * and the same shape: a name with a newline in it lists as two rows, and
 * a name nobody can read back is a name nobody can delete. */
int fsutil_name_ok(const char *name);

/* ---- the two tree walks -----------------------------------------------
 *
 * SYS_rmdir takes empty directories only, and the kernel's comment on it
 * says why: "recursive delete is one keystroke away from losing
 * everything under a path, and this OS has no trash to take it back out
 * of." That argument is about the *kernel*, and it still holds - the
 * syscall stays as it is. What changed in M112 is that a person can now
 * create folders from this window, and a file manager that will make a
 * folder but not remove one it filled is visibly half a feature.
 *
 * So the recursion lives here, in user space, where the confirm that
 * precedes it can say what it is about to destroy: fsutil_count_tree
 * runs first and the dialog names the number. "Delete folder and 41
 * items?" is a different question from "Delete this folder?" and it is
 * the one a person can actually answer.
 */

/* How deep either walk will go before refusing. leanfs has no depth
 * limit of its own, and a walk with no ceiling is a stack overflow with
 * a directory tree behind it. Sixteen is far past anything on this disk
 * (the deepest path in the shipped image is five) and shallow enough
 * that the recursion costs a few kilobytes at worst. */
#define FSUTIL_MAX_DEPTH 16

/* One level's worth of SYS_getdents. A whole directory does not have to
 * fit: both walks below re-read from the start until a pass finds
 * nothing, so a buffer this size is a batch size and not a capacity. */
#define FSUTIL_DIRENT_BUF 1024

typedef struct {
    uint32_t entries; /* files and directories below `path`, not counting it */
    uint32_t bytes;   /* the sum of their sizes */
    int      deep;    /* set if FSUTIL_MAX_DEPTH stopped the walk - the counts are then a floor, not a total */
} fsutil_tree_t;

/* Counts what is under `path`, which must be a directory. Returns 0, or
 * -1 if the directory could not be read at all. A subdirectory that
 * cannot be read is counted as the one entry it is and not descended
 * into, rather than failing the whole walk - the count exists to inform
 * a confirm dialog, and refusing to answer because one child is
 * unreadable would be worse than answering slightly low. */
int fsutil_count_tree(const char *path, fsutil_tree_t *out);

/* Removes `path` and everything under it. Returns 0 if the directory is
 * gone, -1 otherwise.
 *
 * Not atomic and cannot be: this filesystem has no transaction, so an
 * interrupted delete leaves a partly emptied tree. That is stated rather
 * than hidden, and it is why this returns a plain failure and the caller
 * re-lists - what is on the disk after a failure is what the next
 * refresh shows, which is the only honest report available. */
int fsutil_remove_tree(const char *path);
