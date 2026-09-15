#!/usr/bin/env python3
"""Every code point Unicode has, and what UTF-8 and UTF-16 make of it.

Python decides the answers. Nothing in this project is consulted, which is
the point: <uchar.h>'s whole job is to agree with every other
implementation of the same two encodings.
"""

import sys


def main(path):
    with open(path, "w") as out:
        for code_point in range(0x110000):
            if 0xD800 <= code_point <= 0xDFFF:
                continue
            character = chr(code_point)
            out.write("%X %s %s\n" % (code_point,
                                      character.encode("utf-8").hex(),
                                      character.encode("utf-16-le").hex()))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "uchar-cases.txt")
