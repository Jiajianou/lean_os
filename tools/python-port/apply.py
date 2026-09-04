#!/usr/bin/env python3
"""tools/python-port/apply.py - M99: teach CPython about lean_os.

Same shape and the same argument as tools/toolchain-port/apply.py, whose
edit()/write_file() this reuses rather than copies: CPython is not
vendored - build-python.sh downloads a tarball into build/ and throws it
away - so what has to survive between releases is *the edit*, anchored to
a line a person can go and read upstream, failing loudly by name when the
anchor moves.

Every edit here is the "add an OS to a portable program" recipe, which is
the line M94 drew and M89 drew before it: configuration shaped the way
upstream shapes it, never a change to how the interpreter works. There is
no edit below that touches an object, an opcode, or a semantic.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "toolchain-port"))
from apply import edit, write_file, port_config_sub, MissingAnchor  # noqa: E402


# ---- configure: the two cross-build case statements -------------------
#
# CPython refuses to cross-compile to a host it has never heard of, and
# says so twice - once when it picks MACHDEP (which becomes sys.platform)
# and once when it builds _PYTHON_HOST_PLATFORM (which names the build
# directory the extension modules land in). Both are closed lists with a
# comment saying "for now, limit cross builds to known configurations",
# which is an invitation to add a case, not a wall.
#
# ac_sys_system is the name, not the triple: MACHDEP is derived from it
# by lowercasing and stripping the release, so `lean_os` in, `lean_os`
# out, and `sys.platform == "lean_os"` on the machine. That string is the
# one a setup.py or a configure script will branch on, so it is the OS's
# own name and not a lie about being Linux.
CONFIGURE_MACHDEP_ANCHOR = """	*-*-wasi)
	    ac_sys_system=WASI
	    ;;
	*)"""
CONFIGURE_MACHDEP_EDIT = """	*-*-wasi)
	    ac_sys_system=WASI
	    ;;
	*-*-lean_os*)
	    ac_sys_system=lean_os
	    ;;
	*)"""

CONFIGURE_HOSTPLAT_ANCHOR = """	wasm32-*-* | wasm64-*-*)
		_host_cpu=$host_cpu
		;;
	*)"""
CONFIGURE_HOSTPLAT_EDIT = """	wasm32-*-* | wasm64-*-*)
		_host_cpu=$host_cpu
		;;
	*-*-lean_os*)
		_host_cpu=$host_cpu
		;;
	*)"""


def port_cpython(root):
    out = []
    out.append(("config.sub", port_config_sub(
        os.path.join(root, "config.sub"))))
    out.append(("configure (MACHDEP)", edit(
        os.path.join(root, "configure"),
        CONFIGURE_MACHDEP_ANCHOR, CONFIGURE_MACHDEP_EDIT,
        "the cross-build list that decides sys.platform")))
    out.append(("configure (host platform)", edit(
        os.path.join(root, "configure"),
        CONFIGURE_HOSTPLAT_ANCHOR, CONFIGURE_HOSTPLAT_EDIT,
        "the cross-build list that names the build directory")))
    return out


def main():
    if len(sys.argv) != 2:
        print("usage: apply.py <cpython-source-dir>", file=sys.stderr)
        return 2
    try:
        for name, status in port_cpython(sys.argv[1]):
            print("  cpython  %-28s %s" % (name, status))
    except MissingAnchor as e:
        print("python-port: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
