#include "leanfs.h"

#include "drivers/ata.h"
#include "drivers/klog.h"
#include "lib/libk.h"

#define LEANFS_MAGIC     0x3253464Cu /* M53: bumped from 0x3153464C with the on-disk layout - an old disk formatted for the flat version has no root directory and no way to grow one, so it must be reformatted rather than misread */
#define LEANFS_START_LBA 2048u /* 1 MiB in - generously past the boot image; see Makefile's build-time size guard */

#define LEANFS_TYPE_FREE 0
#define LEANFS_TYPE_FILE 1
#define LEANFS_TYPE_DIR  2

#define ROOT_INODE 0

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t inode_table_lba;
    uint32_t inode_table_sectors;
    uint32_t bitmap_lba;
    uint32_t bitmap_sectors;
    uint32_t data_lba;
    uint32_t data_blocks;
    uint32_t reserved;
} leanfs_superblock_t;

/* M53: no name field any more. A name lives in exactly one place - the
 * records of the directory that holds it - which is what makes two
 * disagreeing answers to "what is this file called" impossible rather
 * than merely unlikely. `type` doubles as the allocated/free flag the
 * old `used` was. */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    uint32_t direct[LEANFS_DIRECT_BLOCKS];
    uint32_t indirect; /* block number of the pointer table, valid only once size implies more than LEANFS_DIRECT_BLOCKS blocks are in use */
} leanfs_inode_t;

#define INODE_TABLE_SECTORS ((sizeof(leanfs_inode_t) * LEANFS_MAX_INODES + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE)
#define BITMAP_SECTORS       (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE)

/* The claim leanfs.h makes about records never straddling a block, as a
 * check the compiler makes rather than a comment somebody has to keep
 * true - a padded record would silently break every directory on disk. */
_Static_assert(sizeof(leanfs_dirent_t) == 32, "leanfs_dirent_t must stay 32 bytes so 16 fit exactly in a block");
_Static_assert(LEANFS_BLOCK_SIZE % sizeof(leanfs_dirent_t) == 0, "a directory record must not straddle a block");

static leanfs_superblock_t sb;
static leanfs_inode_t inodes[LEANFS_MAX_INODES];
static uint8_t bitmap[BITMAP_SECTORS * LEANFS_BLOCK_SIZE];

/* M53: static, not stack. A kernel task stack is 8 KiB (sched.c's
 * TASK_STACK_SIZE) and the inode table is now 15 sectors - 7680 bytes -
 * so the buffer these two used to declare locally would have overflowed
 * the stack of any user task that reached them through SYS_writefile.
 * It was already 3584 bytes before this milestone, which was uncomfortably
 * close to the same cliff without anyone having measured it. leanfs has
 * no concurrency of its own (every caller is inside a syscall, and the
 * scheduler is cooperative at these points), so one shared scratch buffer
 * is safe as well as smaller. */
static uint8_t inode_table_buf[INODE_TABLE_SECTORS * LEANFS_BLOCK_SIZE];
/* One directory's whole contents, unpacked. Shared for the same reason,
 * and only ever live inside one directory operation at a time. */
static leanfs_dirent_t dirent_scratch[LEANFS_MAX_DIRENTS];

static void save_superblock(void) {
    uint8_t buf[LEANFS_BLOCK_SIZE];
    k_memset(buf, 0, sizeof(buf));
    k_memcpy(buf, &sb, sizeof(sb));
    ata_write_sectors(LEANFS_START_LBA, 1, buf);
}

static void save_inodes(void) {
    k_memset(inode_table_buf, 0, sizeof(inode_table_buf));
    k_memcpy(inode_table_buf, inodes, sizeof(inodes));
    ata_write_sectors(sb.inode_table_lba, (uint8_t)INODE_TABLE_SECTORS, inode_table_buf);
}

static void save_bitmap(void) {
    ata_write_sectors(sb.bitmap_lba, (uint8_t)BITMAP_SECTORS, bitmap);
}

static void format(void) {
    klog_puts("[fs] no valid leanfs superblock found - formatting fresh\n");

    sb.magic = LEANFS_MAGIC;
    sb.inode_table_lba = LEANFS_START_LBA + 1;
    sb.inode_table_sectors = INODE_TABLE_SECTORS;
    sb.bitmap_lba = sb.inode_table_lba + INODE_TABLE_SECTORS;
    sb.bitmap_sectors = BITMAP_SECTORS;
    sb.data_lba = sb.bitmap_lba + BITMAP_SECTORS;
    sb.data_blocks = LEANFS_DATA_BLOCKS;
    sb.reserved = 0;

    k_memset(inodes, 0, sizeof(inodes));
    k_memset(bitmap, 0, sizeof(bitmap));

    /* M53: the root directory is inode 0 and exists from the moment the
     * filesystem does. Empty (size 0, no blocks) - a directory with no
     * entries needs no storage, and dir_store below allocates on demand. */
    inodes[ROOT_INODE].type = LEANFS_TYPE_DIR;
    inodes[ROOT_INODE].size = 0;

    save_superblock();
    save_inodes();
    save_bitmap();
}

void leanfs_init(void) {
    uint8_t buf[LEANFS_BLOCK_SIZE];
    ata_read_sectors(LEANFS_START_LBA, 1, buf);
    k_memcpy(&sb, buf, sizeof(sb));

    /* M29: every field checked here sizes a fixed-size buffer somewhere
     * downstream (data_blocks -> the static `bitmap` array every
     * alloc_block/bitmap_test loop trusts as its own bound;
     * inode_table_sectors/bitmap_sectors -> exactly how many sectors
     * leanfs_init itself is about to read) - so any mismatch means a
     * superblock this build cannot safely interpret. Reformatting (the
     * one recovery path this driver already has and exercises on a blank
     * disk) is what keeps a corrupted field from turning into an
     * out-of-bounds write instead of just losing whatever was here.
     *
     * M53: the magic bump makes that path do double duty as the format
     * migration. There is no in-place upgrade from the flat layout - it
     * had no root directory and nothing to hang one off - and the kernel
     * re-seeds every program it ships on a fresh filesystem anyway. */
    if (sb.magic != LEANFS_MAGIC ||
        sb.data_blocks != LEANFS_DATA_BLOCKS ||
        sb.inode_table_sectors != INODE_TABLE_SECTORS ||
        sb.bitmap_sectors != BITMAP_SECTORS) {
        format();
    } else {
        ata_read_sectors(sb.inode_table_lba, (uint8_t)sb.inode_table_sectors, inode_table_buf);
        k_memcpy(inodes, inode_table_buf, sizeof(inodes));
        ata_read_sectors(sb.bitmap_lba, (uint8_t)sb.bitmap_sectors, bitmap);

        /* A filesystem whose root is not a directory is one nothing can
         * be resolved against - reformat rather than fail every path. */
        if (inodes[ROOT_INODE].type != LEANFS_TYPE_DIR) {
            klog_puts("[fs] leanfs root inode is not a directory - reformatting\n");
            format();
        }
    }

    klog_puts("[fs] leanfs ready: data_lba=0x");
    klog_put_hex32(sb.data_lba);
    klog_puts(" data_blocks=0x");
    klog_put_hex32(sb.data_blocks);
    klog_puts(" inodes=0x");
    klog_put_hex32(LEANFS_MAX_INODES);
    klog_putc('\n');
}

/* ---- blocks ---------------------------------------------------------- */

static int bitmap_test(uint32_t bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void bitmap_clear(uint32_t bit) {
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

/* M29: every block number this driver ever acts on either came straight
 * out of alloc_block (always < sb.data_blocks by construction) or off
 * disk (an inode's direct[]/indirect fields, or an indirect table's
 * entries) - the latter is trusted nowhere else, so a single bit flip
 * there would otherwise walk bitmap_clear/ata_read_sectors off the end of
 * the fixed-size `bitmap` array or into an arbitrary disk LBA. */
static int block_valid(uint32_t block) {
    return block < sb.data_blocks;
}

static int alloc_block(void) {
    for (uint32_t i = 0; i < sb.data_blocks; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            return (int)i;
        }
    }
    return -1;
}

static int inode_valid(int idx) {
    return idx >= 0 && idx < LEANFS_MAX_INODES && inodes[idx].type != LEANFS_TYPE_FREE;
}

static int find_free_inode(void) {
    for (int i = 0; i < LEANFS_MAX_INODES; i++) {
        if (inodes[i].type == LEANFS_TYPE_FREE) {
            return i;
        }
    }
    return -1;
}

/* Frees every block currently backing inode (per its *current* size and
 * on-disk indirect table, if any) - used before an overwrite reallocates
 * a fresh set. Does not touch inode->size/direct/indirect themselves. */
static void free_inode_blocks(leanfs_inode_t *inode) {
    uint32_t nblocks = (inode->size + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
    uint32_t indirect_table[LEANFS_INDIRECT_POINTERS];
    int have_indirect = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        if (b < LEANFS_DIRECT_BLOCKS) {
            if (block_valid(inode->direct[b])) {
                bitmap_clear(inode->direct[b]);
            }
        } else {
            if (!have_indirect) {
                if (!block_valid(inode->indirect)) {
                    break; /* corrupted inode - nothing past here is trustworthy either */
                }
                ata_read_sectors(sb.data_lba + inode->indirect, 1, (uint8_t *)indirect_table);
                have_indirect = 1;
            }
            uint32_t block = indirect_table[b - LEANFS_DIRECT_BLOCKS];
            if (block_valid(block)) {
                bitmap_clear(block);
            }
        }
    }
    if (nblocks > LEANFS_DIRECT_BLOCKS && block_valid(inode->indirect)) {
        bitmap_clear(inode->indirect);
    }
}

/* M53: the read/write halves that used to be inline in leanfs_read and
 * leanfs_write, lifted out so a *directory* can use the exact same block
 * allocation, indirect-table and bounds handling a file gets. That reuse
 * is what makes "a directory is a file whose contents are records" true
 * of the code and not only of the header comment. */
static int64_t inode_read_data(int idx, void *buf, size_t maxlen) {
    leanfs_inode_t *inode = &inodes[idx];
    size_t to_copy = inode->size < maxlen ? inode->size : maxlen;
    size_t copied = 0;
    uint8_t block_buf[LEANFS_BLOCK_SIZE];
    uint32_t indirect_table[LEANFS_INDIRECT_POINTERS];
    int have_indirect = 0;
    for (uint32_t b = 0; copied < to_copy; b++) {
        uint32_t block;
        if (b < LEANFS_DIRECT_BLOCKS) {
            block = inode->direct[b];
        } else {
            if (!have_indirect) {
                if (!block_valid(inode->indirect)) {
                    return -1; /* corrupted inode - fail the read rather than trust an out-of-range LBA */
                }
                ata_read_sectors(sb.data_lba + inode->indirect, 1, (uint8_t *)indirect_table);
                have_indirect = 1;
            }
            block = indirect_table[b - LEANFS_DIRECT_BLOCKS];
        }
        if (!block_valid(block)) {
            return -1; /* corrupted inode - see block_valid's own comment */
        }
        ata_read_sectors(sb.data_lba + block, 1, block_buf);
        size_t chunk = to_copy - copied;
        if (chunk > LEANFS_BLOCK_SIZE) {
            chunk = LEANFS_BLOCK_SIZE;
        }
        k_memcpy((uint8_t *)buf + copied, block_buf, chunk);
        copied += chunk;
    }
    return (int64_t)inode->size;
}

/* Replaces an inode's entire contents. Frees whatever backed it first -
 * a fresh write always gets a fresh set of blocks, kept simple rather
 * than reusing them in place. Leaves the inode's own type alone, so this
 * serves files and directories identically. Does not save the tables;
 * the caller does that once, after whatever else it also changed. */
static int inode_write_data(int idx, const void *buf, size_t len) {
    if (len > LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    leanfs_inode_t *inode = &inodes[idx];
    free_inode_blocks(inode);

    uint32_t needed_blocks = (uint32_t)((len + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE);
    uint32_t indirect_table[LEANFS_INDIRECT_POINTERS];
    int indirect_block = -1;

    if (needed_blocks > LEANFS_DIRECT_BLOCKS) {
        indirect_block = alloc_block();
        if (indirect_block < 0) {
            return -1;
        }
    }

    uint32_t allocated = 0;
    for (uint32_t b = 0; b < needed_blocks; b++) {
        int blk = alloc_block();
        if (blk < 0) {
            for (uint32_t j = 0; j < allocated; j++) {
                bitmap_clear(j < LEANFS_DIRECT_BLOCKS ? inode->direct[j] : indirect_table[j - LEANFS_DIRECT_BLOCKS]);
            }
            if (indirect_block >= 0) {
                bitmap_clear((uint32_t)indirect_block);
            }
            return -1;
        }
        if (b < LEANFS_DIRECT_BLOCKS) {
            inode->direct[b] = (uint32_t)blk;
        } else {
            indirect_table[b - LEANFS_DIRECT_BLOCKS] = (uint32_t)blk;
        }
        allocated++;
    }

    inode->indirect = (indirect_block >= 0) ? (uint32_t)indirect_block : 0;
    inode->size = (uint32_t)len;

    if (indirect_block >= 0) {
        uint8_t table_buf[LEANFS_BLOCK_SIZE];
        k_memset(table_buf, 0, sizeof(table_buf));
        k_memcpy(table_buf, indirect_table, (needed_blocks - LEANFS_DIRECT_BLOCKS) * sizeof(uint32_t));
        ata_write_sectors(sb.data_lba + (uint32_t)indirect_block, 1, table_buf);
    }

    size_t written = 0;
    uint8_t block_buf[LEANFS_BLOCK_SIZE];
    for (uint32_t b = 0; b < needed_blocks; b++) {
        uint32_t block = (b < LEANFS_DIRECT_BLOCKS) ? inode->direct[b] : indirect_table[b - LEANFS_DIRECT_BLOCKS];
        size_t chunk = len - written;
        if (chunk > LEANFS_BLOCK_SIZE) {
            chunk = LEANFS_BLOCK_SIZE;
        }
        k_memset(block_buf, 0, sizeof(block_buf));
        k_memcpy(block_buf, (const uint8_t *)buf + written, chunk);
        ata_write_sectors(sb.data_lba + block, 1, block_buf);
        written += chunk;
    }
    return 0;
}

/* ---- directories ----------------------------------------------------- */

/* Unpacks a directory's records into dirent_scratch and returns how many
 * slots it holds (including free ones, which keeps a slot's index stable
 * across a load/modify/store round trip). -1 if idx isn't a directory or
 * its contents are unreadable. */
static int dir_load(int idx) {
    if (!inode_valid(idx) || inodes[idx].type != LEANFS_TYPE_DIR) {
        return -1;
    }
    uint32_t bytes = inodes[idx].size;
    if (bytes > sizeof(dirent_scratch)) {
        return -1; /* more records than there are inodes to name - corrupt */
    }
    k_memset(dirent_scratch, 0, sizeof(dirent_scratch));
    if (bytes > 0 && inode_read_data(idx, dirent_scratch, bytes) < 0) {
        return -1;
    }
    return (int)(bytes / sizeof(leanfs_dirent_t));
}

static int dir_store(int idx, int count) {
    return inode_write_data(idx, dirent_scratch, (size_t)count * sizeof(leanfs_dirent_t));
}

/* The inode `name` refers to inside the directory `dir`, or -1. */
static int dir_lookup(int dir, const char *name) {
    int count = dir_load(dir);
    if (count < 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (dirent_scratch[i].name[0] && k_strcmp(dirent_scratch[i].name, name) == 0) {
            return (int)dirent_scratch[i].inode;
        }
    }
    return -1;
}

/* Adds one record. Reuses a slot left free by a removal before growing
 * the directory, so a create/remove cycle doesn't make a directory grow
 * without bound. Returns 0 or -1. */
static int dir_add(int dir, const char *name, int inode_idx) {
    int count = dir_load(dir);
    if (count < 0) {
        return -1;
    }
    int slot = -1;
    for (int i = 0; i < count; i++) {
        if (!dirent_scratch[i].name[0]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (count >= LEANFS_MAX_DIRENTS) {
            return -1;
        }
        slot = count++;
    }
    k_memset(&dirent_scratch[slot], 0, sizeof(dirent_scratch[slot]));
    k_strlcpy(dirent_scratch[slot].name, name, sizeof(dirent_scratch[slot].name));
    dirent_scratch[slot].inode = (uint32_t)inode_idx;
    return dir_store(dir, count);
}

/* ---- paths ----------------------------------------------------------- */

/* Copies the next '/'-delimited component of *p into out (at most
 * LEANFS_MAX_NAME chars plus NUL) and advances *p past it. Returns 1 if a
 * component was taken, 0 at the end of the path, -1 for a malformed one -
 * an empty component ("//" or a trailing slash on a non-root path), one
 * that is too long, or "." / ".." which this format deliberately does not
 * store and therefore cannot honestly resolve. */
static int next_component(const char **p, char *out) {
    const char *s = *p;
    if (*s == '\0') {
        return 0;
    }
    int n = 0;
    while (*s && *s != '/') {
        if (n >= LEANFS_MAX_NAME) {
            return -1;
        }
        out[n++] = *s++;
    }
    out[n] = '\0';
    if (n == 0) {
        return -1; /* "//" or a trailing '/' */
    }
    if (out[0] == '.' && (out[1] == '\0' || (out[1] == '.' && out[2] == '\0'))) {
        return -1; /* no "." or ".." on disk to resolve against - see the header */
    }
    if (*s == '/') {
        s++;
        if (*s == '\0') {
            return -1; /* trailing slash - "/bin/" is not a path this resolver accepts */
        }
    }
    *p = s;
    return 1;
}

/* Resolves an absolute path to an inode index, or -1.
 *
 * Deliberately no "." or ".." support: neither is stored on disk, and
 * synthesizing them would mean either walking a parent pointer this
 * format does not have or rewriting the path textually, which is the kind
 * of near-correct shortcut that turns into an escape from the root. The
 * file manager's own ".." is a caller-side string operation on a path it
 * already holds, which is honest about being exactly that. */
static int resolve(const char *path) {
    if (!path || path[0] != '/') {
        return -1;
    }
    if (path[1] == '\0') {
        return ROOT_INODE;
    }
    const char *p = path + 1;
    int at = ROOT_INODE;
    char comp[LEANFS_MAX_NAME + 1];
    int rc;
    while ((rc = next_component(&p, comp)) == 1) {
        if (inodes[at].type != LEANFS_TYPE_DIR) {
            return -1; /* tried to walk through a regular file */
        }
        at = dir_lookup(at, comp);
        if (!inode_valid(at)) {
            return -1;
        }
    }
    return rc < 0 ? -1 : at;
}

/* Splits an absolute path into its parent directory's inode and the leaf
 * name. Returns 0 with *out_parent and out_leaf filled, or -1. The root
 * itself has no parent and is refused. */
static int resolve_parent(const char *path, int *out_parent, char *out_leaf) {
    if (!path || path[0] != '/' || path[1] == '\0') {
        return -1;
    }
    /* The leaf is whatever follows the last '/'. Finding it first means
     * the walk below only ever has to handle interior components, which
     * is what keeps next_component's rules ("no empty, no trailing
     * slash") uniform. */
    const char *last = path;
    for (const char *s = path; *s; s++) {
        if (*s == '/') {
            last = s;
        }
    }
    if (last[1] == '\0') {
        return -1; /* trailing slash */
    }
    int n = 0;
    for (const char *s = last + 1; *s; s++) {
        if (n >= LEANFS_MAX_NAME) {
            return -1;
        }
        out_leaf[n++] = *s;
    }
    out_leaf[n] = '\0';
    if (out_leaf[0] == '.' && (out_leaf[1] == '\0' || (out_leaf[1] == '.' && out_leaf[2] == '\0'))) {
        return -1;
    }

    int parent;
    if (last == path) {
        parent = ROOT_INODE; /* "/name" */
    } else {
        char dir_path[LEANFS_MAX_PATH];
        size_t dir_len = (size_t)(last - path);
        if (dir_len >= sizeof(dir_path)) {
            return -1;
        }
        k_memcpy(dir_path, path, dir_len);
        dir_path[dir_len] = '\0';
        parent = resolve(dir_path);
    }
    if (!inode_valid(parent) || inodes[parent].type != LEANFS_TYPE_DIR) {
        return -1;
    }
    *out_parent = parent;
    return 0;
}

/* ---- public API ------------------------------------------------------ */

int leanfs_exists(const char *path) {
    return inode_valid(resolve(path));
}

int leanfs_is_dir(const char *path) {
    int idx = resolve(path);
    return inode_valid(idx) && inodes[idx].type == LEANFS_TYPE_DIR;
}

int64_t leanfs_read(const char *path, void *buf, size_t maxlen) {
    int idx = resolve(path);
    if (!inode_valid(idx) || inodes[idx].type != LEANFS_TYPE_FILE) {
        return -1;
    }
    return inode_read_data(idx, buf, maxlen);
}

int leanfs_write(const char *path, const void *buf, size_t len) {
    if (len > LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return -1;
    }

    int idx = dir_lookup(parent, leaf);
    if (inode_valid(idx)) {
        if (inodes[idx].type != LEANFS_TYPE_FILE) {
            return -1; /* overwriting a directory with a file is not a thing this format offers */
        }
    } else {
        idx = find_free_inode();
        if (idx < 0) {
            return -1;
        }
        k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
        inodes[idx].type = LEANFS_TYPE_FILE;
        /* The record goes in *before* the contents, deliberately: dir_add
         * uses the same block machinery inode_write_data does, and doing
         * it afterwards would mean a failure to grow the directory left a
         * fully written file that nothing could name. */
        if (dir_add(parent, leaf, idx) != 0) {
            inodes[idx].type = LEANFS_TYPE_FREE;
            return -1;
        }
    }

    if (inode_write_data(idx, buf, len) != 0) {
        return -1;
    }
    save_inodes();
    save_bitmap();
    return 0;
}

int leanfs_mkdir(const char *path) {
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return -1;
    }
    if (inode_valid(dir_lookup(parent, leaf))) {
        return -1; /* already there, of either kind - see the header on why this isn't a no-op */
    }
    int idx = find_free_inode();
    if (idx < 0) {
        return -1;
    }
    k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
    inodes[idx].type = LEANFS_TYPE_DIR;
    inodes[idx].size = 0;
    if (dir_add(parent, leaf, idx) != 0) {
        inodes[idx].type = LEANFS_TYPE_FREE;
        return -1;
    }
    save_inodes();
    save_bitmap();
    return 0;
}

size_t leanfs_list(const char *path, char *buf, size_t maxlen) {
    int idx = resolve(path);
    if (!inode_valid(idx) || inodes[idx].type != LEANFS_TYPE_DIR) {
        return 0;
    }
    int count = dir_load(idx);
    if (count < 0) {
        return 0;
    }
    /* dir_load's results live in the shared scratch buffer, and the
     * is-it-a-directory question below re-reads `inodes` rather than the
     * scratch - so nothing here can invalidate what it is iterating. */
    size_t written = 0;
    for (int i = 0; i < count; i++) {
        if (!dirent_scratch[i].name[0]) {
            continue;
        }
        int child = (int)dirent_scratch[i].inode;
        int is_dir = inode_valid(child) && inodes[child].type == LEANFS_TYPE_DIR;
        size_t name_len = k_strlen(dirent_scratch[i].name);
        size_t need = name_len + (is_dir ? 1u : 0u) + 1u; /* name + optional '/' + '\n' */
        if (written + need > maxlen) {
            break;
        }
        k_memcpy(buf + written, dirent_scratch[i].name, name_len);
        written += name_len;
        if (is_dir) {
            buf[written++] = '/';
        }
        buf[written++] = '\n';
    }
    return written;
}
