//===--- LeanOS.cpp - lean_os ToolChain Implementations ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// tools/clang-port/LeanOS.cpp - M121: clang's driver for x86_64-lean_os.
//
// ---- what this file is, and what it is a second copy of -----------------
//
// tools/toolchain-port/lean_os.h is GCC's target description for this OS,
// and it is the file to read first: it argues, at length and with the bugs
// that taught it, for every decision repeated here. This file says the
// same things to a different compiler, and the interesting part of the
// milestone is that "the same things" turned out to be a real constraint -
// two compilers that disagree about the code model or the startup files
// produce object files that cannot be linked together, and both of them
// link against the *same* libc.a and the same crt1.o.
//
// So each block below names the spec in lean_os.h it corresponds to. When
// one changes, both change, and tools/clang-test.sh is the instrument that
// says so: it requires clang and GCC to agree about the shape of the
// program, not merely each to produce one.
//
// ---- what a "target port" is, and why this is not a fork ----------------
//
// M94's line, applied to clang: a target port is upstream-shaped
// configuration - what the OS is called, which startup files a program
// links, where the C library is, what the preprocessor defines. Nothing
// here touches a pass, an optimisation or a code-generation decision;
// that is X86TargetMachine's business and is untouched.
//
//===----------------------------------------------------------------------===//

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

/// Is this command line asking for position-independent code?
///
/// The predicate is lean_os.h's `%{shared|pie|fpic|fPIC|fpie|fPIE:...}`,
/// spelled out, and it is deliberately the flags on the line rather than
/// the driver's computed PIC level. lean_os.h's own note says why, and it
/// is the case its first draft got wrong: CPython compiles a module's
/// objects with `$(CC) -c $(CCSHARED)`, which is -fPIC and nothing else,
/// and links them with `$(CC) -shared` afterwards. Keying only on the link
/// flags gives those objects the large code model, and a large-model object
/// in a shared library is a page of "relocation truncated to fit" at the
/// end of a long compile.
static bool isPositionIndependentRequest(const ArgList &Args) {
  return Args.hasArg(options::OPT_shared, options::OPT_pie,
                     options::OPT_fpic, options::OPT_fPIC,
                     options::OPT_fpie, options::OPT_fPIE);
}

void LeanOS::addClangTargetOptions(const ArgList &DriverArgs,
                                   ArgStringList &CC1Args,
                                   Action::OffloadKind) const {
  const bool PIC = isPositionIndependentRequest(DriverArgs);

  // ---- the code model: lean_os.h's DRIVER_SELF_SPECS, first clause -----
  //
  //   -shared / -pie / -fPIC  ->  small
  //   neither                 ->  large
  //
  // A static lean_os program is linked at 512 GiB (user_space/lib/user.ld
  // and USER_IMAGE_BASE), which the small model's 32-bit relocations do
  // not reach: left out, the compile succeeds and the LINK fails with a
  // page of "relocation truncated to fit". Position-independent code is
  // small-model code by construction - it reaches itself RIP-relative -
  // so asking for the large model there would turn every self-reference
  // into a relocation, which is the opposite of what PIC is for.
  //
  // Guarded so an explicit -mcmodel= from the caller wins. A driver that
  // could not be overridden would be worse than one that needed flags.
  if (!DriverArgs.hasArg(options::OPT_mcmodel_EQ) && !PIC)
    CC1Args.push_back("-mcmodel=large");

  // ---- the red zone: lean_os.h's second clause -------------------------
  //
  // The kernel builds a signal frame on the process's own stack (M76),
  // which the red zone would be underneath. A program compiled with one
  // loses 128 bytes of live data the first time it takes a signal.
  if (!DriverArgs.hasArg(options::OPT_mred_zone))
    CC1Args.push_back("-disable-red-zone");

  // ---- -fno-plt, wherever the small model is chosen --------------------
  //
  // M100's bug, and it is the same one here. A small-model object calls
  // an external function through the PLT with a 32-bit PC-relative
  // relocation, which is right for every function that exists and wrong
  // for the one kind that does not: a weak undefined symbol, which ld
  // resolves to address 0, and 0 is not within 2 GiB of anything on this
  // target. -fno-plt turns those into GOT loads, and a GOT slot can hold
  // 0 wherever the program sits. It costs nothing the PLT was buying -
  // there is no lazy binding in ld-lean.so, so a GOT entry filled at load
  // is what a PLT stub would have reached on the first call anyway.
  if (PIC && !DriverArgs.hasArg(options::OPT_fplt, options::OPT_fno_plt))
    CC1Args.push_back("-fno-plt");

  // ---- the TLS model for a shared object -------------------------------
  //
  // initial-exec, because general-dynamic needs `__tls_get_addr` and
  // user_space/ld/ld-lean.c does not have one. The cost is written down
  // in lean_os.h and is real: an object dlopen'ed after startup cannot
  // have thread-local variables. Not applied to -pie, whose own
  // thread-locals are local-exec, which is what the compiler already
  // picks and is strictly better.
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

  // The compiler's own headers first - <stdint.h>, <stddef.h>,
  // <stdarg.h>. CLAUDE.md's third non-negotiable allows exactly these:
  // they are compiler-provided and nothing is linked for them.
  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> Dir(D.ResourceDir);
    llvm::sys::path::append(Dir, "include");
    addSystemInclude(DriverArgs, CC1Args, Dir.str());
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // ---- and then this libc, BEFORE system_api -------------------------
  //
  // The order is not cosmetic and it is not clang's choice - it is the
  // order `x86_64-lean_os-gcc -v -E` prints, which is the order the
  // Makefile's `sysroot` target lays the two trees out for:
  //
  //   /usr/local/include   user_space/libc/include - the C library
  //   /usr/include         system_api/include      - the kernel ABI
  //
  // M100 left an open box about this ("Two headers named signal.h"): both
  // trees have one, and a build that puts /usr/include first shadows the
  // libc's headers with the ABI's and breaks <setjmp.h>. This port does
  // not fix that - the fix is to stop having two headers with one name -
  // but it does not introduce a second way to get it wrong either, which
  // is why the order here is copied from the compiler that already works
  // rather than chosen.
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
  // M97's libstdc++, which clang can use with -stdlib=libstdc++. Its
  // headers are in two places and both are needed: the generic ones, and
  // the per-target directory holding the <bits/c++config.h> that records
  // what THIS target's configure decided. Generic_GCC has a detector for
  // this layout, and it does not find ours - it looks under the GCC
  // installation directory, and a `--target=` cross build puts the C++
  // headers under $PREFIX/<target>/include instead.
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
    // libc++abi is a separate archive here rather than merged into
    // libc++: this target has no dynamic C++ runtime, so there is no
    // libc++.so to hide the seam inside, and naming both is what a
    // static libc++ needs anyway. The unwinder they both call is
    // libgcc_eh, added by the link job below - see the note there.
    CmdArgs.push_back("-lc++");
    CmdArgs.push_back("-lc++abi");
    break;
  case ToolChain::CST_Libstdcxx:
    // M97 built this one, and clang can link against it: it is the same
    // ABI, from the same GCC installation whose libgcc this target
    // already uses.
    CmdArgs.push_back("-lstdc++");
    break;
  }
}

Tool *LeanOS::buildLinker() const { return new tools::leanos::Linker(*this); }

LeanOS::LeanOS(const Driver &D, const llvm::Triple &Triple, const ArgList &Args)
    : Generic_ELF(D, Triple, Args) {
  // ---- where the GCC half of this toolchain is -------------------------
  //
  // Not a fallback and not a courtesy: crtbegin.o, crtend.o, libgcc.a and
  // libgcc_eh.a are M94's and M97's, built by GCC for this target, and
  // they are what a clang-compiled program on this machine links against.
  // Building compiler-rt as a second copy of the same runtime is
  // deliberately not done here - see the milestone entry.
  //
  // The detector finds them with no configuration because of where this
  // clang is installed: ToolChain's constructor puts the driver's own
  // directory on the program paths, and GCCInstallationDetector searches
  // `<driver dir>/..` - so a clang in build/toolchain/bin finds
  // build/toolchain/lib/gcc/x86_64-lean_os/<version> the way it finds
  // x86_64-lean_os-ld, by being in the same prefix as it.
  GCCInstallation.init(Triple, Args);

  if (GCCInstallation.isValid())
    getFilePaths().push_back(GCCInstallation.getInstallPath().str());

  // ---- and where the target's C++ libraries live -----------------------
  //
  // $PREFIX/x86_64-lean_os/lib: libstdc++.a and libsupc++.a from M97, and
  // libc++.a/libc++abi.a from tools/build-libcxx.sh. Deliberately NOT the
  // sysroot, and the reason is a hazard CLAUDE.md already names: the
  // Makefile's `sysroot` target begins `rm -rf $(SYSROOT)`, so anything
  // installed into build/sysroot by a separate step is deleted by the
  // next `make sysroot` - which tools/gcc-test.sh and tools/clang-test.sh
  // both run. $PREFIX/<target> is where a GNU cross toolchain puts target
  // libraries anyway, which is why libstdc++ was already there to copy.
  getFilePaths().push_back(getTargetLibDir() + "/lib");

  // crt1.o, Scrt1.o, crti.o, crtn.o, lean_os.ld, libc.a, ld-lean.so, and
  // the fifteen third-party archives M100 installed beside them.
  getFilePaths().push_back(concat(D.SysRoot, "/usr/lib"));
}

/// The triple as the toolchain prefix spells it - `x86_64-lean_os`, with
/// no vendor. llvm::Triple normalises `--target=x86_64-lean_os` to
/// `x86_64-unknown-lean_os`, so getTripleString() is the wrong name for a
/// directory; this rebuilds the one M94's `--target=` actually created.
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
  const bool Pie = Args.hasArg(options::OPT_pie) && !Shared;
  const bool Static = !Shared && !Pie;

  // Silence the warnings for flags that only matter at compile time but
  // which every build system puts on the link line too.
  Args.ClaimAllArgs(options::OPT_g_Group);
  Args.ClaimAllArgs(options::OPT_emit_llvm);
  Args.ClaimAllArgs(options::OPT_w);
  // -rdynamic is meaningless here: ld-lean.so resolves dlsym against the
  // objects it mapped, not against an executable's dynamic symbol table.
  Args.ClaimAllArgs(options::OPT_rdynamic);
  // -static is what this target does anyway; claim it so `-static` on a
  // caller's line is not reported as unused.
  Args.ClaimAllArgs(options::OPT_static);

  if (!D.SysRoot.empty())
    CmdArgs.push_back(Args.MakeArgString("--sysroot=" + D.SysRoot));

  // ---- the stack is not code, said where the linker can hear it -------
  //
  // kernel/proc/proc.c maps a process stack without VMM_FLAG_EXEC and
  // says so in a comment: "a stack is not code". This kernel does not
  // read PT_GNU_STACK at all, so the note is not what makes the stack
  // NX - it is already NX - but the linker has an opinion about objects
  // that do not carry one, and that opinion is a warning on every single
  // link: crtn.o is built by nasm (user_space/lib/crti.asm) and has no
  // .note.GNU-stack, and clang's own objects do, so ld sees a mix and
  // asks. Saying `-z noexecstack` answers it with the truth.
  //
  // GCC's link line does not need this and has not got it: none of its
  // objects carry the note either, so ld never sees a mix and never
  // asks. That asymmetry is the whole of it - the two compilers agree
  // about the stack, and only one of them mentions it.
  CmdArgs.push_back("-z");
  CmdArgs.push_back("noexecstack");

  // ---- the link, and its three cases: lean_os.h's LINK_SPEC -----------
  if (Shared) {
    // ld's own default script, NOT -T lean_os.ld. This was M94's one real
    // mistake in the GCC file and it would have been repeated here: the
    // fixed script has no .dynamic, so every -shared link failed with
    // "undefined reference to _DYNAMIC" - a symbol nobody wrote and the
    // linker normally defines.
    CmdArgs.push_back("-shared");
  } else if (Pie) {
    CmdArgs.push_back("-pie");
    CmdArgs.push_back("-dynamic-linker");
    CmdArgs.push_back("/lib/ld-lean.so");
  } else {
    CmdArgs.push_back("-static");
    // --no-relax, and it is the same fact as -mcmodel=large one layer
    // down. ld rewrites `mov foo@GOTPCREL(%rip)` into `lea foo(%rip)`
    // when it can prove the target is within 2 GiB - which it never is
    // here, because a static lean_os program is linked at 512 GiB and an
    // unresolved weak symbol resolves to address zero. ld does not fall
    // back; it stops, naming this flag. Only on the static branch: a PIE
    // is placed wherever there is room and taking the relaxation is right.
    CmdArgs.push_back("--no-relax");
    CmdArgs.push_back("-T");
    CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("lean_os.ld")));
  }

  assert((Output.isFilename() || Output.isNothing()) && "Invalid output.");
  if (Output.isFilename()) {
    CmdArgs.push_back("-o");
    CmdArgs.push_back(Output.getFilename());
  }

  // ---- the startup files: lean_os.h's STARTFILE_SPEC ------------------
  //
  // crt1.o is user_space/lib/crt0.asm under the name every driver looks
  // for; Scrt1.o is crt0-pie.asm, the same startup with its two calls
  // routed through the PLT. crti.o and crtn.o bracket _init and _fini in
  // all three cases, because user_space/lib/crti.asm builds them out of
  // fragments: crti.o opens what crtbegin contributes to and crtn.o
  // closes it. A shared object gets NO crt1 of any kind - crt0's call to
  // `main` is a PC-relative reference to a symbol that does not exist,
  // which stops the link with a message about PIC from a file that should
  // not have been on the line.
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

  if (D.isUsingLTO()) {
    assert(!Inputs.empty() && "Must have at least one input.");
    auto Input = llvm::find_if(
        Inputs, [](const InputInfo &II) -> bool { return II.isFilename(); });
    if (Input == Inputs.end())
      Input = Inputs.begin();
    addLTOOptions(ToolChain, Args, CmdArgs, Output, *Input,
                  D.getLTOMode() == LTOK_Thin);
  }

  AddLinkerInputs(ToolChain, Inputs, Args, CmdArgs, JA);

  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs,
                   options::OPT_r)) {
    if (D.CCCIsCXX() && ToolChain.ShouldLinkCXXStdlib(Args)) {
      ToolChain.AddCXXStdlibLibArgs(Args, CmdArgs);
      // libm.a exists and is empty on purpose - see the Makefile's
      // `sysroot` target. Every C++ driver on every target ends its link
      // in -lm, so this one does too, and the truthful shape of "there is
      // nothing in libm that is not already in libc" is an archive with
      // no members rather than a LINK_SPEC that differs from everyone
      // else's.
      CmdArgs.push_back("-lm");
    }
    Args.ClaimAllArgs(options::OPT_stdlib_EQ);

    // ---- the loader, on the line of every dynamic executable ----------
    //
    // dlopen, dlsym, dlclose and dlerror are in ld-lean.so: the loader is
    // the only thing that knows what is loaded, and a copy of that in
    // libc would go out of date the first time either side changed.
    // There is no libdl holding stubs that trampoline into it, which is
    // glibc's arrangement since 2.34 and, unlike glibc's, was never
    // anything else. So a -pie program names the loader at link time and
    // gets dlopen with nothing supplied by hand. Not for -shared: a
    // shared object gets dlopen resolved from the executable's scope, and
    // adding it here would put the loader in the DT_NEEDED list of every
    // extension module on the disk.
    if (Pie)
      CmdArgs.push_back("-l:ld-lean.so");

    // ---- libc and libgcc: lean_os.h's LIB_SPEC and LIBGCC_SPEC --------
    //
    // GCC emits `-lgcc -lgcc_eh -lc -lgcc -lgcc_eh` here (its
    // link_gcc_c_sequence is %G %L %G, twice around libc). A group says
    // the same thing once and is why it exists: this libc references
    // libgcc's helpers and libgcc's unwinder references this libc, and
    // neither caller should have to know the order.
    //
    // -lgcc_eh on the static branch is the half that is easy to leave
    // out and was found the hard way in M97: asking for a shared libgcc
    // SPLITS the archive, moving the exception machinery out of libgcc.a
    // and into libgcc_eh.a - so a static link that used to get the
    // unwinder for free stops getting it, and only a program that
    // reaches a part of the C++ runtime which can throw finds out.
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

  // No -lpthread: pthread_create, the mutexes and the futex wait are in
  // libc itself (M79, M96), which is glibc's own arrangement since 2.34.
  // A link line that named a libpthread would fail with "cannot find
  // -lpthread", which is a worse answer than the truth.
  Args.claimAllArgs(options::OPT_pthread, options::OPT_pthreads);

  // ---- and the closing half: lean_os.h's ENDFILE_SPEC ----------------
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
