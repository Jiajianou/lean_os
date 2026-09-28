#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../kernel/file_system/leanfs_format.h"

static FILE *img;
static leanfs_superblock_t sb;
static leanfs_inode_t inodes[LEANFS_MAX_INODES];
static uint8_t bitmap[BITMAP_BLOCKS * LEANFS_BLOCK_SIZE];
static uint8_t directory_block[LEANFS_BLOCK_SIZE];

static void die(const char *message) {
    fprintf(stderr, "leanfs-put: %s\n", message);
    exit(1);
}

static void pread_at(uint64_t byte_off, void *buffer, size_t length) {
    if (fseeko(img, (off_t)byte_off, SEEK_SET) != 0 || fread(buffer, 1, length, img) != length) {
        die("short read from disk image - is it fully built (`make all`)?");
    }
}

static void pwrite_at(uint64_t byte_off, const void *buffer, size_t length) {
    if (fseeko(img, (off_t)byte_off, SEEK_SET) != 0 || fwrite(buffer, 1, length, img) != length) {
        die("short write to disk image");
    }
}

static uint64_t block_bytes(uint32_t block) {
    return (uint64_t)block * LEANFS_BLOCK_SIZE;
}

static void read_block(uint32_t block, void *buffer) {
    pread_at(block_bytes(sb.data_block + block), buffer, LEANFS_BLOCK_SIZE);
}

static void write_block(uint32_t block, const void *buffer) {
    pwrite_at(block_bytes(sb.data_block + block), buffer, LEANFS_BLOCK_SIZE);
}

static int bitmap_test(uint32_t bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}
static void bitmap_set(uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}
static void bitmap_clear(uint32_t bit) {
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

static uint32_t alloc_hint;

static uint32_t alloc_block(void) {
    for (int pass = 0; pass < 2; pass++) {
        uint32_t start = (pass == 0) ? alloc_hint : 0;
        for (uint32_t i = start; i < sb.data_blocks; i++) {
            if (!bitmap_test(i)) {
                bitmap_set(i);
                alloc_hint = i + 1;
                return i;
            }
        }
    }
    die("filesystem full (no free data blocks)");
    return 0;
}

static int block_present(uint32_t block) {
    return block != 0 && block < sb.data_blocks;
}

static uint32_t map_block(leanfs_inode_t *inode, uint32_t logical) {
    if (logical >= (uint32_t)LEANFS_MAX_FILE_BLOCKS) {
        die("file needs more blocks than leanfs can address");
    }
    if (logical < LEANFS_DIRECT_BLOCKS) {
        if (!block_present(inode->direct[logical])) {
            inode->direct[logical] = alloc_block();
        }
        return inode->direct[logical];
    }

    uint32_t rel = logical - LEANFS_DIRECT_BLOCKS;
    int use_dindirect = rel >= (uint32_t)LEANFS_INDIRECT_POINTERS;
    uint32_t table[LEANFS_INDIRECT_POINTERS];
    uint32_t slot;
    uint32_t table_block;

    if (!use_dindirect) {
        slot = rel;
        if (!block_present(inode->indirect)) {
            inode->indirect = alloc_block();
            memset(table, 0, sizeof(table));
            write_block(inode->indirect, table);
        }
        table_block = inode->indirect;
    } else {
        uint32_t drel = rel - (uint32_t)LEANFS_INDIRECT_POINTERS;
        uint32_t outer = drel / (uint32_t)LEANFS_INDIRECT_POINTERS;
        slot = drel % (uint32_t)LEANFS_INDIRECT_POINTERS;
        if (!block_present(inode->dindirect)) {
            inode->dindirect = alloc_block();
            memset(table, 0, sizeof(table));
            write_block(inode->dindirect, table);
        }
        read_block(inode->dindirect, table);
        if (!block_present(table[outer])) {
            uint32_t inner = alloc_block();
            table[outer] = inner;
            write_block(inode->dindirect, table);
            uint32_t empty[LEANFS_INDIRECT_POINTERS];
            memset(empty, 0, sizeof(empty));
            write_block(inner, empty);
        }
        table_block = table[outer];
    }

    read_block(table_block, table);
    if (!block_present(table[slot])) {
        table[slot] = alloc_block();
        write_block(table_block, table);
    }
    return table[slot];
}

static void free_inode_blocks(leanfs_inode_t *inode) {
    uint32_t nblocks = (inode->size + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
    uint32_t table[LEANFS_INDIRECT_POINTERS];

    for (uint32_t b = 0; b < nblocks && b < LEANFS_DIRECT_BLOCKS; b++) {
        if (block_present(inode->direct[b])) {
            bitmap_clear(inode->direct[b]);
        }
    }
    if (block_present(inode->indirect)) {
        read_block(inode->indirect, table);
        for (int i = 0; i < LEANFS_INDIRECT_POINTERS; i++) {
            if (block_present(table[i])) {
                bitmap_clear(table[i]);
            }
        }
        bitmap_clear(inode->indirect);
    }
    if (block_present(inode->dindirect)) {
        uint32_t outer_table[LEANFS_INDIRECT_POINTERS];
        read_block(inode->dindirect, outer_table);
        for (int o = 0; o < LEANFS_INDIRECT_POINTERS; o++) {
            if (!block_present(outer_table[o])) {
                continue;
            }
            read_block(outer_table[o], table);
            for (int i = 0; i < LEANFS_INDIRECT_POINTERS; i++) {
                if (block_present(table[i])) {
                    bitmap_clear(table[i]);
                }
            }
            bitmap_clear(outer_table[o]);
        }
        bitmap_clear(inode->dindirect);
    }
    memset(inode->direct, 0, sizeof(inode->direct));
    inode->indirect = 0;
    inode->dindirect = 0;
    inode->size = 0;
}

static void inode_write_all(leanfs_inode_t *inode, const uint8_t *data, size_t length,
                            uint32_t mtime) {
    free_inode_blocks(inode);
    uint8_t block_buffer[LEANFS_BLOCK_SIZE];
    size_t done = 0;
    for (uint32_t b = 0; done < length; b++) {
        uint32_t phys = map_block(inode, b);
        size_t chunk = length - done;
        if (chunk > LEANFS_BLOCK_SIZE) {
            chunk = LEANFS_BLOCK_SIZE;
        }
        memset(block_buffer, 0, sizeof(block_buffer));
        memcpy(block_buffer, data + done, chunk);
        write_block(phys, block_buffer);
        done += chunk;
    }
    inode->size = (uint32_t)length;
    inode->mtime = mtime;
}

static leanfs_dirent_t *directory_rec(uint32_t off) {
    return (leanfs_dirent_t *)(void *)(directory_block + off);
}

static int directory_block_valid(void) {
    uint32_t off = 0;
    while (off < LEANFS_BLOCK_SIZE) {
        if (off + LEANFS_DIRENT_HEADER > LEANFS_BLOCK_SIZE) {
            return 0;
        }
        leanfs_dirent_t *r = directory_rec(off);
        if (r->rec_length < LEANFS_DIRENT_HEADER ||
            (r->rec_length % LEANFS_DIRENT_ALIGN) != 0 ||
            off + r->rec_length > LEANFS_BLOCK_SIZE ||
            LEANFS_DIRENT_NEED(r->name_length) > r->rec_length) {
            return 0;
        }
        off += r->rec_length;
    }
    return off == LEANFS_BLOCK_SIZE;
}

static void directory_block_init(void) {
    memset(directory_block, 0, LEANFS_BLOCK_SIZE);
    directory_rec(0)->rec_length = (uint16_t)LEANFS_BLOCK_SIZE;
}

static uint32_t directory_nblocks(const leanfs_inode_t *directory) {
    return directory->size / LEANFS_BLOCK_SIZE;
}

static void directory_block_read(leanfs_inode_t *directory, uint32_t logical) {
    read_block(map_block(directory, logical), directory_block);
    if (!directory_block_valid()) {
        die("a directory block does not parse - corrupt image");
    }
}

static int32_t directory_block_find(const char *name, uint32_t name_length) {
    uint32_t off = 0;
    while (off < LEANFS_BLOCK_SIZE) {
        leanfs_dirent_t *r = directory_rec(off);
        if (r->inode != 0 && r->name_length == name_length &&
            memcmp(directory_block + off + LEANFS_DIRENT_HEADER, name, name_length) == 0) {
            return (int32_t)off;
        }
        off += r->rec_length;
    }
    return -1;
}

static int directory_block_place(const char *name, uint32_t name_length, int inode_index, uint32_t type) {
    uint32_t need = LEANFS_DIRENT_NEED(name_length);
    for (int pass = 0; pass < 2; pass++) {
        uint32_t off = 0;
        while (off < LEANFS_BLOCK_SIZE) {
            leanfs_dirent_t *r = directory_rec(off);
            uint32_t place_at = 0;
            uint32_t place_length = 0;
            if (pass == 0 && r->inode == 0 && r->rec_length >= need) {
                place_at = off;
                place_length = r->rec_length;
            } else if (pass == 1 && r->inode != 0) {
                uint32_t used = LEANFS_DIRENT_NEED(r->name_length);
                if (r->rec_length >= used + need) {
                    place_length = r->rec_length - used;
                    r->rec_length = (uint16_t)used;
                    place_at = off + used;
                }
            }
            if (place_length > 0) {
                leanfs_dirent_t *n = directory_rec(place_at);
                n->inode = (uint32_t)inode_index;
                n->rec_length = (uint16_t)place_length;
                n->name_length = (uint8_t)name_length;
                n->type = (uint8_t)type;
                memcpy(directory_block + place_at + LEANFS_DIRENT_HEADER, name, name_length);
                return 1;
            }
            off += r->rec_length;
        }
    }
    return 0;
}

static int directory_lookup(leanfs_inode_t *directory, const char *name) {
    if (directory->type != LEANFS_TYPE_DIRECTORY) {
        die("a path component exists but is not a directory");
    }
    uint32_t name_length = (uint32_t)strlen(name);
    uint32_t blocks = directory_nblocks(directory);
    for (uint32_t b = 0; b < blocks; b++) {
        directory_block_read(directory, b);
        int32_t off = directory_block_find(name, name_length);
        if (off >= 0) {
            return (int)directory_rec((uint32_t)off)->inode;
        }
    }
    return -1;
}

static uint32_t inode_hint;

static int alloc_inode(uint32_t type) {
    for (int pass = 0; pass < 2; pass++) {
        uint32_t start = (pass == 0) ? inode_hint : 0;
        for (uint32_t i = start; i < LEANFS_MAX_INODES; i++) {
            if (inodes[i].type == LEANFS_TYPE_FREE) {
                memset(&inodes[i], 0, sizeof(inodes[i]));
                inodes[i].type = type;
                inodes[i].nlink = 1;
                inode_hint = i + 1;
                return (int)i;
            }
        }
    }
    fprintf(stderr, "leanfs-put: no free inode (this filesystem holds at most %d)\n",
            LEANFS_MAX_INODES);
    exit(1);
}

static int directory_repoint(leanfs_inode_t *directory, const char *name, int inode_index) {
    uint32_t name_length = (uint32_t)strlen(name);
    uint32_t blocks = directory_nblocks(directory);
    for (uint32_t b = 0; b < blocks; b++) {
        directory_block_read(directory, b);
        int32_t off = directory_block_find(name, name_length);
        if (off >= 0) {
            leanfs_dirent_t *r = directory_rec((uint32_t)off);
            r->inode = (uint32_t)inode_index;
            r->type = (uint8_t)inodes[inode_index].type;
            write_block(map_block(directory, b), directory_block);
            return 1;
        }
    }
    return 0;
}

static void directory_add(leanfs_inode_t *directory, const char *name, int inode_index) {
    if (directory->type != LEANFS_TYPE_DIRECTORY) {
        die("cannot add an entry to something that is not a directory");
    }
    uint32_t name_length = (uint32_t)strlen(name);
    if (name_length == 0 || name_length > LEANFS_MAX_NAME) {
        die("empty or over-long name");
    }
    uint32_t type = inodes[inode_index].type;

    uint32_t blocks = directory_nblocks(directory);
    if (blocks > 0) {
        directory_block_read(directory, blocks - 1);
        if (directory_block_place(name, name_length, inode_index, type)) {
            write_block(map_block(directory, blocks - 1), directory_block);
            return;
        }
    }
    for (uint32_t b = 0; b + 1 < blocks; b++) {
        directory_block_read(directory, b);
        if (directory_block_place(name, name_length, inode_index, type)) {
            write_block(map_block(directory, b), directory_block);
            return;
        }
    }
    directory_block_init();
    if (!directory_block_place(name, name_length, inode_index, type)) {
        die("a name did not fit an empty directory block - impossible");
    }
    write_block(map_block(directory, blocks), directory_block);
    directory->size += LEANFS_BLOCK_SIZE;
}

static int resolve_parent(const char *path, char *leaf_out) {
    if (path[0] != '/') {
        die("leanfs paths are absolute - give a name like /bin/yourprog");
    }
    int at = ROOT_INODE;
    const char *p = path + 1;
    for (;;) {
        const char *slash = strchr(p, '/');
        if (!slash) {
            break;
        }
        size_t n = (size_t)(slash - p);
        if (n == 0 || n > LEANFS_MAX_NAME) {
            die("empty or over-long path component");
        }
        char comp[LEANFS_MAX_NAME + 1];
        memcpy(comp, p, n);
        comp[n] = '\0';
        int next = directory_lookup(&inodes[at], comp);
        if (next < 0) {
            next = alloc_inode(LEANFS_TYPE_DIRECTORY);
            directory_add(&inodes[at], comp, next);
            fprintf(stderr, "leanfs-put: created directory %.*s\n",
                    (int)(slash - path), path);
        }
        at = next;
        p = slash + 1;
    }
    if (*p == '\0' || strlen(p) > LEANFS_MAX_NAME) {
        die("empty or over-long final path component");
    }
    snprintf(leaf_out, LEANFS_MAX_NAME + 1, "%s", p);
    return at;
}

static void inode_write_stream(leanfs_inode_t *inode, FILE *source, uint64_t length,
                               uint32_t mtime, const char *what, uint32_t *hash) {
    free_inode_blocks(inode);
    if (length > LEANFS_MAX_FILE_SIZE) {
        fprintf(stderr, "leanfs-put: %s is %llu bytes; leanfs holds at most %u\n",
                what, (unsigned long long)length, (unsigned)LEANFS_MAX_FILE_SIZE);
        exit(1);
    }
    uint8_t block_buffer[LEANFS_BLOCK_SIZE];
    uint64_t done = 0;
    for (uint32_t b = 0; done < length; b++) {
        size_t chunk = (size_t)((length - done > LEANFS_BLOCK_SIZE)
                                ? LEANFS_BLOCK_SIZE : (length - done));
        memset(block_buffer, 0, sizeof(block_buffer));
        if (fread(block_buffer, 1, chunk, source) != chunk) {
            fprintf(stderr, "leanfs-put: short read on %s\n", what);
            exit(1);
        }
        if (hash) {
            *hash = leanfs_fnv1a(*hash, block_buffer, chunk);
        }
        write_block(map_block(inode, b), block_buffer);
        done += chunk;
    }
    inode->size = (uint32_t)length;
    inode->mtime = mtime;
}

#define HL_SLOTS 262144u
typedef struct {
    int      used;
    uint64_t dev;
    uint64_t ino;
    int      inode;
} hardlink_slot_t;
static hardlink_slot_t hardlinks[HL_SLOTS];

static uint32_t hl_hash(uint64_t dev, uint64_t ino) {
    uint64_t h = dev * 0x9E3779B97F4A7C15ull ^ (ino + 0x165667B19E3779F9ull);
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return (uint32_t)(h & (HL_SLOTS - 1));
}

static int hl_lookup(uint64_t dev, uint64_t ino) {
    uint32_t i = hl_hash(dev, ino);
    for (uint32_t probe = 0; probe < HL_SLOTS; probe++) {
        hardlink_slot_t *s = &hardlinks[(i + probe) & (HL_SLOTS - 1)];
        if (!s->used) {
            return -1;
        }
        if (s->dev == dev && s->ino == ino) {
            return s->inode;
        }
    }
    return -1;
}

static void hl_remember(uint64_t dev, uint64_t ino, int inode) {
    uint32_t i = hl_hash(dev, ino);
    for (uint32_t probe = 0; probe < HL_SLOTS; probe++) {
        hardlink_slot_t *s = &hardlinks[(i + probe) & (HL_SLOTS - 1)];
        if (!s->used) {
            s->used = 1;
            s->dev = dev;
            s->ino = ino;
            s->inode = inode;
            return;
        }
    }
    die("the hard-link table filled - more distinct files than leanfs has inodes");
}

static struct {
    uint64_t files;
    uint64_t dirs;
    uint64_t links;
    uint64_t hardlinks;
    uint64_t names;
    uint64_t bytes;
    uint64_t skipped;
    uint32_t deepest;
    uint32_t hash;
} tally;

static uint32_t name_hash_start(const char *leanfs_path, const char *tree_root) {
    size_t skip = strlen(tree_root);
    const char *rel = leanfs_path + skip;
    while (*rel == '/') {
        rel++;
    }
    return leanfs_fnv1a(LEANFS_FNV1A_INIT, rel, strlen(rel));
}

static const char *tree_root;
static uint32_t tree_mtime;

#define TREE_MAX_DEPTH 48

static void put_tree(const char *host_directory, int parent, const char *at, uint32_t depth) {
    if (depth > TREE_MAX_DEPTH) {
        fprintf(stderr, "leanfs-put: %s is more than %d directories deep\n",
                at, TREE_MAX_DEPTH);
        exit(1);
    }
    if (depth >= 2 && depth > tally.deepest) {
        tally.deepest = depth;
    }

    struct dirent **names = NULL;
    int n = scandir(host_directory, &names, NULL, alphasort);
    if (n < 0) {
        fprintf(stderr, "leanfs-put: cannot read directory %s: %s\n",
                host_directory, strerror(errno));
        exit(1);
    }

    for (int i = 0; i < n; i++) {
        const char *name = names[i]->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            free(names[i]);
            continue;
        }
        if (strlen(name) > LEANFS_MAX_NAME) {
            fprintf(stderr, "leanfs-put: %s/%s: name is longer than leanfs's %d bytes\n",
                    host_directory, name, LEANFS_MAX_NAME);
            exit(1);
        }

        char host_path[LEANFS_MAX_PATH];
        char leanfs_path[LEANFS_MAX_PATH];
        if (snprintf(host_path, sizeof(host_path), "%s/%s", host_directory, name) >=
                (int)sizeof(host_path) ||
            snprintf(leanfs_path, sizeof(leanfs_path), "%s/%s", at, name) >=
                (int)sizeof(leanfs_path)) {
            fprintf(stderr, "leanfs-put: %s/%s: path longer than %d bytes\n",
                    host_directory, name, LEANFS_MAX_PATH);
            exit(1);
        }

        struct stat st;
        if (lstat(host_path, &st) != 0) {
            fprintf(stderr, "leanfs-put: cannot stat %s: %s\n", host_path, strerror(errno));
            exit(1);
        }
        uint32_t mtime = (uint32_t)st.st_mtime;

        int prior = directory_lookup(&inodes[parent], name);

        if (S_ISDIR(st.st_mode)) {
            int idx = prior;
            if (idx >= 0) {
                if (inodes[idx].type != LEANFS_TYPE_DIRECTORY) {
                    die("re-putting a tree where a file became a directory");
                }
                inodes[idx].mtime = mtime;
            } else {
                idx = alloc_inode(LEANFS_TYPE_DIRECTORY);
                inodes[idx].mtime = mtime;
                directory_add(&inodes[parent], name, idx);
            }
            tally.dirs++;
            put_tree(host_path, idx, leanfs_path, depth + 1);
        } else if (S_ISLNK(st.st_mode)) {
            char target[LEANFS_MAX_PATH];
            ssize_t tlen = readlink(host_path, target, sizeof(target) - 1);
            if (tlen < 0) {
                fprintf(stderr, "leanfs-put: cannot read the link %s: %s\n",
                        host_path, strerror(errno));
                exit(1);
            }
            int idx = prior;
            if (idx >= 0) {
                if (inodes[idx].type != LEANFS_TYPE_LINK) {
                    die("re-putting a tree where a file became a symlink");
                }
                free_inode_blocks(&inodes[idx]);
            } else {
                idx = alloc_inode(LEANFS_TYPE_LINK);
                directory_add(&inodes[parent], name, idx);
            }
            inode_write_all(&inodes[idx], (const uint8_t *)target, (size_t)tlen, mtime);
            tally.links++;
            tally.hash += leanfs_fnv1a(name_hash_start(leanfs_path, tree_root),
                                       target, (size_t)tlen);
        } else if (S_ISREG(st.st_mode)) {
            int existing = -1;
            if (st.st_nlink > 1) {
                existing = hl_lookup((uint64_t)st.st_dev, (uint64_t)st.st_ino);
            }
            if (existing >= 0) {
                if (prior == existing) {
                } else if (prior >= 0) {
                    die("re-putting a tree whose hard links moved - rebuild the image");
                } else {
                    if (inodes[existing].nlink == 0xFFFFFFFFu) {
                        die("a file has more names than leanfs can count");
                    }
                    inodes[existing].nlink++;
                    directory_add(&inodes[parent], name, existing);
                }
                tally.hardlinks++;
                tally.names++;
                tally.bytes += (uint64_t)st.st_size;
                {
                    FILE *again = fopen(host_path, "rb");
                    if (!again) {
                        fprintf(stderr, "leanfs-put: cannot re-open %s: %s\n",
                                host_path, strerror(errno));
                        exit(1);
                    }
                    uint32_t h = name_hash_start(leanfs_path, tree_root);
                    uint8_t buffer[LEANFS_BLOCK_SIZE];
                    size_t got;
                    while ((got = fread(buffer, 1, sizeof(buffer), again)) > 0) {
                        h = leanfs_fnv1a(h, buffer, got);
                    }
                    fclose(again);
                    tally.hash += h;
                }
            } else {
                FILE *source = fopen(host_path, "rb");
                if (!source) {
                    fprintf(stderr, "leanfs-put: cannot open %s: %s\n",
                            host_path, strerror(errno));
                    exit(1);
                }
                int idx = prior;
                if (idx >= 0) {
                    if (inodes[idx].type != LEANFS_TYPE_FILE) {
                        die("re-putting a tree where a directory or link became a file");
                    }
                    free_inode_blocks(&inodes[idx]);
                } else {
                    idx = alloc_inode(LEANFS_TYPE_FILE);
                    directory_add(&inodes[parent], name, idx);
                }
                uint32_t h = name_hash_start(leanfs_path, tree_root);
                inode_write_stream(&inodes[idx], source, (uint64_t)st.st_size, mtime,
                                   host_path, &h);
                fclose(source);
                if (st.st_nlink > 1) {
                    hl_remember((uint64_t)st.st_dev, (uint64_t)st.st_ino, idx);
                }
                tally.files++;
                tally.names++;
                tally.bytes += (uint64_t)st.st_size;
                tally.hash += h;
            }
        } else {
            fprintf(stderr, "leanfs-put: skipping %s (not a file, directory or symlink)\n",
                    host_path);
            tally.skipped++;
        }
        free(names[i]);
    }
    free(names);
}

static void write_manifest(const char *at) {
    char text[1024];
    int length = snprintf(text, sizeof(text),
                       "leanfs-image-manifest 1\n"
                       "tree %s\n"
                       "dirs %llu\n"
                       "names %llu\n"
                       "links %llu\n"
                       "bytes %llu\n"
                       "depth %u\n"
                       "hash %u\n",
                       at,
                       (unsigned long long)tally.dirs,
                       (unsigned long long)tally.names,
                       (unsigned long long)tally.links,
                       (unsigned long long)tally.bytes,
                       tally.deepest,
                       tally.hash);
    if (length < 0 || length >= (int)sizeof(text)) {
        die("the manifest did not fit its buffer");
    }

    char leaf[LEANFS_MAX_NAME + 1];
    int parent = resolve_parent("/.image-manifest", leaf);
    int idx = directory_lookup(&inodes[parent], leaf);
    if (idx < 0) {
        idx = alloc_inode(LEANFS_TYPE_FILE);
        directory_add(&inodes[parent], leaf, idx);
    } else if (inodes[idx].type != LEANFS_TYPE_FILE) {
        die("/.image-manifest exists and is not a regular file");
    }
    inode_write_all(&inodes[idx], (const uint8_t *)text, (size_t)length, tree_mtime);
}

static void format_fresh(void) {
    memset(&sb, 0, sizeof(sb));
    sb.magic = LEANFS_MAGIC;
    sb.version = LEANFS_VERSION;
    sb.inode_table_block = LEANFS_START_BLOCK + 1;
    sb.inode_table_blocks = (uint32_t)INODE_TABLE_BLOCKS;
    sb.bitmap_block = sb.inode_table_block + (uint32_t)INODE_TABLE_BLOCKS;
    sb.bitmap_blocks_field = BITMAP_BLOCKS;
    sb.data_block = sb.bitmap_block + BITMAP_BLOCKS;
    sb.data_blocks = LEANFS_DATA_BLOCKS;
    sb.state = LEANFS_STATE_CLEAN;

    memset(inodes, 0, sizeof(inodes));
    memset(bitmap, 0, sizeof(bitmap));
    bitmap[0] |= 1u;

    inodes[ROOT_INODE].type = LEANFS_TYPE_DIRECTORY;
    inodes[ROOT_INODE].size = 0;
    inodes[ROOT_INODE].nlink = 1;
}

static void save_all(void) {
    uint8_t sb_buffer[LEANFS_BLOCK_SIZE];
    memset(sb_buffer, 0, sizeof(sb_buffer));
    memcpy(sb_buffer, &sb, sizeof(sb));
    pwrite_at(block_bytes(LEANFS_START_BLOCK), sb_buffer, sizeof(sb_buffer));

    static uint8_t table_buffer[INODE_TABLE_BLOCKS * LEANFS_BLOCK_SIZE];
    memset(table_buffer, 0, sizeof(table_buffer));
    memcpy(table_buffer, inodes, sizeof(inodes));
    pwrite_at(block_bytes(sb.inode_table_block), table_buffer, sizeof(table_buffer));

    pwrite_at(block_bytes(sb.bitmap_block), bitmap, sizeof(bitmap));
}

/* M194: an image whose machine stopped without shutting down can have
   committed transactions in its journal that never reached home. Writing into
   it here and leaving them would let the next boot's replay write them over
   what this tool put there, so they are replayed first, the same way the
   kernel does, and the journal is left empty. An image with no journal header
   has nothing to replay and is not written. */
static void journal_replay_into_image(void) {
    static uint8_t block[LEANFS_BLOCK_SIZE];
    static uint8_t descriptor[LEANFS_BLOCK_SIZE];
    uint64_t journal_bytes = (uint64_t)LEANFS_JOURNAL_START_LBA * LEANFS_SECTOR_SIZE;
    pread_at(journal_bytes, block, LEANFS_BLOCK_SIZE);
    leanfs_journal_header_t header;
    memcpy(&header, block, sizeof(header));
    if (header.magic != LEANFS_JOURNAL_HEADER_MAGIC || header.version != LEANFS_JOURNAL_VERSION ||
        header.checksum != leanfs_journal_header_checksum(&header) ||
        header.journal_blocks != LEANFS_JOURNAL_BLOCKS || header.start_block == 0 ||
        header.start_block >= LEANFS_JOURNAL_BLOCKS) {
        return;
    }
    uint32_t at = header.start_block;
    uint64_t sequence = header.start_sequence;
    uint32_t replayed = 0;
    for (;;) {
        if (at + 2 >= LEANFS_JOURNAL_BLOCKS) {
            break;
        }
        pread_at(journal_bytes + (uint64_t)at * LEANFS_BLOCK_SIZE, descriptor, LEANFS_BLOCK_SIZE);
        const leanfs_journal_descriptor_t *d = (const leanfs_journal_descriptor_t *)descriptor;
        if (d->magic != LEANFS_JOURNAL_DESCRIPTOR_MAGIC || d->index != 0 || d->sequence != sequence ||
            d->count == 0 || d->count > LEANFS_JOURNAL_BLOCKS) {
            break;
        }
        uint32_t count = d->count;
        uint32_t descriptors = leanfs_journal_descriptor_blocks(count);
        if (at + descriptors + count + 1 > LEANFS_JOURNAL_BLOCKS) {
            break;
        }
        uint32_t checksum = LEANFS_FNV1A_INIT;
        int intact = 1;
        for (uint32_t item = 0; item < descriptors + count; item++) {
            pread_at(journal_bytes + (uint64_t)(at + item) * LEANFS_BLOCK_SIZE, block, LEANFS_BLOCK_SIZE);
            if (item < descriptors) {
                const leanfs_journal_descriptor_t *dd = (const leanfs_journal_descriptor_t *)block;
                if (dd->magic != LEANFS_JOURNAL_DESCRIPTOR_MAGIC || dd->index != item ||
                    dd->sequence != sequence || dd->count != count) {
                    intact = 0;
                    break;
                }
            }
            checksum = leanfs_fnv1a(checksum, block, LEANFS_BLOCK_SIZE);
        }
        pread_at(journal_bytes + (uint64_t)(at + descriptors + count) * LEANFS_BLOCK_SIZE, block,
                 LEANFS_BLOCK_SIZE);
        const leanfs_journal_commit_t *c = (const leanfs_journal_commit_t *)block;
        if (!intact || c->magic != LEANFS_JOURNAL_COMMIT_MAGIC || c->sequence != sequence ||
            c->count != count || c->checksum != checksum) {
            break;
        }
        for (uint32_t item = 0; item < count; item++) {
            if (item % LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR == 0) {
                pread_at(journal_bytes +
                             (uint64_t)(at + item / LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR) * LEANFS_BLOCK_SIZE,
                         descriptor, LEANFS_BLOCK_SIZE);
            }
            uint32_t target = ((const leanfs_journal_descriptor_t *)descriptor)
                                  ->targets[item % LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR];
            if ((uint64_t)target * LEANFS_SECTORS_PER_BLOCK < LEANFS_START_LBA ||
                (uint64_t)target * LEANFS_SECTORS_PER_BLOCK >= LEANFS_JOURNAL_START_LBA) {
                die("a committed journal transaction names a block outside the filesystem");
            }
            pread_at(journal_bytes + (uint64_t)(at + descriptors + item) * LEANFS_BLOCK_SIZE, block,
                     LEANFS_BLOCK_SIZE);
            pwrite_at((uint64_t)target * LEANFS_BLOCK_SIZE, block, LEANFS_BLOCK_SIZE);
        }
        replayed++;
        at += descriptors + count + 1;
        sequence++;
    }
    memset(block, 0, sizeof(block));
    header.start_block = 1;
    header.start_sequence = sequence;
    header.checksum = leanfs_journal_header_checksum(&header);
    memcpy(block, &header, sizeof(header));
    pwrite_at(journal_bytes, block, LEANFS_SECTOR_SIZE);
    if (replayed) {
        fprintf(stderr, "leanfs-put: replayed %u committed journal transaction(s) left by a machine "
                        "that did not shut down\n", replayed);
    }
}

static void image_open(const char *image_path) {
    img = fopen(image_path, "r+b");
    if (!img) {
        die("could not open disk-image for read/write - run `make all` first");
    }
    journal_replay_into_image();

    uint8_t sb_buffer[LEANFS_BLOCK_SIZE];
    pread_at(block_bytes(LEANFS_START_BLOCK), sb_buffer, sizeof(sb_buffer));
    memcpy(&sb, sb_buffer, sizeof(sb));

    if (sb.magic == LEANFS_MAGIC) {
        if (sb.inode_table_blocks > INODE_TABLE_BLOCKS) {
            fprintf(stderr,
                    "leanfs-put: this image's inode table is %u blocks and this tool is built "
                    "for %zu.\n            The image predates this build's "
                    "kernel/file_system/leanfs_format.h - rebuild the image.\n",
                    sb.inode_table_blocks, (size_t)INODE_TABLE_BLOCKS);
            exit(1);
        }
        if (sb.bitmap_blocks_field > BITMAP_BLOCKS || sb.data_blocks > LEANFS_DATA_BLOCKS) {
            die("this image's data region is larger than this tool is built for - "
                "the image predates this build's kernel/file_system/leanfs_format.h");
        }
        static uint8_t table_buffer[INODE_TABLE_BLOCKS * LEANFS_BLOCK_SIZE];
        memset(table_buffer, 0, sizeof(table_buffer));
        pread_at(block_bytes(sb.inode_table_block), table_buffer,
                  (size_t)sb.inode_table_blocks * LEANFS_BLOCK_SIZE);
        memcpy(inodes, table_buffer, sizeof(inodes));
        memset(bitmap, 0, sizeof(bitmap));
        pread_at(block_bytes(sb.bitmap_block), bitmap,
                  (size_t)sb.bitmap_blocks_field * LEANFS_BLOCK_SIZE);
        if (inodes[ROOT_INODE].type != LEANFS_TYPE_DIRECTORY) {
            die("this image has a leanfs superblock but no root directory - corrupt");
        }
    } else if (sb.magic != 0 && sb.magic != 0xFFFFFFFFu) {
        fprintf(stderr,
                "leanfs-put: this image's leanfs magic is 0x%08X and this tool writes 0x%08X.\n"
                "            The on-disk format has moved - rebuild this tool against the\n"
                "            current kernel/file_system/leanfs_format.h, and rebuild the image.\n",
                sb.magic, (unsigned)LEANFS_MAGIC);
        exit(1);
    } else {
        format_fresh();
    }
}

static int resolve_directory(const char *path) {
    if (path[0] != '/') {
        die("leanfs paths are absolute - give a name like /gcc-15.1.0");
    }
    if (path[1] == '\0') {
        return ROOT_INODE;
    }
    char leaf[LEANFS_MAX_NAME + 1];
    int parent = resolve_parent(path, leaf);
    int idx = directory_lookup(&inodes[parent], leaf);
    if (idx < 0) {
        idx = alloc_inode(LEANFS_TYPE_DIRECTORY);
        directory_add(&inodes[parent], leaf, idx);
    } else if (inodes[idx].type != LEANFS_TYPE_DIRECTORY) {
        die("the destination path already exists and is not a directory");
    }
    return idx;
}

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s <disk-image> <local-file> <leanfs-path>\n", argv0);
    fprintf(stderr, "       %s -r <disk-image> <local-dir> <leanfs-dir>\n", argv0);
    fprintf(stderr, "       %s -s <disk-image> <target> <leanfs-path>\n\n", argv0);
    fprintf(stderr, "  Writes into disk-image's leanfs filesystem without booting anything.\n");
    fprintf(stderr, "  leanfs paths are absolute (\"/bin/yourprog\"); missing parent\n");
    fprintf(stderr, "  directories are created.\n\n");
    fprintf(stderr, "  -s makes leanfs-path a symbolic link to target. `target` is a\n");
    fprintf(stderr, "  string stored verbatim and is NOT resolved on the host - the link\n");
    fprintf(stderr, "  is being made for a filesystem this host cannot see. M89 added it\n");
    fprintf(stderr, "  for the multi-call binary, which needs one link per command name\n");
    fprintf(stderr, "  and has no host-side tree to copy them out of.\n\n");
    fprintf(stderr, "  -r copies a whole host directory tree, preserving symbolic links,\n");
    fprintf(stderr, "  hard links and modification times, and writes /.image-manifest\n");
    fprintf(stderr, "  describing what it put there.\n\n");
    fprintf(stderr, "  disk-image must already exist and be at least as large as `make all`\n");
    fprintf(stderr, "  produces (build/os-image.bin).\n");
}

int main(int argc, char **argv) {
    uint32_t endian_probe = 1;
    if (*(uint8_t *)&endian_probe != 1) {
        die("this host is big-endian; leanfs's on-disk format is not portable to it as written");
    }

    int recursive = (argc >= 2 && strcmp(argv[1], "-r") == 0);
    int symlink_mode = (argc >= 2 && strcmp(argv[1], "-s") == 0);
    int flagged = recursive || symlink_mode;
    if ((flagged && argc != 5) || (!flagged && argc != 4)) {
        usage(argv[0]);
        return 1;
    }

    const char *image_path  = argv[flagged ? 2 : 1];
    const char *local_path  = argv[flagged ? 3 : 2];
    const char *leanfs_path = argv[flagged ? 4 : 3];

    if (recursive) {
        struct stat st;
        if (lstat(local_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            die("local-dir is not a directory this tool can read");
        }
        image_open(image_path);

        int at = resolve_directory(leanfs_path);
        tree_root = leanfs_path;
        tree_mtime = (uint32_t)st.st_mtime;
        tally.dirs = 1;
        put_tree(local_path, at, (strcmp(leanfs_path, "/") == 0) ? "" : leanfs_path, 1);
        write_manifest(leanfs_path);
        save_all();
        fclose(img);

        printf("leanfs-put: %s -> %s:%s\n", local_path, image_path, leanfs_path);
        printf("            %llu dirs, %llu files (%llu bytes), %llu symlinks, "
               "%llu extra hard links, %llu skipped, %u deep\n",
               (unsigned long long)tally.dirs, (unsigned long long)tally.files,
               (unsigned long long)tally.bytes, (unsigned long long)tally.links,
               (unsigned long long)tally.hardlinks, (unsigned long long)tally.skipped,
               tally.deepest);
        return 0;
    }

    if (symlink_mode) {
        size_t tlen = strlen(local_path);
        if (tlen == 0 || tlen >= LEANFS_MAX_PATH) {
            die("the link target is empty or longer than a path");
        }
        image_open(image_path);
        char leaf[LEANFS_MAX_NAME + 1];
        int parent = resolve_parent(leanfs_path, leaf);
        int idx = directory_lookup(&inodes[parent], leaf);
        if (idx >= 0 && inodes[idx].type == LEANFS_TYPE_LINK) {
            free_inode_blocks(&inodes[idx]);
        } else if (idx >= 0) {
            if (inodes[idx].type == LEANFS_TYPE_DIRECTORY) {
                die("that path already names a directory");
            }
            free_inode_blocks(&inodes[idx]);
            memset(&inodes[idx], 0, sizeof(inodes[idx]));
            idx = alloc_inode(LEANFS_TYPE_LINK);
            directory_repoint(&inodes[parent], leaf, idx);
        } else {
            idx = alloc_inode(LEANFS_TYPE_LINK);
            directory_add(&inodes[parent], leaf, idx);
        }
        inode_write_all(&inodes[idx], (const uint8_t *)local_path, tlen,
                        (uint32_t)time(NULL));
        save_all();
        fclose(img);
        printf("leanfs-put: %s -> %s (symlink)\n", leanfs_path, local_path);
        return 0;
    }

    struct stat st;
    if (lstat(local_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        die("local-file is not a regular file this tool can read");
    }
    if ((uint64_t)st.st_size > (uint64_t)LEANFS_MAX_FILE_SIZE) {
        die("local-file exceeds LEANFS_MAX_FILE_SIZE");
    }
    FILE *source = fopen(local_path, "rb");
    if (!source) {
        die("could not open local-file");
    }

    image_open(image_path);

    char leaf[LEANFS_MAX_NAME + 1];
    int parent = resolve_parent(leanfs_path, leaf);

    int idx = directory_lookup(&inodes[parent], leaf);
    if (idx >= 0 && inodes[idx].type == LEANFS_TYPE_LINK) {
        free_inode_blocks(&inodes[idx]);
        memset(&inodes[idx], 0, sizeof(inodes[idx]));
        idx = alloc_inode(LEANFS_TYPE_FILE);
        directory_repoint(&inodes[parent], leaf, idx);
    } else if (idx >= 0) {
        if (inodes[idx].type != LEANFS_TYPE_FILE) {
            die("that path already names a directory");
        }
        free_inode_blocks(&inodes[idx]);
    } else {
        idx = alloc_inode(LEANFS_TYPE_FILE);
        directory_add(&inodes[parent], leaf, idx);
    }

    inode_write_stream(&inodes[idx], source, (uint64_t)st.st_size,
                       (uint32_t)st.st_mtime, local_path, NULL);
    save_all();

    fclose(img);
    fclose(source);
    printf("leanfs-put: wrote %lld bytes to %s as %s\n",
           (long long)st.st_size, image_path, leanfs_path);
    return 0;
}
