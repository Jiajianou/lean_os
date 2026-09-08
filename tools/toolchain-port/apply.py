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


def write_file(path, content, why):
    """Creates a file the upstream tree does not have. Idempotent the
    same way edit() is: content already in place means already applied,
    different content in place is replaced (the port owns this file -
    nothing upstream will ever write it)."""
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == content:
                return "already applied"
    with open(path, "w") as f:
        f.write(content)
    return "applied"


def edit(path, anchor, replacement, why, count=1):
    """Replaces the first `count` occurrences of `anchor` (all of them when
    count is 0). Fails loudly.

    M97 added the count: libtool's case statements appear TWICE in a
    generated configure - once for the C tag and once for C++ - and
    editing only the first leaves the C++ half answering "no dynamic
    linker", which does not fail, it just quietly builds no shared
    library. One edit, both copies."""
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
# M100: and the 2024-05 vintage, which freetype 2.13.3 ships. config.sub
# rewrote its OS list as one name per line, alphabetically, so the
# six-names-on-a-line anchor above is not in it at all. Same edit, second
# attachment point: the `nsk*` line, which every vintage has. A shell
# `case` pattern list is unordered, so where the name lands in the list
# is cosmetic; it goes before nsk* so both edits read the same.
CONFIG_SUB_ANCHOR_2024 = "\t| nsk* \\\n"
CONFIG_SUB_EDIT_2024 = ("\t| lean_os* \\\n"
                        "\t| nsk* \\\n")


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

    # The linker emulation - lean_os's OWN, not a name added to the
    # generic -elf case. M94 shared elf_x86_64 and that held until M98
    # ran `ld hello.o` with no flags ON the machine: the generic
    # emulation's TEXT_START_ADDR is Linux's 0x400000, which is outside
    # the window kernel/proc/elf.c will load, so a bare ld produced
    # programs the kernel truthfully refused. The driver's -T lean_os.ld
    # never sees this default; a person or a build system invoking ld
    # directly gets it. M94's rule decides where the fix goes: the load
    # address comes from the target description, and for bare ld the
    # target description IS the emulation.
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
    # The generated-emulation lists, so make knows eelf_x86_64_lean_os.c
    # exists to be generated - the e%.c pattern rule does the rest.
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

    # What the OS is, in general: GNU as and ld, and - since M97 - a C
    # library that has __cxa_atexit.
    #
    # M94 set this to `no`, correctly at the time: nothing here had
    # __cxa_atexit, and GCC's fallback (one destructor per translation
    # unit, registered with plain atexit) is fine for C. It is not fine
    # for C++ in a program with shared objects, and that is M97's
    # problem: the fallback has nowhere to record WHICH shared object a
    # static object belongs to, so dlclose either runs nothing or runs
    # everything. The three-argument form carries the handle, which is
    # the only reason it has three arguments.
    out.append(("gcc/config.gcc (os)", edit(
        os.path.join(root, "gcc/config.gcc"),
        "*-*-phoenix*)\n  gas=yes\n  gnu_ld=yes\n  default_use_cxa_atexit=yes\n  ;;",
        "*-*-lean_os*)\n"
        "  gas=yes\n"
        "  gnu_ld=yes\n"
        "  default_use_cxa_atexit=yes\n"
        # M97: which threading model the compiler and the C++ runtime
        # assume. `posix` means <pthread.h>, which this libc has since
        # M79 and M96 - gthr-posix.h needs pthread_create, the mutex and
        # condition-variable pairs, pthread_key_create/getspecific and
        # pthread_once, and all of them are here. Without it std::thread
        # does not exist and std::mutex is a no-op that silently does not
        # exclude, which is the worse of the two failures.
        "  thread_file=posix\n"
        "  use_gcc_stdint=provide\n"
        # M99: and the driver option that says a program uses threads.
        #
        # `-pthread` is not a generic GCC option - it is declared per-OS,
        # in that OS's .opt file, and a target which does not declare it
        # answers `unrecognized command-line option '-pthread'`. That is
        # not a cosmetic refusal: CPython's configure link-tests EVERY
        # libc function with `$CC -pthread`, so on this target every one
        # of four hundred probes failed and configure concluded the C
        # library had no snprintf, no waitpid and no uname.
        #
        # lean_os.opt declares it; lean_os.h's CPP_SPEC turns it into
        # -D_REENTRANT. It adds no -lpthread because there is no such
        # archive here: pthread_create is in libc (M79/M96), which is
        # the same arrangement glibc has had since 2.34 and the reason
        # `Ignore` is what several targets in this table use.
        "  extra_options=\"${extra_options} lean_os.opt\"\n"
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
        # M97: t-slibgcc and friends, which build libgcc_s.so.
        #
        # ONE unwinder, and that is the entire reason. libgcc's exception
        # machinery keeps a static registry of the .eh_frame tables it
        # has been told about; link libgcc.a into two objects and there
        # are two registries. The shared library registers its frames in
        # its copy, the executable throws through its own, and the throw
        # finds no handler and calls std::terminate - which presents as
        # an abort with no message from a program whose single-object
        # exception tests all pass.
        #
        # t-slibgcc supplies the shared-library rules, t-slibgcc-gld the
        # GNU-ld version-script form, and t-slibgcc-elf-ver the symbol
        # versioning that goes in it. The three go together on every ELF
        # target that uses GNU ld, and this is one.
        #
        # NOT t-eh-dw2-dip, which is the other way to find an FDE:
        # dl_iterate_phdr over the loaded objects, reading each one's
        # PT_GNU_EH_FRAME. It needs <elf.h> and a dl_iterate_phdr, and
        # this system has neither - the loader is M95's and does not
        # publish its object list. The registry is the older mechanism
        # and the one this target uses: crtbeginS's frame_dummy runs from
        # a shared object's own .init_array and hands its table to
        # libgcc_s, which is a thing the loader already does for every
        # object it maps.
        "\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic"
        " t-slibgcc t-slibgcc-gld t-slibgcc-elf-ver\"\n"
        # M97: crtbeginS.o and crtendS.o, the -fPIC pair, as well.
        #
        # A shared object needs its own copy of the frame-registration
        # code that crtbegin does for an executable: it has its own
        # .eh_frame, and an exception thrown inside it can only be
        # unwound if something told the registry where that table is.
        # crtbeginS's frame_dummy runs from the object's own
        # .init_array, which this project's loader already walks for
        # every object it maps (user_space/ld/ld-lean.c).
        #
        # The rules for them already exist - t-crtstuff-pic is in the
        # tmake_file above and has been since M94. Only the request was
        # missing, which is the same half M94's own note says is easy to
        # leave out.
        "\textra_parts=\"crtbegin.o crtend.o crtbeginS.o crtendS.o\"\n"
        "\t;;\n"
        "x86_64-*-elf* | x86_64-*-rtems*)\n\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"",
        "libgcc's per-host crtstuff rules")))

    # M97: libstdc++'s configure, and a link test that is a hard error.
    #
    # libstdc++-v3/configure.ac calls GCC_NO_EXECUTABLES for every cross
    # target that is not a named special case, after which any link test
    # is a fatal error rather than a failed probe - the build stops with
    # "Link tests are not allowed after GCC_NO_EXECUTABLES". libtool's
    # dlopen probe is a link test, and it does not know that: its case on
    # $host_os (which for a target library IS the target) falls through
    # to a default that link-tests for `shl_load`.
    #
    # A `lean_os*)` case that leaves lt_cv_dlopen at its already-assigned
    # `no` is the whole fix, and it is a true statement rather than a
    # workaround: **libstdc++ on this target does not dlopen anything.**
    # M95 gives a *program* dlopen; what this switch controls is whether
    # the C++ runtime loads pieces of itself at runtime, which on a
    # target with one locale and a static libsupc++ it does not.
    #
    # The generated configure is edited rather than libtool.m4, for the
    # same reason config.sub is edited directly: regenerating it means
    # running the exact autoconf these trees were built with, and the
    # edit is one case label.
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

    # M97: and the target table libstdc++ refuses to build without.
    #
    # crossconfig.m4 ends in `*) AC_MSG_ERROR([No support for this
    # host/target combination.])`, so an unlisted target does not get a
    # default - it gets a configure that stops. What each case there
    # actually does is decide which math and stdlib probes to run, and
    # every one of those probes is a link test, which GCC_NO_EXECUTABLES
    # has already forbidden.
    #
    # So this target is modelled on `*-fuchsia*`, which is the case that
    # asks for nothing except function and data sections - and asking for
    # nothing is right here: a freestanding libsupc++ uses no libm and
    # calls almost nothing from stdlib, and claiming otherwise would be
    # claiming it on this project's behalf about a libc it is still
    # filling in. The day a hosted libstdc++ is built here, this case is
    # where the answer changes and it will be a list of things measured
    # rather than assumed.
    out.append(("libstdc++-v3/configure (target)", edit(
        os.path.join(root, "libstdc++-v3/configure"),
        "  *-fuchsia*)\n"
        "    SECTION_FLAGS='-ffunction-sections -fdata-sections'\n"
        "\n"
        "    ;;",
        "  *-lean_os*)\n"
        "    SECTION_FLAGS='-ffunction-sections -fdata-sections'\n"
        "\n"
        "    ;;\n"
        "\n"
        "  *-fuchsia*)\n"
        "    SECTION_FLAGS='-ffunction-sections -fdata-sections'\n"
        "\n"
        "    ;;",
        "the per-target case list libstdc++ refuses to build without")))

    # M97: and what a shared library LOOKS like on this system.
    #
    # libtool's two big `case $host_os` switches - one for how to check a
    # dependent library, one for the dynamic linker's characteristics -
    # have no entry for this target, so `checking dynamic linker
    # characteristics` answers `no` and libtool then declines to build a
    # shared library at all. `--enable-shared` is accepted and quietly
    # does nothing, which is the worst shape a configuration failure can
    # take.
    #
    # Both cases are answered the way linux* is, and every clause of that
    # answer is a true statement about M95's loader rather than a guess:
    # it reads DT_SONAME, it searches /lib, it honours LD_LIBRARY_PATH,
    # and a `libfoo.so.1` name with a `libfoo.so` development link is the
    # scheme it resolves. What it does NOT have is ldconfig or a cache,
    # which is what shlibpath_overrides_runpath=no says.
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

    # M99: the option table for this OS. One entry, and the format is
    # the one options.texi documents - a name, then what the driver does
    # with it. `Driver` rather than `Ignore` because CPP_SPEC in
    # lean_os.h reads it back to define _REENTRANT.
    out.append(("gcc/config/lean_os.opt", write_file(
        os.path.join(root, "gcc/config/lean_os.opt"),
        "; lean_os options. See gcc/config/lean_os.h for what they do.\n"
        "; Written by tools/toolchain-port/apply.py - M99.\n"
        "\n"
        "pthread\n"
        "Driver\n",
        "the driver options this OS declares")))

    # And the documentation-URL sidecar GCC 14 requires beside every .opt.
    #
    # Not optional and not cosmetic: gcc/Makefile.in has `s-options`
    # depend on `$(ALL_OPT_FILES:.opt=.opt.urls)`, so an .opt with no
    # .opt.urls stops the build with "No rule to make target
    # .../lean_os.opt.urls" - after config.gcc has been read, which is
    # late enough to look like a makefile bug rather than a missing file.
    # Upstream generates these from the built HTML manual with
    # regenerate-opt-urls.py; the honest content for this one is what
    # rtems.opt.urls has for the same option, which is a comment saying
    # the manual documents -pthread in two places and neither wins.
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
    why = "the list of operating system names config.sub accepts"
    try:
        return edit(path, CONFIG_SUB_ANCHOR, CONFIG_SUB_EDIT, why)
    except MissingAnchor:
        # Not the six-per-line vintage; try the one-per-line one. If
        # that anchor is missing too, its error is the one reported.
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
