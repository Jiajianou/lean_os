#include "LeanOS.h"
#include "llvm/Config/llvm-config.h"
#if __has_include("clang/Driver/CommonArgs.h")
#include "clang/Driver/CommonArgs.h"
#else
#include "CommonArgs.h"
#endif
#include "clang/Config/config.h"
#include "clang/Driver/Compilation.h"
#if __has_include("clang/Options/Options.h")
#include "clang/Options/Options.h"
#else
#include "clang/Driver/Options.h"
#endif
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::tools;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

static bool isPositionIndependentRequest(const ArgList &Args) {
  return Args.hasArg(options::OPT_shared, options::OPT_pie,
                     options::OPT_fpic, options::OPT_fPIC,
                     options::OPT_fpie, options::OPT_fPIE);
}

#if LLVM_VERSION_MAJOR >= 24
void LeanOS::addClangTargetOptions(const ArgList &DriverArgs,
                                   ArgStringList &CC1Args, BoundArch,
                                   Action::OffloadKind) const {
#else
void LeanOS::addClangTargetOptions(const ArgList &DriverArgs,
                                   ArgStringList &CC1Args,
                                   Action::OffloadKind) const {
#endif
  const bool PIC = isPositionIndependentRequest(DriverArgs);

  if (!DriverArgs.hasArg(options::OPT_mcmodel_EQ) && !PIC)
    CC1Args.push_back("-mcmodel=large");

  if (!DriverArgs.hasArg(options::OPT_mred_zone))
    CC1Args.push_back("-disable-red-zone");

  if (PIC && !DriverArgs.hasArg(options::OPT_fplt, options::OPT_fno_plt))
    CC1Args.push_back("-fno-plt");

  if (!DriverArgs.hasArg(options::OPT_ftlsmodel_EQ) &&
      DriverArgs.hasArg(options::OPT_shared, options::OPT_fpic,
                        options::OPT_fPIC))
    CC1Args.push_back("-ftls-model=initial-exec");
}

void LeanOS::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                       ArgStringList &CC1Args) const {
  const Driver &D = getDriver();

  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> Dir(D.ResourceDir);
    llvm::sys::path::append(Dir, "include");
    addSystemInclude(DriverArgs, CC1Args, Dir.str());
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  addSystemInclude(DriverArgs, CC1Args,
                   concat(D.SysRoot, "/usr/local/include"));
  addSystemInclude(DriverArgs, CC1Args, concat(D.SysRoot, "/usr/include"));
}

void LeanOS::addLibCxxIncludePaths(const ArgList &DriverArgs,
                                   ArgStringList &CC1Args) const {
  addSystemInclude(DriverArgs, CC1Args,
                   getTargetLibDir() + "/include/c++/v1");
}

void LeanOS::addLibStdCxxIncludePaths(const ArgList &DriverArgs,
                                      ArgStringList &CC1Args) const {
  const std::string Base = getTargetLibDir() + "/include/c++";
  std::error_code EC;
  for (llvm::vfs::directory_iterator LI = getVFS().dir_begin(Base, EC), LE;
       !EC && LI != LE; LI = LI.increment(EC)) {
    llvm::StringRef Version = llvm::sys::path::filename(LI->path());
    addSystemInclude(DriverArgs, CC1Args, LI->path());
    addSystemInclude(DriverArgs, CC1Args,
                     Base + "/" + Version.str() + "/" +
                         getTriple().getArchName().str() + "-" +
                         getTriple().getOSName().str());
    addSystemInclude(DriverArgs, CC1Args, Base + "/" + Version.str() +
                                              "/backward");
    break;
  }
}

void LeanOS::AddCXXStdlibLibArgs(const ArgList &Args,
                                 ArgStringList &CmdArgs) const {
  switch (GetCXXStdlibType(Args)) {
  case ToolChain::CST_Libcxx:
    CmdArgs.push_back("-lc++");
    CmdArgs.push_back("-lc++abi");
    break;
  case ToolChain::CST_Libstdcxx:
    CmdArgs.push_back("-lstdc++");
    break;
  }
}

Tool *LeanOS::buildLinker() const { return new tools::leanos::Linker(*this); }

LeanOS::LeanOS(const Driver &D, const llvm::Triple &Triple, const ArgList &Args)
    : Generic_ELF(D, Triple, Args) {
  GCCInstallation.init(Triple, Args);

  if (GCCInstallation.isValid())
    getFilePaths().push_back(GCCInstallation.getInstallPath().str());

  getFilePaths().push_back(getTargetLibDir() + "/lib");

  getFilePaths().push_back(concat(D.SysRoot, "/usr/lib"));
}

std::string LeanOS::getTargetLibDir() const {
  return getDriver().Dir + "/../" + getTriple().getArchName().str() + "-" +
         getTriple().getOSName().str();
}

void leanos::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                  const InputInfo &Output,
                                  const InputInfoList &Inputs,
                                  const ArgList &Args,
                                  const char *LinkingOutput) const {
  const auto &ToolChain = static_cast<const LeanOS &>(getToolChain());
  const Driver &D = ToolChain.getDriver();
  ArgStringList CmdArgs;

  const bool Shared = Args.hasArg(options::OPT_shared);
  // hasFlag, not hasArg: -pie and -no-pie are a pair and the LAST one on the
  // command line wins, which is how every other toolchain here reads them and
  // how a build system that adds -pie for everybody expects to be able to
  // take it back. M121 wrote hasArg, so -no-pie was silently ignored and a
  // caller that asked for a static executable got a position-independent one
  // with an interpreter this machine has no path for. M145 found it by
  // linking Chromium's //base, whose executable_config adds -pie to every
  // link.
  const bool Pie =
      Args.hasFlag(options::OPT_pie, options::OPT_no_pie, false) && !Shared;
  const bool Static = !Shared && !Pie;

  Args.ClaimAllArgs(options::OPT_g_Group);
  Args.ClaimAllArgs(options::OPT_emit_llvm);
  Args.ClaimAllArgs(options::OPT_w);
  Args.ClaimAllArgs(options::OPT_rdynamic);
  Args.ClaimAllArgs(options::OPT_static);

  if (!D.SysRoot.empty())
    CmdArgs.push_back(Args.MakeArgString("--sysroot=" + D.SysRoot));

  CmdArgs.push_back("-z");
  CmdArgs.push_back("noexecstack");

  if (Shared) {
    CmdArgs.push_back("-shared");
  } else if (Pie) {
    CmdArgs.push_back("-pie");
    CmdArgs.push_back("-dynamic-linker");
    CmdArgs.push_back("/lib/ld-lean.so");
  } else {
    CmdArgs.push_back("-static");
    CmdArgs.push_back("--no-relax");
    CmdArgs.push_back("-T");
    CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("lean_os.ld")));
  }

  assert((Output.isFilename() || Output.isNothing()) && "Invalid output.");
  if (Output.isFilename()) {
    CmdArgs.push_back("-o");
    CmdArgs.push_back(Output.getFilename());
  }

  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles,
                   options::OPT_r)) {
    if (!Shared)
      CmdArgs.push_back(Args.MakeArgString(
          ToolChain.GetFilePath(Pie ? "Scrt1.o" : "crt1.o")));
    CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crti.o")));
    CmdArgs.push_back(Args.MakeArgString(
        ToolChain.GetFilePath(Static ? "crtbegin.o" : "crtbeginS.o")));
  }

  Args.addAllArgs(CmdArgs, {options::OPT_L, options::OPT_u});
  ToolChain.AddFilePathLibArgs(Args, CmdArgs);

#if LLVM_VERSION_MAJOR >= 24
  if (ToolChain.isUsingLTO(Args)) {
#else
  if (D.isUsingLTO()) {
#endif
    assert(!Inputs.empty() && "Must have at least one input.");
    auto Input = llvm::find_if(
        Inputs, [](const InputInfo &II) -> bool { return II.isFilename(); });
    if (Input == Inputs.end())
      Input = Inputs.begin();
#if LLVM_VERSION_MAJOR >= 24
    addLTOOptions(ToolChain, Args, CmdArgs, Output, Inputs,
                  ToolChain.getLTOMode(Args) == LTOK_Thin);
#else
    addLTOOptions(ToolChain, Args, CmdArgs, Output, *Input,
                  D.getLTOMode() == LTOK_Thin);
#endif
  }

  AddLinkerInputs(ToolChain, Inputs, Args, CmdArgs, JA);

  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs,
                   options::OPT_r)) {
    if (D.CCCIsCXX() && ToolChain.ShouldLinkCXXStdlib(Args)) {
      ToolChain.AddCXXStdlibLibArgs(Args, CmdArgs);
      CmdArgs.push_back("-lm");
    }
    Args.ClaimAllArgs(options::OPT_stdlib_EQ);

    if (Pie)
      CmdArgs.push_back("-l:ld-lean.so");

    CmdArgs.push_back("--start-group");
    CmdArgs.push_back("-lc");
    if (Static) {
      CmdArgs.push_back("-lgcc");
      CmdArgs.push_back("-lgcc_eh");
    } else {
      CmdArgs.push_back("-lgcc_s");
      CmdArgs.push_back("-lgcc");
    }
    CmdArgs.push_back("--end-group");
  }

  Args.claimAllArgs(options::OPT_pthread, options::OPT_pthreads);

  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles,
                   options::OPT_r)) {
    CmdArgs.push_back(Args.MakeArgString(
        ToolChain.GetFilePath(Static ? "crtend.o" : "crtendS.o")));
    CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crtn.o")));
  }

  const char *Exec = Args.MakeArgString(ToolChain.GetLinkerPath());
  C.addCommand(std::make_unique<Command>(JA, *this,
                                         ResponseFileSupport::AtFileCurCP(),
                                         Exec, CmdArgs, Inputs, Output));
}
