/* system_api/include/paths.h
 *
 * M53: the filesystem layout, in one place both halves of the system
 * agree on. leanfs was flat until this milestone, so "where does this
 * live" was never a question anything had to answer - a name *was* a
 * location. Now that it isn't, the kernel (which seeds programs and
 * self-test fixtures) and user space (which spawns, saves and lists)
 * would otherwise each carry their own copy of the same four strings.
 *
 * The layout, and why each one exists:
 *
 *   /bin   programs, and only programs. This is what retires M48's
 *          "That file is not a program." toast for the common case: the
 *          launcher lists this directory, so a data file is no longer
 *          something it can offer to run at all, leaving that message
 *          for the genuinely broken file it was written for.
 *   /home  the user's own files - what the file manager opens on, and
 *          where the editor saves.
 *   /etc   configuration. settings.conf, which until now sat in the same
 *          list as the compositor.
 *   /tmp   the boot self-tests' own fixtures. They have to live
 *          somewhere, and "somewhere that is not /home" is the whole
 *          requirement: a person opening their file manager should not
 *          find m48trunc in it.
 */
#pragma once

#define PATH_BIN  "/bin"
#define PATH_HOME "/home"
#define PATH_ETC  "/etc"
#define PATH_ICONS "/icons" /* M63 stretch goal: icon blobs, one file each - see system_api/include/icon.h */
#define PATH_TMP  "/tmp"

/* The trailing-slash forms, for building a path by concatenation. Spelled
 * out rather than derived so a call site reads as the path it produces. */
#define PATH_BIN_DIR  "/bin/"
#define PATH_HOME_DIR "/home/"
#define PATH_ETC_DIR  "/etc/"
#define PATH_ICONS_DIR "/icons/"
#define PATH_TMP_DIR  "/tmp/"

/* M87: the two synthetic filesystems. Not directories on the disk - a
 * mount table entry each, answered by kernel/fs/devfs.c and
 * kernel/fs/procfs.c. Named here with everything else so that a caller
 * building a path does not have to know which of the three filesystems
 * it will land on. */
#define PATH_DEV      "/dev"
#define PATH_DEV_DIR  "/dev/"
#define PATH_PROC     "/proc"
#define PATH_PROC_DIR "/proc/"

#define PATH_SETTINGS PATH_ETC_DIR "settings.conf"

/* Longest absolute path anything here will build or resolve, NUL
 * included - the same number kernel/fs/leanfs.h's LEANFS_MAX_PATH uses,
 * repeated here because user space has no business including a kernel
 * header and a buffer sized differently from the resolver's own limit is
 * how a path gets silently truncated into a different file.
 *
 * M81: 128 -> 4096, in step with LEANFS_MAX_PATH. The two numbers have to
 * move together and this is why: the kernel stores a task's working
 * directory in a buffer of *this* size (task_t.cwd), so a PATH_MAX_LEN
 * smaller than the resolver's limit means a directory you can create and
 * open but cannot cd into. It also means user stacks got bigger - see
 * USER_STACK_PAGES in kernel/proc/proc.h, which four kilobytes of path in
 * a local variable made too small. */
#define PATH_MAX_LEN 4096

/* Joins `dir` (with its trailing slash) and `name` into out, which must
 * hold PATH_MAX_LEN bytes. Returns 0, or -1 if the result would not fit -
 * refused rather than truncated, for the reason above. */
static inline int path_join(char *out, const char *dir, const char *name) {
    int n = 0;
    for (const char *s = dir; *s; s++) {
        if (n >= PATH_MAX_LEN - 1) {
            return -1;
        }
        out[n++] = *s;
    }
    for (const char *s = name; *s; s++) {
        if (n >= PATH_MAX_LEN - 1) {
            return -1;
        }
        out[n++] = *s;
    }
    out[n] = '\0';
    return 0;
}
