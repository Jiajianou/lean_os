#!/usr/bin/env python3
"""tools/gen-iconv-tables.py - M100: the single-byte charset tables for
user_space/libc/src/iconv.c.

Same argument as tools/gen-font.c: a table of 3,584 numbers typed by hand
is a table with a typo in it, and the typo is in the character nobody on
this project ever types. So it is generated, checked in as generated
source, and - the part that matters - graded on every `--fast` run by
tools/iconv-test.sh against the HOST's iconv, which is a different
implementation written by different people from different data.

Two independent sources have to agree before a byte of this is believed:
Python's codec tables produce it, and the host's libiconv checks it.

Run it when the charset list below changes. It rewrites
user_space/libc/src/iconv_tables.c in place - or writes the path given
as its one argument instead, which is how the test compares without
touching the tree.
"""
import codecs
import os
import sys

# name in this libc          python codec
CHARSETS = [
    ("iso-8859-2",   "iso8859_2"),
    ("iso-8859-3",   "iso8859_3"),
    ("iso-8859-4",   "iso8859_4"),
    ("iso-8859-5",   "iso8859_5"),
    ("iso-8859-6",   "iso8859_6"),
    ("iso-8859-7",   "iso8859_7"),
    ("iso-8859-8",   "iso8859_8"),
    ("iso-8859-9",   "iso8859_9"),
    ("iso-8859-10",  "iso8859_10"),
    ("iso-8859-11",  "iso8859_11"),
    ("iso-8859-13",  "iso8859_13"),
    ("iso-8859-14",  "iso8859_14"),
    ("iso-8859-15",  "iso8859_15"),
    ("iso-8859-16",  "iso8859_16"),
    ("windows-1250", "cp1250"),
    ("windows-1251", "cp1251"),
    ("windows-1252", "cp1252"),
    ("windows-1253", "cp1253"),
    ("windows-1254", "cp1254"),
    ("windows-1255", "cp1255"),
    ("windows-1256", "cp1256"),
    ("windows-1257", "cp1257"),
    ("windows-1258", "cp1258"),
    ("koi8-r",       "koi8_r"),
    ("koi8-u",       "koi8_u"),
    ("macintosh",    "mac_roman"),
]

HEADER = '''#include "iconv_tables.h"

'''


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    out = os.path.join(root, "user_space/libc/src/iconv_tables.c")
    # M116: or somewhere else, which is what tools/iconv-test.sh asks
    # for. It used to regenerate the checked-in file in place to compare
    # it - identical bytes, new mtime - and that one timestamp made the
    # next `make` relink every program on the machine, rebuild the
    # kernel that embeds them and recreate the disk image. See M116.
    if len(sys.argv) > 1:
        out = sys.argv[1]
    body = []
    names = []
    for name, pyname in CHARSETS:
        dec = codecs.lookup(pyname)
        row = []
        for b in range(0x80, 0x100):
            try:
                ch = bytes([b]).decode(pyname)
            except UnicodeDecodeError:
                row.append(0xFFFF)
                continue
            cp = ord(ch)
            if cp > 0xFFFF:
                print("%s: 0x%02X maps above the BMP (U+%04X); the table "
                      "is uint16_t and cannot hold it" % (name, b, cp),
                      file=sys.stderr)
                return 1
            row.append(cp)
        ident = name.replace("-", "_")
        names.append((name, ident))
        body.append("static const uint16_t tbl_%s[128] = {\n" % ident)
        for i in range(0, 128, 8):
            body.append("    " + " ".join("0x%04X," % v for v in row[i:i + 8])
                        + "\n")
        body.append("};\n\n")
        del dec

    body.append("const struct iconv_sb_charset __iconv_sb_charsets[] = {\n")
    for name, ident in names:
        body.append('    {"%s", tbl_%s},\n' % (name, ident))
    body.append("};\n\n")
    body.append("const size_t __iconv_sb_charset_count =\n"
                "    sizeof(__iconv_sb_charsets) / sizeof(__iconv_sb_charsets[0]);\n")

    with open(out, "w") as f:
        f.write(HEADER)
        f.write("".join(body))
    print("gen-iconv-tables: %s - %d charsets, %d entries"
          % (os.path.relpath(out, root), len(CHARSETS), len(CHARSETS) * 128))
    return 0


if __name__ == "__main__":
    sys.exit(main())
