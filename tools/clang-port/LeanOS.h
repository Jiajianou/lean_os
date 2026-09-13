#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_LEANOS_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_LEANOS_H

#include "Gnu.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/ToolChain.h"

namespace clang {
namespace driver {
namespace tools {

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
}
}

namespace toolchains {

class LLVM_LIBRARY_VISIBILITY LeanOS : public Generic_ELF {
public:
  LeanOS(const Driver &D, const llvm::Triple &Triple,
         const llvm::opt::ArgList &Arguments);

  bool IsMathErrnoDefault() const override { return true; }

  bool IsObjCNonFragileABIDefault() const override { return true; }

  bool isPICDefault() const override { return false; }
  bool isPIEDefault(const llvm::opt::ArgList &Arguments) const override {
    return false;
  }
  bool isPICDefaultForced() const override { return false; }

  bool IsIntegratedAssemblerDefault() const override { return true; }

  bool HasNativeLLVMSupport() const override { return true; }

  const char *getDefaultLinker() const override {
    return "x86_64-lean_os-ld";
  }

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

  std::string getTargetLibDir() const;

  void AddCXXStdlibLibArgs(const llvm::opt::ArgList &Arguments,
                           llvm::opt::ArgStringList &CmdArgs) const override;

protected:
  Tool *buildLinker() const override;
};

}
}
}

#endif
