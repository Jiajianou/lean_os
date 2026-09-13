#!/usr/bin/env python3
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "toolchain-port"))
from apply import edit, write_file, port_config_sub, MissingAnchor

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

CONFIGURE_SHAREDLIB_ANCHOR = """    Linux*|GNU*|NetBSD*|FreeBSD*|DragonFly*|OpenBSD*|VxWorks*)
\t  LDLIBRARY='libpython$(LDVERSION).so'"""
CONFIGURE_SHAREDLIB_EDIT = """    lean_os*)
\t  LDLIBRARY='libpython$(LDVERSION).so'
\t  BLDLIBRARY='-L. -lpython$(LDVERSION)'
\t  INSTSONAME="$LDLIBRARY".$SOVERSION
\t  ;;
    Linux*|GNU*|NetBSD*|FreeBSD*|DragonFly*|OpenBSD*|VxWorks*)
\t  LDLIBRARY='libpython$(LDVERSION).so'"""

CONFIGURE_LDSHARED_ANCHOR = """\tLinux*|GNU*|QNX*|VxWorks*|Haiku*)
\t\tLDSHARED='$(CC) -shared'
\t\tLDCXXSHARED='$(CXX) -shared';;"""
CONFIGURE_LDSHARED_EDIT = """\tlean_os*)
\t\tLDSHARED='$(CC) -shared'
\t\tLDCXXSHARED='$(CXX) -shared';;
\tLinux*|GNU*|QNX*|VxWorks*|Haiku*)
\t\tLDSHARED='$(CC) -shared'
\t\tLDCXXSHARED='$(CXX) -shared';;"""

CONFIGURE_CCSHARED_ANCHOR = """\tLinux*|GNU*) CCSHARED="-fPIC";;"""
CONFIGURE_CCSHARED_EDIT = """\tlean_os*) CCSHARED="-fPIC";;
\tLinux*|GNU*) CCSHARED="-fPIC";;"""

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
