//===--- LeanOS.h - lean_os ToolChain Implementations -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// tools/clang-port/LeanOS.h - M121.
//
// Clang's driver for x86_64-lean_os. The header is the small half; what
// each of these overrides is FOR is written down in LeanOS.cpp beside the
// code that does it, and every one of them has a counterpart in
// tools/toolchain-port/lean_os.h - which is GCC's answer to the same
// questions and is the file to read first.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_LEANOS_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_LEANOS_H

#include "Gnu.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/ToolChain.h"

namespace clang {
namespace driver {
namespace tools {

/// Drive x86_64-lean_os-ld directly. There is no lean_os-specific
/// assembler step: clang's integrated assembler produces the object, and
/// the ELF64 it produces is the ELF64 this OS's loader reads.
namespace leanos {
class LLVM_LIBRARY_VISIBILITY Linker final : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("leanos::Linker", "linker", TC) {}

  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }

  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};
} // end namespace leanos
} // end namespace tools

namespace toolchains {

class LLVM_LIBRARY_VISIBILITY LeanOS : public Generic_ELF {
public:
  LeanOS(const Driver &D, const llvm::Triple &Triple,
         const llvm::opt::ArgList &Args);

  // errno is set by this libm (user_space/libc/src/math.c) and
  // tools/math-test.sh grades it against the host's, so the compiler may
  // not assume a math call is side-effect free.
  bool IsMathErrnoDefault() const override { return true; }

  bool IsObjCNonFragileABIDefault() const override { return true; }

  // A lean_os program is linked at a fixed address (512 GiB) and is
  // static unless it asks otherwise - see LeanOS.cpp's code-model note.
  bool isPICDefault() const override { return false; }
  bool isPIEDefault(const llvm::opt::ArgList &Args) const override {
    return false;
  }
  bool isPICDefaultForced() const override { return false; }

  bool IsIntegratedAssemblerDefault() const override { return true; }

  bool HasNativeLLVMSupport() const override { return true; }

  // `ld`, from the binutils half of M94's port, which lives in the same
  // prefix this clang is installed into. Named in full rather than as
  // "ld" so that a clang on somebody's PATH cannot pick up the host's.
  const char *getDefaultLinker() const override {
    return "x86_64-lean_os-ld";
  }

  // libgcc, not compiler-rt: M94 built libgcc.a and libgcc_eh.a for this
  // target and they are the unwinder every static program here links.
  RuntimeLibType GetDefaultRuntimeLibType() const override {
    return ToolChain::RLT_Libgcc;
  }
  UnwindLibType GetDefaultUnwindLibType() const override {
    return ToolChain::UNW_None;
  }

  CXXStdlibType GetDefaultCXXStdlibType() const override {
    return ToolChain::CST_Libcxx;
  }

  void
  addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                        llvm::opt::ArgStringList &CC1Args,
                        Action::OffloadKind DeviceOffloadKind) const override;

  void AddClangSystemIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;

  void addLibCxxIncludePaths(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;

  void addLibStdCxxIncludePaths(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;

  /// $PREFIX/x86_64-lean_os - where the target's own libraries live, as
  /// distinct from the sysroot (this OS's own /usr) and from the GCC
  /// installation directory. See the note in the constructor.
  std::string getTargetLibDir() const;

  void AddCXXStdlibLibArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const override;

protected:
  Tool *buildLinker() const override;
};

} // end namespace toolchains
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_LEANOS_H
