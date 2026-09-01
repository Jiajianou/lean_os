#!/usr/bin/env python3
"""tools/leanfs-fsck.py - Q17: an independent check of a leanfs image.

---- Why a second implementation, when Q3 spent a milestone deleting one

Q3's whole point was that two copies of the on-disk format, kept in
agreement by a comment, is a data-corruption bug with a long fuse - and
it was right, because they had already drifted. This reader is a
deliberate exception to that rule and the reason is specific.

The thing being checked here is whether the kernel's filesystem survives
having power cut mid-write. A checker built from the kernel's own code
shares the kernel's own assumptions, so a bug in those assumptions is
invisible to it in exactly the case it is most needed: `leanfs_check`
cannot find a corruption that `leanfs.c` does not believe is possible.
An independent reader can.

The drift risk is real and is handled the only honest way available: this
file asserts the geometry it expects against the superblock it finds, and
refuses rather than guessing if they disagree. A format change breaks
this loudly on the next run instead of quietly passing a broken image.

---- What it checks ---------------------------------------------------

Structural invariants, none of which the kernel is asked about:

  * the superblock's geometry is this build's
  * every block referenced by an inode is inside the data region
  * no block is referenced by two inodes, or twice by one
  * every referenced block is marked allocated in the bitmap
  * every directory entry names an allocated inode
  * directory records tile their block exactly and none straddles one
  * the directory tree is a tree: reachable from the root, no cycles
  * every inode with a nonzero type is reachable by some name
  * a file's size agrees with the blocks it holds

Usage:
    tools/leanfs-fsck.py build/os-image.bin
    tools/leanfs-fsck.py --quiet image      # exit status only
"""

import argparse
import struct
import sys

# The geometry this build expects. Checked against the superblock rather
# than assumed - see the header.
START_LBA = 8192
SECTOR = 512
BLOCK = 4096
SECTORS_PER_BLOCK = BLOCK // SECTOR
START_BLOCK = START_LBA // SECTORS_PER_BLOCK
MAX_INODES = 131072
DATA_BLOCKS = 524288
INODE_SIZE = 128
DIRECT_BLOCKS = 16
INDIRECT_POINTERS = BLOCK // 4
MAGIC = 0x3553464C          # "LFS5"
VERSION = 5
STATE_CLEAN = 0
STATE_DIRTY = 0x4449525A

TYPE_FREE, TYPE_FILE, TYPE_DIR, TYPE_LINK = 0, 1, 2, 3
DIRENT_HDR = 8
ROOT_INODE = 0


class Image:
    def __init__(self, path):
        self.f = open(path, "rb")

    def block(self, n):
        self.f.seek(n * BLOCK)
        b = self.f.read(BLOCK)
        if len(b) < BLOCK:
            b += b"\0" * (BLOCK - len(b))
        return b


class Fsck:
    def __init__(self, path):
        self.img = Image(path)
        self.problems = []
        self.notes = []

    def bad(self, fmt, *a):
        self.problems.append(fmt % a if a else fmt)

    def note(self, fmt, *a):
        self.notes.append(fmt % a if a else fmt)

    # ---- superblock ----------------------------------------------------

    def read_super(self):
        raw = self.img.block(START_BLOCK)[:36]
        (self.magic, self.inode_table_block, self.inode_table_blocks,
         self.bitmap_block, self.bitmap_blocks, self.data_block,
         self.data_blocks, self.state, self.version) = struct.unpack("<9I", raw)

        if self.magic != MAGIC:
            self.bad("superblock magic is 0x%08X, expected 0x%08X (LFS5). "
                     "This is not a leanfs image this build understands.",
                     self.magic, MAGIC)
            return False
        if self.version != VERSION:
            self.bad("on-disk version is %d, this checker knows %d",
                     self.version, VERSION)
            return False
        # Geometry, checked rather than assumed. A mismatch means this
        # file has drifted from kernel/fs/leanfs_format.h and every
        # check below would be reading the wrong offsets.
        expect = {
            "inode_table_blocks": (self.inode_table_blocks,
                                   MAX_INODES * INODE_SIZE // BLOCK),
            "bitmap_blocks": (self.bitmap_blocks, DATA_BLOCKS // 8 // BLOCK),
            "data_blocks": (self.data_blocks, DATA_BLOCKS),
        }
        ok = True
        for name, (got, want) in expect.items():
            if got != want:
                self.bad("superblock %s is %d, this checker is built for %d - "
                         "tools/leanfs-fsck.py has drifted from "
                         "kernel/fs/leanfs_format.h", name, got, want)
                ok = False
        if self.state == STATE_DIRTY:
            self.note("mounted dirty (the machine did not unmount cleanly) - "
                      "expected after a crash, and the whole point of this run")
        elif self.state != STATE_CLEAN:
            self.bad("superblock state is 0x%08X, which is neither CLEAN nor "
                     "DIRTY", self.state)
        return ok

    # ---- inodes and the bitmap ------------------------------------------

    def read_inodes(self):
        raw = b"".join(self.img.block(self.inode_table_block + i)
                       for i in range(self.inode_table_blocks))
        self.inodes = []
        fmt = "<3I%dII I40s" % DIRECT_BLOCKS
        size = struct.calcsize("<" + "I" * (3 + DIRECT_BLOCKS + 3) + "40s")
        assert size == INODE_SIZE, size
        for i in range(MAX_INODES):
            off = i * INODE_SIZE
            fields = struct.unpack_from("<%dI" % (3 + DIRECT_BLOCKS + 3),
                                        raw, off)
            self.inodes.append({
                "type": fields[0], "size": fields[1], "mtime": fields[2],
                "direct": list(fields[3:3 + DIRECT_BLOCKS]),
                "indirect": fields[3 + DIRECT_BLOCKS],
                "dindirect": fields[4 + DIRECT_BLOCKS],
                "nlink": fields[5 + DIRECT_BLOCKS],
            })

    def read_bitmap(self):
        self.bitmap = b"".join(self.img.block(self.bitmap_block + i)
                               for i in range(self.bitmap_blocks))

    def bit(self, n):
        return (self.bitmap[n // 8] >> (n % 8)) & 1

    def blocks_of(self, idx):
        """Every data-region block number this inode references, including
        its own indirect tables - those are blocks too and a checker that
        forgets them reports every one as leaked."""
        ino = self.inodes[idx]
        out = []

        def valid(b):
            if b == 0:
                return False
            if b >= self.data_blocks:
                self.bad("inode %d references block %d, outside the %d-block "
                         "data region", idx, b, self.data_blocks)
                return False
            return True

        for b in ino["direct"]:
            if valid(b):
                out.append(b)
        if valid(ino["indirect"]):
            out.append(ino["indirect"])
            table = self.img.block(self.data_block + ino["indirect"])
            for j in range(INDIRECT_POINTERS):
                b = struct.unpack_from("<I", table, j * 4)[0]
                if valid(b):
                    out.append(b)
        if valid(ino["dindirect"]):
            out.append(ino["dindirect"])
            l1 = self.img.block(self.data_block + ino["dindirect"])
            for j in range(INDIRECT_POINTERS):
                b1 = struct.unpack_from("<I", l1, j * 4)[0]
                if not valid(b1):
                    continue
                out.append(b1)
                l2 = self.img.block(self.data_block + b1)
                for k in range(INDIRECT_POINTERS):
                    b2 = struct.unpack_from("<I", l2, k * 4)[0]
                    if valid(b2):
                        out.append(b2)
        return out

    def check_blocks(self):
        owner = {}
        for idx, ino in enumerate(self.inodes):
            if ino["type"] == TYPE_FREE:
                continue
            seen = set()
            for b in self.blocks_of(idx):
                if b in seen:
                    self.bad("inode %d references block %d more than once",
                             idx, b)
                seen.add(b)
                if b in owner:
                    self.bad("block %d is referenced by inode %d and inode %d "
                             "- two files share storage", b, owner[b], idx)
                else:
                    owner[b] = idx
                if not self.bit(b):
                    self.bad("block %d is referenced by inode %d but marked "
                             "free in the bitmap - the allocator can hand it "
                             "out again", b, idx)
        self.referenced = owner
        # Block 0 is reserved forever so a zero pointer can mean "nothing".
        if not self.bit(0):
            self.bad("block 0 is marked free; it is reserved so that a zero "
                     "block pointer means 'no block'")
        allocated = sum(self.bit(b) for b in range(self.data_blocks))
        leaked = allocated - len(owner) - 1   # -1 for the reserved block 0
        if leaked > 0:
            self.note("%d block(s) marked allocated but referenced by no "
                      "inode. Storage that cannot be reused; not corruption, "
                      "and expected after an interrupted write", leaked)

    # ---- directories -----------------------------------------------------

    def dir_entries(self, idx):
        """(name, inode, type) for a directory, checking the record
        invariants as it walks."""
        ino = self.inodes[idx]
        blocks = ino["direct"] + ([] if not ino["indirect"] else [])
        out = []
        nblocks = (ino["size"] + BLOCK - 1) // BLOCK
        logical = 0
        for b in ino["direct"]:
            if logical >= nblocks:
                break
            logical += 1
            if b == 0 or b >= self.data_blocks:
                continue
            data = self.img.block(self.data_block + b)
            off = 0
            while off < BLOCK:
                if off + DIRENT_HDR > BLOCK:
                    self.bad("directory inode %d: a record header straddles "
                             "the end of a block", idx)
                    break
                d_ino, rec_len, name_len, d_type = struct.unpack_from(
                    "<IHBB", data, off)
                if rec_len < DIRENT_HDR or rec_len % 4 != 0:
                    self.bad("directory inode %d: record at offset %d has "
                             "rec_len %d, which is not a valid length",
                             idx, off, rec_len)
                    break
                if off + rec_len > BLOCK:
                    self.bad("directory inode %d: record at offset %d claims "
                             "%d bytes and runs past its block", idx, off, rec_len)
                    break
                if d_ino != 0:
                    if DIRENT_HDR + name_len > rec_len:
                        self.bad("directory inode %d: a name of %d bytes does "
                                 "not fit its %d-byte record", idx, name_len, rec_len)
                        break
                    name = data[off + DIRENT_HDR:off + DIRENT_HDR + name_len]
                    out.append((name.decode("utf-8", "replace"), d_ino, d_type))
                off += rec_len
            if off != BLOCK:
                self.bad("directory inode %d: records do not tile the block "
                         "exactly (ended at %d of %d)", idx, off, BLOCK)
        return out

    def check_tree(self):
        if self.inodes[ROOT_INODE]["type"] != TYPE_DIR:
            self.bad("the root inode is type %d, not a directory",
                     self.inodes[ROOT_INODE]["type"])
            return
        seen = set()
        stack = [(ROOT_INODE, "/")]
        self.names = {}
        while stack:
            idx, path = stack.pop()
            if idx in seen:
                self.bad("directory cycle: inode %d is reachable twice (%s)",
                         idx, path)
                continue
            seen.add(idx)
            for name, child, _t in self.dir_entries(idx):
                if child >= MAX_INODES:
                    self.bad("%s%s names inode %d, past the end of the table",
                             path, name, child)
                    continue
                if self.inodes[child]["type"] == TYPE_FREE:
                    self.bad("%s%s names inode %d, which is free - a name "
                             "pointing at nothing", path, name, child)
                    continue
                self.names.setdefault(child, []).append(path + name)
                if self.inodes[child]["type"] == TYPE_DIR:
                    stack.append((child, path + name + "/"))
        self.reachable = seen

        orphans = [i for i, ino in enumerate(self.inodes)
                   if ino["type"] != TYPE_FREE and i not in self.names
                   and i != ROOT_INODE]
        if orphans:
            self.note("%d allocated inode(s) with no name: %s. Storage that "
                      "cannot be reached; expected after an interrupted "
                      "create", len(orphans), orphans[:8])

    def check_sizes(self):
        for idx, ino in enumerate(self.inodes):
            if ino["type"] not in (TYPE_FILE, TYPE_DIR):
                continue
            need = (ino["size"] + BLOCK - 1) // BLOCK
            have = len([b for b in self.blocks_of(idx)])
            if need > have:
                self.bad("inode %d says it is %d bytes (%d blocks) but holds "
                         "%d block(s) - reading it would run off the end",
                         idx, ino["size"], need, have)

    def run(self):
        if not self.read_super():
            return False
        self.read_inodes()
        self.read_bitmap()
        self.check_blocks()
        self.check_tree()
        self.check_sizes()
        return not self.problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    fs = Fsck(args.image)
    ok = fs.run()

    if not args.quiet:
        for n in fs.notes:
            print("note: %s" % n)
        for p in fs.problems:
            print("PROBLEM: %s" % p)
        print("%s: %s" % (args.image,
                          "consistent" if ok else
                          "%d problem(s)" % len(fs.problems)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
