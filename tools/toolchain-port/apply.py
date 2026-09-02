#!/usr/bin/env python3
"""tools/toolchain-port/apply.py - M94: teach binutils and GCC about lean_os.

---- why a script of anchored edits rather than a .patch series ----------

tools/toybox-port/ uses real patch files, and that is right there: toybox
is vendored in third_party/ byte-identical to its tarball, so a diff
against the tarball has to keep meaning something.

binutils and GCC are not vendored. They are downloaded by
tools/build-toolchain.sh, unpacked into build/, and thrown away - nothing
in this repository claims to hold a copy of them. What has to survive is
*the edit*, stated so a person can read it and check it against the
upstream file, and that survives better as an anchored replacement than
as a context diff: a diff breaks on the next release for a reason that
has nothing to do with the edit, and this fails with the name of the
anchor it could not find, which is the actual news.

Every edit below is the "add an OS to a toolchain" recipe and nothing
else. M94's own bullet draws the line: a target port is upstream-shaped
configuration, and a patch to the compiler's own passes is what M63's
rule forbids. Nothing here touches a pass, an optimisation, or a
code-generation decision.
"""
import sys
import os
import shutil


class MissingAnchor(Exception):
    pass


def edit(path, anchor, replacement, why):
    """Replaces the first occurrence of `anchor`. Fails loudly."""
    with open(path) as f:
        text = f.read()
    if replacement in text:
        return "already applied"
    if anchor not in text:
        raise MissingAnchor(
            "%s: could not find the anchor for '%s'.\n"
            "The upstream file has moved. The edit itself is still what it\n"
            "was; what needs updating is where it attaches.\n"
            "Anchor: %r" % (path, why, anchor[:120]))
    with open(path, "w") as f:
        f.write(text.replace(anchor, replacement, 1))
    return "applied"


# The OS name, added to the list config.sub validates against. Both trees
# ship their own copy and both have to be told, and so does every
# autotools project - see port_config_sub. `lean_os*` rather than
# `leanos*` because the triple this project chose is x86_64-lean_os and
# the underscore is part of the name.
#
# The anchor is a line that ends differently between vintages, so it is
# matched WITHOUT its tail: `| nsk* | powerunix* | genode* | zvmoe* |
# qnx* | emx*` is followed by ` | zephyr* \` in one copy and by `)` in
# another. Matching the shared prefix and re-emitting it is what lets one
# edit apply to a 2019 config.sub and a 2024 one.
# The two trees ship config.sub at different vintages and the list ends
# differently in each, which is why the anchor is the one line both have
# rather than the last line of either.
CONFIG_SUB_ANCHOR = "\t     | nsk* | powerunix* | genode* | zvmoe* | qnx* | emx*"
CONFIG_SUB_EDIT = ("\t     | lean_os* \\\n"
                   "\t     | nsk* | powerunix* | genode* | zvmoe* | qnx* | emx*")


def port_binutils(root):
    out = []
    out.append(("config.sub", edit(
        os.path.join(root, "config.sub"), CONFIG_SUB_ANCHOR, CONFIG_SUB_EDIT,
        "the list of operating system names config.sub accepts")))

    # Which object format the linker and assembler produce. lean_os is
    # ELF64 on x86-64, which is exactly what the -elf target already
    # produces, so this adds a name to that case rather than a case.
    out.append(("bfd/config.bfd", edit(
        os.path.join(root, "bfd/config.bfd"),
        "  x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia | x86_64-*-genode*)",
        "  x86_64-*-lean_os* | x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia | x86_64-*-genode*)",
        "the BFD target vector for x86-64 ELF")))

    out.append(("ld/configure.tgt", edit(
        os.path.join(root, "ld/configure.tgt"),
        "x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia* | x86_64-*-genode*)",
        "x86_64-*-lean_os* | x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia* | x86_64-*-genode*)",
        "the linker emulation for x86-64 ELF")))

    out.append(("gas/configure.tgt", edit(
        os.path.join(root, "gas/configure.tgt"),
        "  i386-*-elf*)\t\t\t\tfmt=elf ;;",
        "  i386-*-lean_os*)\t\t\tfmt=elf ;;\n  i386-*-elf*)\t\t\t\tfmt=elf ;;",
        "the assembler's object format")))
    return out


def port_gcc(root, header_src):
    out = []
    out.append(("config.sub", edit(
        os.path.join(root, "config.sub"), CONFIG_SUB_ANCHOR, CONFIG_SUB_EDIT,
        "the list of operating system names config.sub accepts")))

    # What the OS is, in general: GNU as and ld, and a C library that has
    # atexit rather than __cxa_atexit (see tools/toolchain-port/lean_os.h,
    # which says the same thing to the compiler and says why).
    out.append(("gcc/config.gcc (os)", edit(
        os.path.join(root, "gcc/config.gcc"),
        "*-*-phoenix*)\n  gas=yes\n  gnu_ld=yes\n  default_use_cxa_atexit=yes\n  ;;",
        "*-*-lean_os*)\n"
        "  gas=yes\n"
        "  gnu_ld=yes\n"
        "  default_use_cxa_atexit=no\n"
        "  use_gcc_stdint=provide\n"
        "  ;;\n"
        "*-*-phoenix*)\n  gas=yes\n  gnu_ld=yes\n  default_use_cxa_atexit=yes\n  ;;",
        "the per-OS block in config.gcc")))

    # And what the machine is: the same header list the bare x86-64 ELF
    # target uses, plus this project's own lean_os.h at the end so its
    # #undef/#define pairs win.
    out.append(("gcc/config.gcc (cpu)", edit(
        os.path.join(root, "gcc/config.gcc"),
        "x86_64-*-elf*)\n\ttm_file=\"${tm_file} i386/unix.h i386/att.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h\"\n\t;;",
        "x86_64-*-lean_os*)\n"
        "\ttm_file=\"${tm_file} i386/unix.h i386/att.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h lean_os.h\"\n"
        "\t;;\n"
        "x86_64-*-elf*)\n\ttm_file=\"${tm_file} i386/unix.h i386/att.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h\"\n\t;;",
        "the tm_file list for x86-64")))

    # libgcc: the same crtstuff the bare ELF target builds, and
    # crtbegin.o/crtend.o named in extra_parts.
    #
    # **extra_parts is the half that is easy to leave out**, and leaving
    # it out is not a build failure - libgcc builds, installs, and the
    # first `gcc hello.c` then says "cannot find crtbegin.o". The
    # tmake_file supplies the *rules* for building them and extra_parts
    # is what asks for them to be built. Both, or neither works.
    #
    # crti.o and crtn.o are deliberately NOT here: this project supplies
    # its own from the sysroot (user_space/lib/crti.asm), because _init
    # and _fini have to bracket what THIS runtime does. Named because
    # their absence is the kind of thing a reader checks for.
    out.append(("libgcc/config.host", edit(
        os.path.join(root, "libgcc/config.host"),
        "x86_64-*-elf* | x86_64-*-rtems*)\n\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"",
        "x86_64-*-lean_os*)\n"
        "\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"\n"
        "\textra_parts=\"crtbegin.o crtend.o\"\n"
        "\t;;\n"
        "x86_64-*-elf* | x86_64-*-rtems*)\n\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"",
        "libgcc's per-host crtstuff rules")))

    dst = os.path.join(root, "gcc/config/lean_os.h")
    shutil.copyfile(header_src, dst)
    out.append(("gcc/config/lean_os.h", "copied"))
    return out


def port_config_sub(path):
    """Teach one project's bundled config.sub about lean_os.

    Every autotools project ships its own copy of config.sub, usually
    older than the one in the toolchain, and it validates --host before
    doing anything else - so `./configure --host=x86_64-lean_os` stops at
    "OS `lean_os' not recognized" no matter how good the compiler is.

    Replacing a bundled config.sub is what every distribution does and
    what the autotools manual tells you to do; this does the smaller
    thing and adds one name to the copy that is there. Nothing
    third-party is vendored into this repository as a result, which is
    the reason it is an edit rather than a file.
    """
    return edit(path, CONFIG_SUB_ANCHOR, CONFIG_SUB_EDIT,
                "the list of operating system names config.sub accepts")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--config-sub":
        try:
            print("  %-40s %s" % (sys.argv[2], port_config_sub(sys.argv[2])))
        except MissingAnchor as e:
            print("toolchain-port: %s" % e, file=sys.stderr)
            return 1
        return 0
    if len(sys.argv) != 3:
        print("usage: apply.py <binutils-dir> <gcc-dir>\n"
              "       apply.py --config-sub <path/to/config.sub>",
              file=sys.stderr)
        return 2
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        for name, status in port_binutils(sys.argv[1]):
            print("  binutils %-24s %s" % (name, status))
        for name, status in port_gcc(sys.argv[2],
                                     os.path.join(here, "lean_os.h")):
            print("  gcc      %-24s %s" % (name, status))
    except MissingAnchor as e:
        print("toolchain-port: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
