#!/usr/bin/env python3
"""tools/netsurf-port/apply.py - M100: teach NetSurf's buildsystem about
this desk and about lean_os.

Same shape and the same rules as tools/toolchain-port/apply.py and
tools/python-port/apply.py, whose edit()/write_file() this reuses: the
NetSurf tree is not vendored - build-netsurf.sh downloads
netsurf-all-3.11.tar.gz into build/ and throws it away - so what has to
survive between releases is *the edit*, anchored to a line a person can
go and read upstream, failing loudly by name when the anchor moves.

Every edit below is in one of two categories, and neither of them is a
change to how the browser works:

  1. **Host portability.** NetSurf's buildsystem assumes a Linux box is
     doing the building. This desk is macOS. These edits would be needed
     to cross-compile NetSurf to *anything* from here.

  2. **Adding an OS to the list.** The "teach a portable program a new
     target" recipe, which is the line M94 drew and M89 drew before it:
     configuration shaped the way upstream shapes it.

There is no edit here that touches layout, CSS, HTML parsing, or any
other thing the browser is actually for. The lean_os display surface is
NOT an edit - see user_space/bin/nsfb_leanos.c for why it did not have
to be one.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "toolchain-port"))
from apply import edit, write_file, MissingAnchor  # noqa: E402


# ---- 1. host portability: /bin/which is a Linux path ------------------
#
# Makefile.tools locates the cross compiler with `$(shell /bin/which
# $(CC__))`. On macOS `which` is /usr/bin/which and /bin/which does not
# exist, so the shell prints "Command not found", the result is empty,
# and the buildsystem concludes the toolchain must live under
# /opt/netsurf - then fails with "Unable to detect toolchain", which
# names neither the real problem nor the file it happened in.
#
# Dropping the absolute path is the fix every distribution's package of
# this makes; PATH is already how the rest of the file finds its tools.
WHICH_ANCHOR = "toolpath_ := $(shell /bin/which $(CC__))"
WHICH_EDIT = "toolpath_ := $(shell which $(CC__))"

# ---- 2. host portability: `echo -n` is not portable -------------------
#
# netsurf/Makefile writes the link-time dependency file with
# `echo -n "..." > link.d`. `-n` is a bashism that /bin/sh honours on
# Linux and does not honour here: this desk's `sh` prints the flag, so
# link.d begins `-n nsfb build/...` and the NEXT make run stops at
# "deps/link.d:2: *** missing separator", which names neither `echo` nor
# the file that wrote it.
#
# It bites only on a REBUILD - the first build writes the bad file and
# links fine, and the second reads it - which is what makes it worth an
# anchored edit rather than a note: the failure arrives long after the
# change that caused it.
#
# `printf %s` is the portable spelling and is what POSIX says to use.
ECHO_N_ANCHOR = ('\t$(Q)echo -n "$(EXETARGET) $(DEPROOT)/link.d: " '
                 '> $(DEPROOT)/link.d')
ECHO_N_EDIT = ('\t$(Q)printf %s "$(EXETARGET) $(DEPROOT)/link.d: " '
               '> $(DEPROOT)/link.d')

EDITS = [
    ("buildsystem/makefiles/Makefile.tools", WHICH_ANCHOR, WHICH_EDIT,
     "the shell command that locates the cross compiler"),
    ("netsurf/Makefile", ECHO_N_ANCHOR, ECHO_N_EDIT,
     "the rule that writes the link-time dependency file"),
]


def main():
    if len(sys.argv) != 2:
        print("usage: apply.py <netsurf-all tree>", file=sys.stderr)
        return 1
    root = sys.argv[1]
    rc = 0
    for path, anchor, replacement, why in EDITS:
        full = os.path.join(root, path)
        try:
            print("  %-52s %s" % (path, edit(full, anchor, replacement, why)))
        except MissingAnchor as e:
            print("netsurf-port: %s" % e, file=sys.stderr)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
