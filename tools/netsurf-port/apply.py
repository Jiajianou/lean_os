#!/usr/bin/env python3
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "toolchain-port"))
from apply import edit, write_file, MissingAnchor

WHICH_ANCHOR = "toolpath_ := $(shell /bin/which $(CC__))"
WHICH_EDIT = "toolpath_ := $(shell which $(CC__))"

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
