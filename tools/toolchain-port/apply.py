#!/usr/bin/env python3
import sys
import os
import shutil

class MissingAnchor(Exception):
    pass

def write_file(path, content, why):
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == content:
                return "already applied"
    with open(path, "w") as f:
        f.write(content)
    return "applied"

def edit(path, anchor, replacement, why, count=1):
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
        f.write(text.replace(anchor, replacement, -1 if count == 0 else count))
    return "applied"

CONFIG_SUB_ANCHOR = "\t     | nsk* | powerunix* | genode* | zvmoe* | qnx* | emx*"
CONFIG_SUB_EDIT = ("\t     | lean_os* \\\n"
                   "\t     | nsk* | powerunix* | genode* | zvmoe* | qnx* | emx*")
CONFIG_SUB_ANCHOR_2024 = "\t| nsk* \\\n"
CONFIG_SUB_EDIT_2024 = ("\t| lean_os* \\\n"
                        "\t| nsk* \\\n")

def port_binutils(root):
    out = []
    out.append(("config.sub", edit(
        os.path.join(root, "config.sub"), CONFIG_SUB_ANCHOR, CONFIG_SUB_EDIT,
        "the list of operating system names config.sub accepts")))

    out.append(("bfd/config.bfd", edit(
        os.path.join(root, "bfd/config.bfd"),
        "  x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia | x86_64-*-genode*)",
        "  x86_64-*-lean_os* | x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia | x86_64-*-genode*)",
        "the BFD target vector for x86-64 ELF")))

    out.append(("ld/emulparams", write_file(
        os.path.join(root, "ld/emulparams/elf_x86_64_lean_os.sh"),
        "source_sh ${srcdir}/emulparams/elf_x86_64.sh\n"
        "# Where a lean_os process image lives: PML4[1], 512 GiB - the\n"
        "# same address user_space/lib/user.ld sets, kept in step by\n"
        "# M98's boot self-test linking with no script at all.\n"
        "TEXT_START_ADDR=0x8000000000\n"
        "# This machine has 4 KiB pages and no transparent huge ones, so\n"
        "# aligning segments to the generic 2 MiB would buy nothing and\n"
        "# cost address-space sprawl in every bare-ld binary.\n"
        "MAXPAGESIZE=0x1000\n"
        "COMMONPAGESIZE=0x1000\n",
        "the lean_os linker emulation")))
    out.append(("ld/configure.tgt", edit(
        os.path.join(root, "ld/configure.tgt"),
        "x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia* | x86_64-*-genode*)",
        "x86_64-*-lean_os*)\ttarg_emul=elf_x86_64_lean_os\n"
        "\t\t\t;;\n"
        "x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia* | x86_64-*-genode*)",
        "the linker emulation for x86-64 ELF")))
    for mf in ("ld/Makefile.am", "ld/Makefile.in"):
        out.append((mf, edit(
            os.path.join(root, mf),
            "\teelf_x86_64_haiku.c \\\n",
            "\teelf_x86_64_haiku.c \\\n\teelf_x86_64_lean_os.c \\\n",
            "the 64-bit emulation source list")))

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

    out.append(("gcc/config.gcc (os)", edit(
        os.path.join(root, "gcc/config.gcc"),
        "*-*-phoenix*)\n  gas=yes\n  gnu_ld=yes\n  default_use_cxa_atexit=yes\n  ;;",
        "*-*-lean_os*)\n"
        "  gas=yes\n"
        "  gnu_ld=yes\n"
        "  default_use_cxa_atexit=yes\n"
        "  thread_file=posix\n"
        "  use_gcc_stdint=provide\n"
        "  extra_options=\"${extra_options} lean_os.opt\"\n"
        "  ;;\n"
        "*-*-phoenix*)\n  gas=yes\n  gnu_ld=yes\n  default_use_cxa_atexit=yes\n  ;;",
        "the per-OS block in config.gcc")))

    out.append(("gcc/config.gcc (cpu)", edit(
        os.path.join(root, "gcc/config.gcc"),
        "x86_64-*-elf*)\n\ttm_file=\"${tm_file} i386/unix.h i386/att.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h\"\n\t;;",
        "x86_64-*-lean_os*)\n"
        "\ttm_file=\"${tm_file} i386/unix.h i386/att.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h lean_os.h\"\n"
        "\t;;\n"
        "x86_64-*-elf*)\n\ttm_file=\"${tm_file} i386/unix.h i386/att.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h\"\n\t;;",
        "the tm_file list for x86-64")))

    out.append(("libgcc/config.host", edit(
        os.path.join(root, "libgcc/config.host"),
        "x86_64-*-elf* | x86_64-*-rtems*)\n\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"",
        "x86_64-*-lean_os*)\n"
        "\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic"
        " t-slibgcc t-slibgcc-gld t-slibgcc-elf-ver\"\n"
        "\textra_parts=\"crtbegin.o crtend.o crtbeginS.o crtendS.o\"\n"
        "\t;;\n"
        "x86_64-*-elf* | x86_64-*-rtems*)\n\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"",
        "libgcc's per-host crtstuff rules")))

    out.append(("libstdc++-v3/configure (dlopen)", edit(
        os.path.join(root, "libstdc++-v3/configure"),
        "  case $host_os in\n"
        "  beos*)\n"
        "    lt_cv_dlopen=\"load_add_on\"",
        "  case $host_os in\n"
        "  lean_os*)\n"
        "    ;;\n"
        "  beos*)\n"
        "    lt_cv_dlopen=\"load_add_on\"",
        "libtool's dlopen probe, which is a link test")))

    out.append(("libstdc++-v3/configure (target)", edit(
        os.path.join(root, "libstdc++-v3/configure"),
        "  *-fuchsia*)\n"
        "    SECTION_FLAGS='-ffunction-sections -fdata-sections'\n"
        "\n"
        "    ;;",
        "  *-lean_os*)\n"
        "    SECTION_FLAGS='-ffunction-sections -fdata-sections'\n"
        "\n"
        "    $as_echo \"#define HAVE_FABSF 1\" >>confdefs.h\n"
        "    $as_echo \"#define HAVE_FLOORF 1\" >>confdefs.h\n"
        "    $as_echo \"#define HAVE_CEILF 1\" >>confdefs.h\n"
        "    $as_echo \"#define HAVE_SINF 1\" >>confdefs.h\n"
        "    $as_echo \"#define HAVE_COSF 1\" >>confdefs.h\n"
        "    $as_echo \"#define HAVE_TANF 1\" >>confdefs.h\n"
        "    $as_echo \"#define HAVE_ATANF 1\" >>confdefs.h\n"
        "    $as_echo \"#define HAVE_SQRTF 1\" >>confdefs.h\n"
        "    $as_echo \"#define HAVE_HYPOTF 1\" >>confdefs.h\n"
        "\n"
        "    ;;\n"
        "\n"
        "  *-fuchsia*)\n"
        "    SECTION_FLAGS='-ffunction-sections -fdata-sections'\n"
        "\n"
        "    ;;",
        "the per-target case list libstdc++ refuses to build without")))

    out.append(("libstdc++-v3/configure (deplibs)", edit(
        os.path.join(root, "libstdc++-v3/configure"),
        "linux* | k*bsd*-gnu | kopensolaris*-gnu | uclinuxfdpiceabi)\n"
        "  lt_cv_deplibs_check_method=pass_all\n"
        "  ;;",
        "lean_os* | linux* | k*bsd*-gnu | kopensolaris*-gnu | uclinuxfdpiceabi)\n"
        "  lt_cv_deplibs_check_method=pass_all\n"
        "  ;;",
        "libtool's how-to-check-a-dependent-library switch", 0)))

    out.append(("libstdc++-v3/configure (dynamic linker)", edit(
        os.path.join(root, "libstdc++-v3/configure"),
        "linux* | k*bsd*-gnu | kopensolaris*-gnu | gnu* | uclinuxfdpiceabi)",
        "lean_os* | linux* | k*bsd*-gnu | kopensolaris*-gnu | gnu* | uclinuxfdpiceabi)",
        "libtool's dynamic-linker-characteristics switch", 0)))

    dst = os.path.join(root, "gcc/config/lean_os.h")
    shutil.copyfile(header_src, dst)
    out.append(("gcc/config/lean_os.h", "copied"))

    out.append(("gcc/config/lean_os.opt", write_file(
        os.path.join(root, "gcc/config/lean_os.opt"),
        "; lean_os options. See gcc/config/lean_os.h for what they do.\n"
        "; Written by tools/toolchain-port/apply.py - M99.\n"
        "\n"
        "pthread\n"
        "Driver\n",
        "the driver options this OS declares")))

    out.append(("gcc/config/lean_os.opt.urls", write_file(
        os.path.join(root, "gcc/config/lean_os.opt.urls"),
        "; Written by tools/toolchain-port/apply.py - M99. Upstream\n"
        "; autogenerates these from the HTML manual; -pthread is\n"
        "; documented in two places there and rtems.opt.urls records the\n"
        "; same standoff.\n"
        "\n"
        "; skipping UrlSuffix for 'pthread' due to multiple URLs\n",
        "the documentation-URL sidecar GCC 14 requires beside every .opt")))
    return out

def port_config_sub(path):
    why = "the list of operating system names config.sub accepts"
    try:
        return edit(path, CONFIG_SUB_ANCHOR, CONFIG_SUB_EDIT, why)
    except MissingAnchor:
        return edit(path, CONFIG_SUB_ANCHOR_2024, CONFIG_SUB_EDIT_2024, why)

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
