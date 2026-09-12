#!/usr/bin/env python3
"""tools/clang-port/apply.py - M121: teach LLVM and clang about lean_os.

---- why a script of anchored edits rather than a .patch series ----------

The same reason tools/toolchain-port/apply.py gives, and that file is the
one to read for the argument. In one line: llvm-project is downloaded by
tools/build-clang.sh, unpacked into build/, and thrown away - nothing in
this repository claims to hold a copy of it - so what has to survive is
*the edit*, stated so a person can read it against the upstream file. An
anchored replacement fails with the name of the anchor it could not find,
which is the actual news; a context diff fails on the next release for a
reason that has nothing to do with the edit.

---- what these edits are, and what they are not ------------------------

Six edits and two files, and every one of them is the "add an OS to a
compiler" recipe: a name in an enum, a name in two string tables, a
TargetInfo saying which macros the preprocessor defines, a ToolChain
saying which files a program links, and the two lines that reach them.
Nothing here touches a pass, an optimisation, or a code-generation
decision - M63's rule, and the same line M94 drew for GCC.

The two files are tools/clang-port/LeanOS.h and LeanOS.cpp, which are
this project's own source, kept here rather than in the download.
"""
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
    """Replaces the first `count` occurrences of `anchor` (all of them when
    count is 0). Fails loudly, and is idempotent the same way the GCC
    port's edit() is: the replacement already being present means already
    applied."""
    with open(path) as f:
        text = f.read()
    if replacement in text:
        return "already applied"
    if anchor not in text:
        raise MissingAnchor(
            "%s: could not find the anchor for '%s'.\n"
            "The upstream file has moved. The edit itself is still what it\n"
            "was; what needs updating is where it attaches.\n"
            "Anchor: %r" % (path, why, anchor[:160]))
    with open(path, "w") as f:
        f.write(text.replace(anchor, replacement, -1 if count == 0 else count))
    return "applied"


# ---- 1. the OS, in llvm::Triple ----------------------------------------
#
# Appended at the end of OSType rather than inserted alphabetically, and
# LastOSType moves with it. That is the convention for adding an OS and it
# is not only convention: clang/include/clang/Basic/DarwinSDKInfo.h packs
# a pair of OSTypes into one integer using LastOSType as the radix, so the
# value has to be the real maximum - and nothing in either tree depends on
# where in the list a name sits.
#
# `lean_os` with the underscore, because that is the triple this project
# chose in M94 and the underscore is part of the name.
TRIPLE_H_ANCHOR = """    Serenity,
    Vulkan, // Vulkan SPIR-V
    LastOSType = Vulkan"""
TRIPLE_H_EDIT = """    Serenity,
    Vulkan, // Vulkan SPIR-V
    LeanOS, // lean_os
    LastOSType = LeanOS"""

# The two string tables that turn the enum into a name and a name into the
# enum. ELF comes free: getDefaultFormat's x86_64 case answers ELF for
# every OS that is not Windows, UEFI or Darwin, so there is no third table
# to edit.
TRIPLE_CPP_NAME_ANCHOR = '  case Serenity: return "serenity";'
TRIPLE_CPP_NAME_EDIT = ('  case Serenity: return "serenity";\n'
                        '  case LeanOS: return "lean_os";')

TRIPLE_CPP_PARSE_ANCHOR = '    .StartsWith("serenity", Triple::Serenity)'
TRIPLE_CPP_PARSE_EDIT = ('    .StartsWith("serenity", Triple::Serenity)\n'
                         '    .StartsWith("lean_os", Triple::LeanOS)')

# ---- 2. what the preprocessor defines ----------------------------------
#
# The same four names and two assertions tools/toolchain-port/lean_os.h
# gives GCC, for the same reason: `__lean_os__` is what the toybox port
# keys on, and `__unix__` is what a configure script tests before it tests
# anything more specific. DefineStd is clang's spelling of the
# __unix/__unix__/unix triple, and it is what every other hosted target
# here uses.
#
# _REENTRANT under -pthread matches lean_os.h's CPP_SPEC. _GNU_SOURCE
# under C++ is what Fuchsia and Linux both do and what libc++'s locale
# support needs; it is a request to this project's own headers, which is
# the only libc clang will find here.
OSTARGETS_ANCHOR = """// Fuchsia Target
template <typename Target>
class LLVM_LIBRARY_VISIBILITY FuchsiaTargetInfo : public OSTargetInfo<Target> {"""
OSTARGETS_EDIT = """// lean_os Target - M121. The counterpart of
// tools/toolchain-port/lean_os.h's TARGET_OS_CPP_BUILTINS, and it defines
// the same names for the same reasons: __lean_os__ is what this project's
// own ports key on, __unix__ is what a configure script asks first.
template <typename Target>
class LLVM_LIBRARY_VISIBILITY LeanOSTargetInfo : public OSTargetInfo<Target> {
protected:
  void getOSDefines(const LangOptions &Opts, const llvm::Triple &Triple,
                    MacroBuilder &Builder) const override {
    Builder.defineMacro("__lean_os__");
    Builder.defineMacro("__lean_os");
    DefineStd(Builder, "unix", Opts);
    if (Opts.POSIXThreads)
      Builder.defineMacro("_REENTRANT");
    if (Opts.CPlusPlus)
      Builder.defineMacro("_GNU_SOURCE");
  }

public:
  LeanOSTargetInfo(const llvm::Triple &Triple, const TargetOptions &Opts)
      : OSTargetInfo<Target>(Triple, Opts) {
    this->WIntType = TargetInfo::UnsignedInt;
    // The Itanium C++ ABI, which is what M97's libsupc++ and M121's
    // libc++abi both implement and what every ELF target here uses.
    this->TheCXXABI.set(TargetCXXABI::GenericItanium);
  }
};

// Fuchsia Target
template <typename Target>
class LLVM_LIBRARY_VISIBILITY FuchsiaTargetInfo : public OSTargetInfo<Target> {"""

# ---- 3. and the two lines that reach the new classes -------------------
TARGETS_CPP_ANCHOR = """    case llvm::Triple::Hurd:
      return std::make_unique<HurdTargetInfo<X86_64TargetInfo>>(Triple, Opts);"""
TARGETS_CPP_EDIT = """    case llvm::Triple::Hurd:
      return std::make_unique<HurdTargetInfo<X86_64TargetInfo>>(Triple, Opts);
    case llvm::Triple::LeanOS:
      return std::make_unique<LeanOSTargetInfo<X86_64TargetInfo>>(Triple,
                                                                  Opts);"""

DRIVER_CPP_INC_ANCHOR = '#include "ToolChains/Hurd.h"\n'
DRIVER_CPP_INC_EDIT = '#include "ToolChains/Hurd.h"\n#include "ToolChains/LeanOS.h"\n'

DRIVER_CPP_ANCHOR = """    case llvm::Triple::Fuchsia:
      TC = std::make_unique<toolchains::Fuchsia>(*this, Target, Args);
      break;"""
DRIVER_CPP_EDIT = """    case llvm::Triple::Fuchsia:
      TC = std::make_unique<toolchains::Fuchsia>(*this, Target, Args);
      break;
    case llvm::Triple::LeanOS:
      TC = std::make_unique<toolchains::LeanOS>(*this, Target, Args);
      break;"""

CMAKE_ANCHOR = "  ToolChains/Hurd.cpp\n"
CMAKE_EDIT = "  ToolChains/Hurd.cpp\n  ToolChains/LeanOS.cpp\n"

# ---- 4. which extended-locale API this libc has ------------------------
#
# libc++'s <locale> needs a `locale_t` and about forty `*_l` functions.
# locale_base_api.h is the switch that says where a platform's come from,
# and the choice for lean_os is the same one Fuchsia makes: the inline
# fallbacks libc++ ships, which take a locale_t and ignore it.
#
# That is a TRUE statement about this machine rather than a shortcut, and
# user_space/libc/include/locale.h has been making it since M89: there is
# one locale here and it is "C", so `newlocale` returns the one static
# object and `uselocale` has nothing to swap. A libc++ built against the
# generic glibc path would instead call forty functions like `strtold_l`
# and `snprintf_l` that do not exist, to pass them a locale that cannot
# differ from the default - which would be forty functions of pretence,
# and M65's rule is the one that decides it.
#
# What this libc does supply and the fallbacks need is `locale_t`,
# `newlocale`, `uselocale`, `duplocale` and `freelocale` (M89), plus the
# plain ctype, strto* and wchar functions (M80, M99, M100).
LOCALE_API_ANCHOR = """#elif defined(__Fuchsia__)
#  include <__locale_dir/locale_base_api/fuchsia.h>"""
LOCALE_API_EDIT = """#elif defined(__Fuchsia__)
#  include <__locale_dir/locale_base_api/fuchsia.h>
#elif defined(__lean_os__)
// M121: this libc has one locale and it is "C" - see
// user_space/libc/include/locale.h, and tools/clang-port/apply.py for
// why the inline fallbacks are the honest answer rather than the easy one.
#  include <__locale_dir/locale_base_api/lean_os.h>"""

# And the header that switch names. It is Fuchsia's, byte for byte, under
# a name of its own - because a `#include .../fuchsia.h` in a lean_os
# build would be a reader's question every time, and the two platforms
# agreeing today is not a promise that they will.
LOCALE_API_HEADER = """// -*- C++ -*-
//===-----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Written by tools/clang-port/apply.py - M121.
//
// lean_os has one locale and it is "C" (user_space/libc/include/locale.h,
// M89), so the extended-locale functions are the inline fallbacks libc++
// ships: they take a locale_t and ignore it, which is what a machine with
// one locale means. The `locale_t` itself, and newlocale/uselocale/
// duplocale/freelocale, come from this libc.
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___LOCALE_LOCALE_BASE_API_LEAN_OS_H
#define _LIBCPP___LOCALE_LOCALE_BASE_API_LEAN_OS_H

// <locale.h> FIRST, and this is where lean_os differs from Fuchsia.
// Every one of the forty fallbacks below takes a `locale_t`, and that
// type comes from this libc's <locale.h> (M89). Fuchsia's copy of this
// header gets the name from <cstdlib>, because its libc declares it
// there; ours does not, and the failure is forty copies of "unknown type
// name 'locale_t'" from a file that never mentions where it should have
// come from.
#include <locale.h>

#include <__support/xlocale/__posix_l_fallback.h>
#include <__support/xlocale/__strtonum_fallback.h>
#include <cstdlib>
#include <cwchar>

#endif // _LIBCPP___LOCALE_LOCALE_BASE_API_LEAN_OS_H
"""


def port_llvm(root):
    out = []
    out.append(("llvm/.../Triple.h", edit(
        os.path.join(root, "llvm/include/llvm/TargetParser/Triple.h"),
        TRIPLE_H_ANCHOR, TRIPLE_H_EDIT,
        "the OSType enum llvm::Triple parses into")))
    tcpp = os.path.join(root, "llvm/lib/TargetParser/Triple.cpp")
    out.append(("llvm/.../Triple.cpp (name)", edit(
        tcpp, TRIPLE_CPP_NAME_ANCHOR, TRIPLE_CPP_NAME_EDIT,
        "the enum-to-name table")))
    out.append(("llvm/.../Triple.cpp (parse)", edit(
        tcpp, TRIPLE_CPP_PARSE_ANCHOR, TRIPLE_CPP_PARSE_EDIT,
        "the name-to-enum table")))
    return out


def port_clang(root, port_dir):
    out = []
    out.append(("clang/.../OSTargets.h", edit(
        os.path.join(root, "clang/lib/Basic/Targets/OSTargets.h"),
        OSTARGETS_ANCHOR, OSTARGETS_EDIT,
        "the per-OS TargetInfo classes")))
    out.append(("clang/.../Targets.cpp", edit(
        os.path.join(root, "clang/lib/Basic/Targets.cpp"),
        TARGETS_CPP_ANCHOR, TARGETS_CPP_EDIT,
        "the x86-64 target-allocation switch")))
    dcpp = os.path.join(root, "clang/lib/Driver/Driver.cpp")
    out.append(("clang/.../Driver.cpp (include)", edit(
        dcpp, DRIVER_CPP_INC_ANCHOR, DRIVER_CPP_INC_EDIT,
        "the ToolChains include block")))
    out.append(("clang/.../Driver.cpp (dispatch)", edit(
        dcpp, DRIVER_CPP_ANCHOR, DRIVER_CPP_EDIT,
        "getToolChain's per-OS switch")))
    out.append(("clang/.../CMakeLists.txt", edit(
        os.path.join(root, "clang/lib/Driver/CMakeLists.txt"),
        CMAKE_ANCHOR, CMAKE_EDIT,
        "the driver's source list")))

    # And this project's own two files, copied rather than patched in -
    # they are 500 lines of ours, not an edit to 500 lines of theirs.
    dst = os.path.join(root, "clang/lib/Driver/ToolChains")
    for name in ("LeanOS.h", "LeanOS.cpp"):
        shutil.copyfile(os.path.join(port_dir, name), os.path.join(dst, name))
        out.append(("clang/.../%s" % name, "copied"))
    return out


# ---- 5. and message catalogues, which this system does not have --------
#
# libc++'s <locale> assumes `catopen` on anything that defines __unix__,
# with a three-name exception list and a comment saying what the list is:
# "Most unix variants have catopen. These are the specific ones that
# don't." lean_os is a fourth, so it joins the list - which is why this
# is a one-line configuration edit rather than the alternative.
#
# The alternative was to write a <nl_types.h> here, and it was rejected
# on M65's rule. A catopen that always fails and a catgets that always
# returns its default argument is the correct behaviour for a system with
# no catalogues - but it is also a header, four functions and a type that
# exist so that one `#include` in somebody else's file succeeds, and
# nothing on this machine would ever call them. std::messages degrades on
# this target exactly the way it does on Android, for the same reason and
# by the same mechanism.
CATOPEN_ANCHOR = """#    if !defined(__BIONIC__) && !defined(_NEWLIB_VERSION) && !defined(__EMSCRIPTEN__)"""
CATOPEN_EDIT = """#    if !defined(__BIONIC__) && !defined(_NEWLIB_VERSION) && !defined(__EMSCRIPTEN__) && \\
        !defined(__lean_os__)"""


# ---- 6. and that this machine has a monotonic clock --------------------
#
# libc++'s chrono.cpp decides whether `steady_clock` can be built at all,
# and its gate is `_POSIX_TIMERS > 0` plus a list of three systems that
# have a monotonic clock without the rest of the POSIX timers option. The
# comment above that list describes lean_os exactly: OpenBSD "does not
# have a fully conformant suite of POSIX timers, but it does have
# clock_gettime and CLOCK_MONOTONIC which is all we need."
#
# So lean_os joins the list, and does NOT get `_POSIX_TIMERS` defined in
# its own <unistd.h> - which was the other way to fix this and would have
# been a claim about timer_create, timer_settime and six other functions
# this libc does not have. user_space/libc/include/time.h has had
# clock_gettime and CLOCK_MONOTONIC since M63 (SYS_uptime_ms underneath),
# and that is the whole of what this edit asserts.
#
# Without it the build stops at `#error "Monotonic clock not implemented
# on this platform"`, which is the right error and the wrong conclusion.
CHRONO_ANCHOR = """#if defined(__APPLE__) || defined(__gnu_hurd__) || defined(__OpenBSD__) || (defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0)"""
CHRONO_EDIT = """#if defined(__APPLE__) || defined(__gnu_hurd__) || defined(__OpenBSD__) || \\
    defined(__lean_os__) || (defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0)"""


def port_libcxx(root):
    """The runtime half: which extended-locale API libc++ finds here.

    Applied by the same script and at the same time as the compiler edits,
    even though tools/build-libcxx.sh is a separate step - one port, one
    place to read it, and an unpacked tree is either ported or it is not.
    """
    out = []
    base = os.path.join(root, "libcxx/include/__locale_dir/locale_base_api")
    out.append(("libcxx/.../lean_os.h", write_file(
        os.path.join(base, "lean_os.h"), LOCALE_API_HEADER,
        "the extended-locale API for a machine with one locale")))
    out.append(("libcxx/.../locale_base_api.h", edit(
        os.path.join(root, "libcxx/include/__locale_dir/locale_base_api.h"),
        LOCALE_API_ANCHOR, LOCALE_API_EDIT,
        "the per-platform locale-API switch")))

    # And the two lists that decide whether a header in that directory is
    # actually a header. libcxx does not install its include tree by
    # copying it: CMakeLists.txt names every file, one per line, and
    # anything not on the list is not copied into the build's own include
    # directory - so the #include above failed with "file not found" for a
    # file sitting right beside the one that includes it. The modulemap is
    # the same fact for a modules build; it costs one line and a build
    # configured with modules would otherwise fail much later.
    out.append(("libcxx/.../CMakeLists.txt", edit(
        os.path.join(root, "libcxx/include/CMakeLists.txt"),
        "  __locale_dir/locale_base_api/fuchsia.h\n",
        "  __locale_dir/locale_base_api/fuchsia.h\n"
        "  __locale_dir/locale_base_api/lean_os.h\n",
        "the list of headers libcxx installs")))
    out.append(("libcxx/.../locale (catopen)", edit(
        os.path.join(root, "libcxx/include/locale"),
        CATOPEN_ANCHOR, CATOPEN_EDIT,
        "the list of unix variants that have no catopen")))
    out.append(("libcxx/.../chrono.cpp", edit(
        os.path.join(root, "libcxx/src/chrono.cpp"),
        CHRONO_ANCHOR, CHRONO_EDIT,
        "the systems with a monotonic clock but not all of POSIX timers")))
    out.append(("libcxx/.../module.modulemap", edit(
        os.path.join(root, "libcxx/include/module.modulemap"),
        'module std_private_locale_locale_base_api_fuchsia              '
        '[system] { textual header "__locale_dir/locale_base_api/fuchsia.h" }',
        'module std_private_locale_locale_base_api_fuchsia              '
        '[system] { textual header "__locale_dir/locale_base_api/fuchsia.h" }\n'
        'module std_private_locale_locale_base_api_lean_os              '
        '[system] { textual header "__locale_dir/locale_base_api/lean_os.h" }',
        "the module map for the same directory")))
    return out


def main():
    if len(sys.argv) != 2:
        print("usage: apply.py <llvm-project-dir>", file=sys.stderr)
        return 2
    here = os.path.dirname(os.path.abspath(__file__))
    root = sys.argv[1]
    try:
        for name, status in port_llvm(root):
            print("  %-32s %s" % (name, status))
        for name, status in port_clang(root, here):
            print("  %-32s %s" % (name, status))
        for name, status in port_libcxx(root):
            print("  %-32s %s" % (name, status))
    except MissingAnchor as e:
        print("clang-port: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
