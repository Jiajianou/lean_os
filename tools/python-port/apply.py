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


# ---- M99's second increment: the four arms a SHARED build needs -------
#
# The two edits above are what it takes to cross-compile CPython at all.
# These four are what it takes to build the *dynamic* one - a
# libpython3.12.so, a PIE interpreter, and every C extension module as a
# shared object dlopen'ed at import - which is M99's own open box and
# the thing M95's dynamic loader was built for and had never carried.
#
# All four are arms in closed `case $ac_sys_system` lists, exactly like
# the two above, and none of them says anything about how the
# interpreter works.
#
# **Four, and M99's entry predicted four.** It was right about the
# number and wrong about one of the members: it expected an arm for
# DYNLOADFILE, and DYNLOADFILE needs none, because configure's own `*)`
# fallback picks dynload_shlib.o for any system whose dlopen it found.
# What it did not expect is that `--enable-shared` has a closed list of
# its own, naming what the interpreter library is called. See the
# archive's standing lesson about predicted lists.
#
# ---- why --enable-shared, and not a static python with shared modules -
#
# The second was tried first and is a dead end on this target, for a
# reason that is this project's own rather than CPython's: with
# --disable-shared, the interpreter's ~450 core objects are compiled
# with $(CFLAGS) and nothing else, so there is no upstream-shaped place
# to say "and these have to be position-independent". They must be:
# LINKFORSHARED makes python3 a PIE here (it has to be a PIE - a static
# program on this machine cannot dlopen at all, which is M95's own
# arrangement and the reason this box was open), and a non-PIC object in
# a PIE is a page of "relocation R_X86_64_32S ... can not be used when
# making a PIE object".
#
# --enable-shared answers it in upstream's own terms: LIBRARY and
# LDLIBRARY differ, so CFLAGSFORSHARED becomes $(CCSHARED), and every
# core object is compiled -fPIC by the same variable the extension
# modules use. It is also what every distribution ships, which means it
# is the configuration whose failures are somebody else's bugs too.
CONFIGURE_SHAREDLIB_ANCHOR = """    Linux*|GNU*|NetBSD*|FreeBSD*|DragonFly*|OpenBSD*|VxWorks*)
\t  LDLIBRARY='libpython$(LDVERSION).so'"""
CONFIGURE_SHAREDLIB_EDIT = """    lean_os*)
\t  LDLIBRARY='libpython$(LDVERSION).so'
\t  BLDLIBRARY='-L. -lpython$(LDVERSION)'
\t  INSTSONAME="$LDLIBRARY".$SOVERSION
\t  ;;
    Linux*|GNU*|NetBSD*|FreeBSD*|DragonFly*|OpenBSD*|VxWorks*)
\t  LDLIBRARY='libpython$(LDVERSION).so'"""

# RUNSHARED is deliberately not set in that arm, and PY3LIBRARY is not
# either. RUNSHARED tells the build how to run the python it just built,
# on the BUILD machine - which in a cross build is a thing that cannot
# happen and is what --with-build-python is for. PY3LIBRARY is
# libpython3.so, the stable-ABI shim; nothing on this disk loads a module
# built against the limited API, and it is one more object to install for
# a case that does not exist yet.

# LDSHARED: how a shared object is linked. The `*)` fallback in this list
# is `LDSHARED="ld"` - the bare linker, with no specs, no startup files
# and no libc - which does not fail, it produces a .so with nothing
# resolved in it.
CONFIGURE_LDSHARED_ANCHOR = """\tLinux*|GNU*|QNX*|VxWorks*|Haiku*)
\t\tLDSHARED='$(CC) -shared'
\t\tLDCXXSHARED='$(CXX) -shared';;"""
CONFIGURE_LDSHARED_EDIT = """\tlean_os*)
\t\tLDSHARED='$(CC) -shared'
\t\tLDCXXSHARED='$(CXX) -shared';;
\tLinux*|GNU*|QNX*|VxWorks*|Haiku*)
\t\tLDSHARED='$(CC) -shared'
\t\tLDCXXSHARED='$(CXX) -shared';;"""

# CCSHARED: the flag that makes an object fit to go in one. There is no
# default - an unlisted system gets the empty string - and the failure is
# the whole compile succeeding and the link of the first module saying
# "relocation R_X86_64_32S against `.rodata' can not be used when making
# a shared object", about a file whose compile line never mentioned a
# shared object.
#
# -fPIC and nothing else: -mcmodel=small is this target's own
# requirement for position-independent code and the driver supplies it
# from -fPIC. That is M99's change to gcc/config/lean_os.h, and it is
# the reason this arm can be the same one line Linux has.
CONFIGURE_CCSHARED_ANCHOR = """\tLinux*|GNU*) CCSHARED="-fPIC";;"""
CONFIGURE_CCSHARED_EDIT = """\tlean_os*) CCSHARED="-fPIC";;
\tLinux*|GNU*) CCSHARED="-fPIC";;"""

# LINKFORSHARED: how the interpreter itself is linked, and the one arm
# here that differs from what Linux does.
#
#   -pie, because dlopen lives in /lib/ld-lean.so and a static
#   executable never maps it (M95). On Linux this is not needed because
#   the interpreter is dynamic either way; here it is the difference
#   between an interpreter that can import a C module and one that
#   cannot. It is also what makes the link go through LINK_SPEC's
#   dynamic branch at all - the static branch links at 512 GiB under a
#   fixed script and has no interpreter.
#
#   --export-dynamic, so the interpreter's symbols are in .dynsym. With
#   --enable-shared the modules resolve against libpython3.12.so and
#   would manage without it; it is kept because it is what upstream does
#   on every ELF system and because a module that resolves Py_* out of
#   the executable rather than the library is not an error here - the
#   program is the head of the loader's search scope by design.
#
# Spelled `--export-dynamic` rather than Linux's `-export-dynamic`: GNU
# ld accepts both, and the single-dash spelling is the one that also
# reads as a valid single-letter cluster to a linker that is not GNU ld.
CONFIGURE_LINKFORSHARED_ANCHOR = (
    """\tLinux*|GNU*) LINKFORSHARED="-Xlinker -export-dynamic";;""")
CONFIGURE_LINKFORSHARED_EDIT = (
    """\tlean_os*) LINKFORSHARED="-pie -Xlinker --export-dynamic";;
\tLinux*|GNU*) LINKFORSHARED="-Xlinker -export-dynamic";;""")


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
    out.append(("configure (shared libpython)", edit(
        os.path.join(root, "configure"),
        CONFIGURE_SHAREDLIB_ANCHOR, CONFIGURE_SHAREDLIB_EDIT,
        "the --enable-shared list that names the interpreter library")))
    out.append(("configure (LDSHARED)", edit(
        os.path.join(root, "configure"),
        CONFIGURE_LDSHARED_ANCHOR, CONFIGURE_LDSHARED_EDIT,
        "the list that says how a shared object is linked")))
    out.append(("configure (CCSHARED)", edit(
        os.path.join(root, "configure"),
        CONFIGURE_CCSHARED_ANCHOR, CONFIGURE_CCSHARED_EDIT,
        "the list that says how an object fit for one is compiled")))
    out.append(("configure (LINKFORSHARED)", edit(
        os.path.join(root, "configure"),
        CONFIGURE_LINKFORSHARED_ANCHOR, CONFIGURE_LINKFORSHARED_EDIT,
        "the list that says how the interpreter itself is linked")))
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
