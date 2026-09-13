#!/usr/bin/env python3

import argparse
import os
import struct
import sys

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
MAGIC = 0x3553464C
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
    def __init__(self, path, compare_against=None):
        self.img = Image(path)
        self.problems = []
        self.notes = []
        self.compared = None
        self.compare_against = compare_against

    def bad(self, fmt, *a):
        self.problems.append(fmt % a if a else fmt)

    def note(self, fmt, *a):
        self.notes.append(fmt % a if a else fmt)

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
                         "kernel/file_system/leanfs_format.h", name, got, want)
                ok = False
        if self.state == STATE_DIRTY:
            self.note("mounted dirty (the machine did not unmount cleanly) - "
                      "expected after a crash, and the whole point of this run")
        elif self.state != STATE_CLEAN:
            self.bad("superblock state is 0x%08X, which is neither CLEAN nor "
                     "DIRTY", self.state)
        return ok

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
        if not self.bit(0):
            self.bad("block 0 is marked free; it is reserved so that a zero "
                     "block pointer means 'no block'")
        allocated = sum(self.bit(b) for b in range(self.data_blocks))
        leaked = allocated - len(owner) - 1
        if leaked > 0:
            self.note("%d block(s) marked allocated but referenced by no "
                      "inode. Storage that cannot be reused; not corruption, "
                      "and expected after an interrupted write", leaked)

    def logical_blocks(self, idx, count):
        out = []
        ino = self.inodes[idx]

        def take(b):
            out.append(b if 0 < b < self.data_blocks else 0)

        for b in ino["direct"][:count]:
            take(b)
        if len(out) >= count:
            return out[:count]

        if 0 < ino["indirect"] < self.data_blocks:
            table = self.img.block(self.data_block + ino["indirect"])
            for j in range(INDIRECT_POINTERS):
                if len(out) >= count:
                    return out[:count]
                take(struct.unpack_from("<I", table, j * 4)[0])
        if len(out) >= count:
            return out[:count]

        if 0 < ino["dindirect"] < self.data_blocks:
            l1 = self.img.block(self.data_block + ino["dindirect"])
            for j in range(INDIRECT_POINTERS):
                b1 = struct.unpack_from("<I", l1, j * 4)[0]
                if not 0 < b1 < self.data_blocks:
                    continue
                l2 = self.img.block(self.data_block + b1)
                for k in range(INDIRECT_POINTERS):
                    if len(out) >= count:
                        return out[:count]
                    take(struct.unpack_from("<I", l2, k * 4)[0])
        if len(out) < count:
            self.bad("inode %d says it is %d block(s) long but only %d are "
                     "mapped - reading it would run off the end",
                     idx, count, len(out))
        return out

    def dir_entries(self, idx):
        ino = self.inodes[idx]
        out = []
        nblocks = (ino["size"] + BLOCK - 1) // BLOCK
        for b in self.logical_blocks(idx, nblocks):
            if b == 0:
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

    def read_file(self, idx):
        ino = self.inodes[idx]
        nblocks = (ino["size"] + BLOCK - 1) // BLOCK
        data = bytearray()
        for b in self.logical_blocks(idx, nblocks):
            if b == 0:
                data += b"\0" * BLOCK
            else:
                data += self.img.block(self.data_block + b)
        return bytes(data[:ino["size"]])

    def resolve(self, path):
        idx = ROOT_INODE
        for comp in [c for c in path.split("/") if c]:
            if self.inodes[idx]["type"] != TYPE_DIR:
                return None
            hit = [i for n, i, _t in self.dir_entries(idx) if n == comp]
            if not hit:
                return None
            idx = hit[0]
        return idx

    def compare_tree(self, host_dir, at):
        idx = self.resolve(at)
        if idx is None:
            self.bad("--compare-tree: %s does not exist in this image", at)
            return
        self.shared = {}
        self.compared = {"dirs": 0, "files": 0, "links": 0, "bytes": 0}
        self._compare_dir(host_dir, idx, at)

    def _compare_dir(self, host_dir, idx, path):
        self.compared["dirs"] += 1
        image = {}
        for name, child, _t in self.dir_entries(idx):
            if name in image:
                self.bad("%s: the name %r appears twice in one directory",
                         path, name)
            image[name] = child

        host = {}
        for e in os.scandir(host_dir):
            host[e.name] = e.path

        for name in sorted(set(host) - set(image)):
            self.bad("%s/%s is on the host and not in the image", path, name)
        for name in sorted(set(image) - set(host)):
            self.bad("%s/%s is in the image and not on the host", path, name)

        for name in sorted(set(host) & set(image)):
            hp = host[name]
            child = image[name]
            ino = self.inodes[child]
            st = os.lstat(hp)
            sub = "%s/%s" % (path.rstrip("/"), name)

            if os.path.islink(hp):
                if ino["type"] != TYPE_LINK:
                    self.bad("%s is a symlink on the host and type %d here",
                             sub, ino["type"])
                    continue
                want = os.readlink(hp).encode()
                got = self.read_file(child)
                if got != want:
                    self.bad("%s points at %r here and %r on the host",
                             sub, got, want)
                self.compared["links"] += 1
            elif os.path.isdir(hp):
                if ino["type"] != TYPE_DIR:
                    self.bad("%s is a directory on the host and type %d here",
                             sub, ino["type"])
                    continue
                self._compare_dir(hp, child, sub)
            else:
                if ino["type"] != TYPE_FILE:
                    self.bad("%s is a regular file on the host and type %d here",
                             sub, ino["type"])
                    continue
                key = (st.st_dev, st.st_ino)
                if st.st_nlink > 1:
                    first = self.shared.get(key)
                    if first is None:
                        self.shared[key] = (child, sub)
                    elif first[0] != child:
                        self.bad("%s and %s are one file on the host and two "
                                 "inodes here (%d and %d) - the hard link "
                                 "became a copy", first[1], sub, first[0], child)
                if ino["size"] != st.st_size:
                    self.bad("%s is %d bytes here and %d on the host",
                             sub, ino["size"], st.st_size)
                    continue
                with open(hp, "rb") as f:
                    want = f.read()
                if self.read_file(child) != want:
                    self.bad("%s has different contents here than on the host", sub)
                self.compared["files"] += 1
                self.compared["bytes"] += st.st_size

    def run(self):
        if not self.read_super():
            return False
        self.read_inodes()
        self.read_bitmap()
        self.check_blocks()
        self.check_tree()
        self.check_sizes()
        if self.compare_against:
            self.compare_tree(*self.compare_against)
        return not self.problems

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--compare-tree", metavar="HOSTDIR",
                    help="also check that the image holds exactly this host "
                         "directory tree, byte for byte")
    ap.add_argument("--at", metavar="LEANFS_PATH", default="/",
                    help="where in the image that tree was written "
                         "(default: /)")
    args = ap.parse_args()

    compare = (args.compare_tree, args.at) if args.compare_tree else None
    fs = Fsck(args.image, compare)
    ok = fs.run()

    if not args.quiet:
        if fs.compared:
            print("compared: %d dirs, %d files (%d bytes), %d symlinks"
                  % (fs.compared["dirs"], fs.compared["files"],
                     fs.compared["bytes"], fs.compared["links"]))
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
