#include "LeanOS.h"
#include "CommonArgs.h"
#include "clang/Config/config.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Options.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::tools;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

static bool isPositionIndependentRequest(const ArgList &Arguments) {
  return Arguments.hasArg(options::OPT_shared, options::OPT_pie,
                     options::OPT_fpic, options::OPT_fPIC,
                     options::OPT_fpie, options::OPT_fPIE);
}

void LeanOS::addClangTargetOptions(const ArgList &DriverArgs,
                                   ArgStringList &CC1Args,
                                   Action::OffloadKind) const {
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
    SmallString<128> Directory(D.ResourceDir);
    llvm::sys::path::append(Directory, "include");
    addSystemInclude(DriverArgs, CC1Args, Directory.string());
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
  for (llvm::virtual_file_system::directory_iterator LI = getVFS().directory_begin(Base, EC), LE;
       !EC && LI != LE; LI = LI.increment(EC)) {
    llvm::StringRef Version = llvm::sys::path::filename(LI->path());
    addSystemInclude(DriverArgs, CC1Args, LI->path());
    addSystemInclude(DriverArgs, CC1Args,
                     Base + "/" + Version.string() + "/" +
                         getTriple().getArchName().string() + "-" +
                         getTriple().getOSName().string());
    addSystemInclude(DriverArgs, CC1Args, Base + "/" + Version.string() +
                                              "/backward");
    break;
  }
}

void LeanOS::AddCXXStdlibLibArgs(const ArgList &Arguments,
                                 ArgStringList &CmdArgs) const {
  switch (GetCXXStdlibType(Arguments)) {
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

LeanOS::LeanOS(const Driver &D, const llvm::Triple &Triple, const ArgList &Arguments)
    : Generic_ELF(D, Triple, Arguments) {
  GCCInstallation.init(Triple, Arguments);

  if (GCCInstallation.isValid())
    getFilePaths().push_back(GCCInstallation.getInstallPath().string());

  getFilePaths().push_back(getTargetLibDir() + "/lib");

  getFilePaths().push_back(concat(D.SysRoot, "/usr/lib"));
}

std::string LeanOS::getTargetLibDir() const {
  return getDriver().Directory + "/../" + getTriple().getArchName().string() + "-" +
         getTriple().getOSName().string();
}

void leanos::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                  const InputInfo &Output,
                                  const InputInfoList &Inputs,
                                  const ArgList &Arguments,
                                  const char *LinkingOutput) const {
  const auto &ToolChain = static_cast<const LeanOS &>(getToolChain());
  const Driver &D = ToolChain.getDriver();
  ArgStringList CmdArgs;

  const bool Shared = Arguments.hasArg(options::OPT_shared);
  const bool Pie = Arguments.hasArg(options::OPT_pie) && !Shared;
  const bool Static = !Shared && !Pie;

  Arguments.ClaimAllArgs(options::OPT_g_Group);
  Arguments.ClaimAllArgs(options::OPT_emit_llvm);
  Arguments.ClaimAllArgs(options::OPT_w);
  Arguments.ClaimAllArgs(options::OPT_rdynamic);
  Arguments.ClaimAllArgs(options::OPT_static);

  if (!D.SysRoot.empty())
    CmdArgs.push_back(Arguments.MakeArgString("--sysroot=" + D.SysRoot));

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
    CmdArgs.push_back(Arguments.MakeArgString(ToolChain.GetFilePath("lean_os.ld")));
  }

  assert((Output.isFilename() || Output.isNothing()) && "Invalid output.");
  if (Output.isFilename()) {
    CmdArgs.push_back("-o");
    CmdArgs.push_back(Output.getFilename());
  }

  if (!Arguments.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles,
                   options::OPT_r)) {
    if (!Shared)
      CmdArgs.push_back(Arguments.MakeArgString(
          ToolChain.GetFilePath(Pie ? "Scrt1.o" : "crt1.o")));
    CmdArgs.push_back(Arguments.MakeArgString(ToolChain.GetFilePath("crti.o")));
    CmdArgs.push_back(Arguments.MakeArgString(
        ToolChain.GetFilePath(Static ? "crtbegin.o" : "crtbeginS.o")));
  }

  Arguments.addAllArgs(CmdArgs, {options::OPT_L, options::OPT_u});
  ToolChain.AddFilePathLibArgs(Arguments, CmdArgs);

  if (D.isUsingLTO()) {
    assert(!Inputs.empty() && "Must have at least one input.");
    auto Input = llvm::find_if(
        Inputs, [](const InputInfo &II) -> bool { return II.isFilename(); });
    if (Input == Inputs.end())
      Input = Inputs.begin();
    addLTOOptions(ToolChain, Arguments, CmdArgs, Output, *Input,
                  D.getLTOMode() == LTOK_Thin);
  }

  AddLinkerInputs(ToolChain, Inputs, Arguments, CmdArgs, JA);

  if (!Arguments.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs,
                   options::OPT_r)) {
    if (D.CCCIsCXX() && ToolChain.ShouldLinkCXXStdlib(Arguments)) {
      ToolChain.AddCXXStdlibLibArgs(Arguments, CmdArgs);
      CmdArgs.push_back("-lm");
    }
    Arguments.ClaimAllArgs(options::OPT_stdlib_EQ);

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

  Arguments.claimAllArgs(options::OPT_pthread, options::OPT_pthreads);

  if (!Arguments.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles,
                   options::OPT_r)) {
    CmdArgs.push_back(Arguments.MakeArgString(
        ToolChain.GetFilePath(Static ? "crtend.o" : "crtendS.o")));
    CmdArgs.push_back(Arguments.MakeArgString(ToolChain.GetFilePath("crtn.o")));
  }

  const char *Exec = Arguments.MakeArgString(ToolChain.GetLinkerPath());
  C.addCommand(std::make_unique<Command>(JA, *this,
                                         ResponseFileSupport::AtFileCurCP(),
                                         Exec, CmdArgs, Inputs, Output));
}
