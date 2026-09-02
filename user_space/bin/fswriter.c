/* user_space/bin/fswriter.c - M105's fixture: one of several writers.
 *
 * ---- what this program is for -----------------------------------------
 *
 * M71 deferred a journal and named two conditions under which one would
 * become worth building. The first was **multiple writers**. Every
 * re-measurement since - M81, M93 - has re-argued the second condition
 * (does the full scan get slow) and left the first alone, because this
 * machine had never actually had two programs writing to the filesystem
 * at the same time. Not "we believe it would be fine": nothing had run.
 *
 * So this runs. Several copies of this program are spawned at once and
 * each hammers its own subtree with the operations that make a journal
 * interesting - the ones that are a SEQUENCE of metadata writes rather
 * than a single one:
 *
 *   create   allocate an inode, then insert a directory entry
 *   write    allocate data blocks, write them, update the inode
 *   grow     allocate an indirect block, then the blocks it points at
 *   rename   remove one directory entry and add another
 *   unlink   free the blocks, free the inode, remove the entry
 *
 * A crash in the middle of any of those is exactly what write ordering
 * is supposed to survive, and two writers interleaving two of them is
 * exactly what M71's first condition was worried about.
 *
 * ---- what it checks, and what it deliberately does not ---------------
 *
 * Each writer verifies **its own** files: every byte it wrote comes back
 * as the byte it wrote, and every file it deleted is gone. It does not
 * look at anybody else's subtree, because "did writer 2 finish" is not a
 * question writer 1 can answer without a synchronisation this fixture
 * has no reason to have.
 *
 * The interesting check is not in here at all - it is the kernel-side
 * `leanfs_check` run after every writer has exited, which rebuilds the
 * block bitmap from the inodes and compares. A block that two writers
 * were both handed, or one that neither freed, shows up there and
 * nowhere else. This program's job is to create the traffic and to be
 * unable to lie about its own half of it.
 *
 * The content is keyed on the writer id and the file index, so a block
 * that ended up belonging to two files reads back as the other writer's
 * pattern rather than as plausible-looking garbage.
 *
 * Exit codes so a failure names itself:
 *   0  everything this writer wrote came back
 *   2  no writer id given
 *   3  could not create the writer's directory
 *   4  a create failed
 *   5  a write failed or was short
 *   6  a file read back as somebody else's bytes
 *   7  a rename failed
 *   8  an unlink failed, or a deleted file was still there
 *   9  a file that should have survived was gone
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Sized so that a file crosses the direct-block limit and forces an
 * indirect block to be allocated - which is the metadata sequence with
 * the most steps in it, and therefore the one worth generating. leanfs
 * blocks are 4096 bytes and an inode holds a handful of direct
 * pointers, so tens of kilobytes is comfortably past it. */
#define FILE_BYTES  (48 * 1024)
#define FILES       12

static char buf[FILE_BYTES];
static char back[FILE_BYTES];

/* One byte's value is decided by the writer, the file and the offset, so
 * no two writers and no two files ever produce the same byte sequence.
 * A block handed to two files at once cannot survive this. */
static unsigned char byte_for(int id, int file, int off) {
    return (unsigned char)(id * 71 + file * 13 + off * 7 + 29);
}

static void fill(int id, int file) {
    for (int i = 0; i < FILE_BYTES; i++) {
        buf[i] = (char)byte_for(id, file, i);
    }
}

static int write_all(const char *path, const char *src, int len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }
    int done = 0;
    while (done < len) {
        int n = (int)write(fd, src + done, (size_t)(len - done));
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += n;
    }
    /* The barrier M104 made real. A writer that never asks for its bytes
     * to be on the disk is a writer whose crash behaviour says nothing
     * about the filesystem. */
    fsync(fd);
    close(fd);
    return 0;
}

static int read_all(const char *path, char *dst, int len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    int done = 0;
    while (done < len) {
        int n = (int)read(fd, dst + done, (size_t)(len - done));
        if (n <= 0) {
            break;
        }
        done += n;
    }
    close(fd);
    return done;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return 2;
    }
    int id = atoi(argv[1]);

    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/w%d", id);
    /* Somebody else's leftovers are not this run's problem, and a
     * directory that already exists is not a failure. */
    mkdir(dir, 0755);
    struct stat st;
    if (stat(dir, &st) != 0) {
        return 3;
    }

    char tmp[96], final[96];

    for (int f = 0; f < FILES; f++) {
        fill(id, f);
        snprintf(tmp, sizeof(tmp), "%s/t%d", dir, f);
        snprintf(final, sizeof(final), "%s/f%d", dir, f);

        if (write_all(tmp, buf, FILE_BYTES) != 0) {
            return 5;
        }
        /* Write-temp-then-rename: M71's atomic replace, which is the
         * sequence a real program uses and the one whose two halves a
         * concurrent writer could interleave. */
        unlink(final);
        if (rename(tmp, final) != 0) {
            return 7;
        }

        memset(back, 0, sizeof(back));
        if (read_all(final, back, FILE_BYTES) != FILE_BYTES) {
            return 6;
        }
        if (memcmp(buf, back, FILE_BYTES) != 0) {
            return 6;
        }
    }

    /* Delete every other one, so the run ends with the bitmap holding a
     * mixture of blocks that were freed and blocks that were not. A run
     * that only ever allocates cannot catch a double-free. */
    for (int f = 0; f < FILES; f += 2) {
        snprintf(final, sizeof(final), "%s/f%d", dir, f);
        if (unlink(final) != 0) {
            return 8;
        }
        if (access(final, F_OK) == 0) {
            return 8;
        }
    }

    /* And the survivors are still exactly what this writer wrote, after
     * everybody else's deletions have been recycling blocks around them.
     * This is the assertion that a block freed by one writer and handed
     * to another would fail. */
    for (int f = 1; f < FILES; f += 2) {
        fill(id, f);
        snprintf(final, sizeof(final), "%s/f%d", dir, f);
        memset(back, 0, sizeof(back));
        int n = read_all(final, back, FILE_BYTES);
        if (n != FILE_BYTES) {
            return 9;
        }
        if (memcmp(buf, back, FILE_BYTES) != 0) {
            return 6;
        }
    }

    printf("fswriter %d: %d files of %d bytes written, half deleted, "
           "survivors verified\n", id, FILES, FILE_BYTES);
    return 0;
}
