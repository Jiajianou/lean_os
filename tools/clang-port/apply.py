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
            "Anchor: %r" % (path, why, anchor[:160]))
    with open(path, "w") as f:
        f.write(text.replace(anchor, replacement, -1 if count == 0 else count))
    return "applied"

TRIPLE_H_ANCHOR = """    Serenity,
    Vulkan, // Vulkan SPIR-V
    LastOSType = Vulkan"""
TRIPLE_H_EDIT = """    Serenity,
    Vulkan, // Vulkan SPIR-V
    LeanOS, // lean_os
    LastOSType = LeanOS"""

TRIPLE_CPP_NAME_ANCHOR = '  case Serenity: return "serenity";'
TRIPLE_CPP_NAME_EDIT = ('  case Serenity: return "serenity";\n'
                        '  case LeanOS: return "lean_os";')

TRIPLE_CPP_PARSE_ANCHOR = '    .StartsWith("serenity", Triple::Serenity)'
TRIPLE_CPP_PARSE_EDIT = ('    .StartsWith("serenity", Triple::Serenity)\n'
                         '    .StartsWith("lean_os", Triple::LeanOS)')

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

LOCALE_API_ANCHOR = """#elif defined(__Fuchsia__)
#  include <__locale_dir/locale_base_api/fuchsia.h>"""
LOCALE_API_EDIT = """#elif defined(__Fuchsia__)
#  include <__locale_dir/locale_base_api/fuchsia.h>
#elif defined(__lean_os__)
// M121: this libc has one locale and it is "C" - see
// user_space/libc/include/locale.h, and tools/clang-port/apply.py for
// why the inline fallbacks are the honest answer rather than the easy one.
#  include <__locale_dir/locale_base_api/lean_os.h>"""

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

    dst = os.path.join(root, "clang/lib/Driver/ToolChains")
    for name in ("LeanOS.h", "LeanOS.cpp"):
        shutil.copyfile(os.path.join(port_dir, name), os.path.join(dst, name))
        out.append(("clang/.../%s" % name, "copied"))
    return out

CATOPEN_ANCHOR = """#    if !defined(__BIONIC__) && !defined(_NEWLIB_VERSION) && !defined(__EMSCRIPTEN__)"""
CATOPEN_EDIT = """#    if !defined(__BIONIC__) && !defined(_NEWLIB_VERSION) && !defined(__EMSCRIPTEN__) && \\
        !defined(__lean_os__)"""

CHRONO_ANCHOR = """#if defined(__APPLE__) || defined(__gnu_hurd__) || defined(__OpenBSD__) || (defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0)"""
CHRONO_EDIT = """#if defined(__APPLE__) || defined(__gnu_hurd__) || defined(__OpenBSD__) || \\
    defined(__lean_os__) || (defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0)"""

def port_libcxx(root):
    out = []
    base = os.path.join(root, "libcxx/include/__locale_dir/locale_base_api")
    out.append(("libcxx/.../lean_os.h", write_file(
        os.path.join(base, "lean_os.h"), LOCALE_API_HEADER,
        "the extended-locale API for a machine with one locale")))
    out.append(("libcxx/.../locale_base_api.h", edit(
        os.path.join(root, "libcxx/include/__locale_dir/locale_base_api.h"),
        LOCALE_API_ANCHOR, LOCALE_API_EDIT,
        "the per-platform locale-API switch")))

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
