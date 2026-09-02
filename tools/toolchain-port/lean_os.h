/* tools/toolchain-port/lean_os.h - M94
 *
 * GCC's target description for x86_64-lean_os. Copied into
 * gcc/config/lean_os.h by tools/build-toolchain.sh.
 *
 * ---- what a "target port" is, and why this is not a fork ---------------
 *
 * M94's last bullet draws the line: "a target port is upstream-shaped
 * configuration; a patch to the compiler's own passes is the thing M63's
 * rule exists to forbid." This file is entirely the first kind. It says
 * what the operating system is called, which startup files a program
 * links against, where the C library is, and what the preprocessor
 * should define - and it says nothing whatever about how code is
 * generated, which is i386/x86-64.h's business and is untouched.
 *
 * ---- the specs, one at a time -------------------------------------------
 *
 * These are what make `x86_64-lean_os-gcc hello.c -o hello` produce a
 * program this OS runs with **no flag invented by hand**, which is the
 * whole milestone: every flag invented by hand is a flag someone else's
 * build system will not pass.
 */

/* ---- the flags the driver supplies so that nobody has to --------------
 *
 * This is the single most important thing in this file, because it is
 * the difference between M94 being finished and M94 being a compiler
 * with a README of flags beside it. **Every flag invented by hand is a
 * flag someone else's build system will not pass** - so the three this
 * OS genuinely requires are added by the driver, and a `./configure` run
 * that knows nothing about lean_os gets them anyway.
 *
 *   -mcmodel=large  a lean_os program is linked at 512 GiB (see
 *                   user_space/lib/user.ld and USER_IMAGE_BASE), and the
 *                   small model's 32-bit relocations do not reach. Left
 *                   out, the compile succeeds and the LINK fails with a
 *                   page of "relocation truncated to fit" - which is
 *                   exactly how this was found here, on GCC's own
 *                   crtbegin.o.
 *   -mno-red-zone   the kernel builds a signal frame on the process's
 *                   own stack (M76), which the red zone would be
 *                   underneath. A program compiled with a red zone loses
 *                   128 bytes of live data the first time it takes a
 *                   signal.
 *   -fno-pic        there is no dynamic loader on this machine yet; a
 *                   GOT nothing fills in is a null dereference at the
 *                   first global. M95 is where this line changes.
 *
 * Each is guarded so an explicit flag from the caller wins - a driver
 * that could not be overridden would be worse than one that needed
 * arguments.
 */
#undef DRIVER_SELF_SPECS
#define DRIVER_SELF_SPECS                                       \
  "%{!mcmodel=*:-mcmodel=large} "                               \
  "%{!mred-zone:-mno-red-zone} "                                \
  "%{!fpic:%{!fPIC:%{!fpie:%{!fPIE:-fno-pic}}}}"

/* The preprocessor's view. `__lean_os__` is what the toybox port already
 * keys on (tools/build-toybox.sh passes -D__lean_os__ by hand today, and
 * stops needing to once this compiler exists), and `__unix__` is what a
 * configure script tests before it tests anything more specific. */
#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()          \
  do {                                    \
    builtin_define ("__lean_os__");       \
    builtin_define ("__lean_os");         \
    builtin_define ("__unix__");          \
    builtin_define ("__unix");            \
    builtin_assert ("system=lean_os");    \
    builtin_assert ("system=unix");       \
  } while (0)

/* The startup files, in the order the ELF ABI requires and for the
 * reason user_space/lib/crti.asm gives: _init and _fini are built out of
 * fragments, so crti.o has to open them before crtbegin.o contributes to
 * the middle, and crtn.o has to close them after crtend.o.
 *
 * crt1.o is user_space/lib/crt0.asm under the name the driver looks for
 * - see the Makefile's `sysroot` target, where the two are reconciled. */
#undef STARTFILE_SPEC
#define STARTFILE_SPEC "crt1.o%s crti.o%s crtbegin.o%s"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC "crtend.o%s crtn.o%s"

/* The library. -lc is enough because this libc is one archive; -lgcc is
 * added by the driver itself. Written with the group so that a program
 * whose object references something in libc that references libgcc links
 * without the caller having to order them. */
#undef LIB_SPEC
#define LIB_SPEC "-lc"

/* ---- the link, and the three cases it now has ------------------------
 *
 * M94 wrote "static always, because there is no dynamic loader on this
 * machine yet - M95 is where that changes and where this line grows a
 * -dynamic-linker." This is that line, grown.
 *
 *   -shared        a shared object. ld's own default script, because
 *                  that is what produces .dynamic, .interp and the
 *                  `_DYNAMIC` symbol a dynamic linker needs to find
 *                  itself. Passing -T here was M94's one real mistake in
 *                  this file: the fixed script has no .dynamic, so every
 *                  -shared link failed with "undefined reference to
 *                  _DYNAMIC" - a symbol nobody wrote and the linker
 *                  normally defines.
 *   -pie           a position-independent executable. Same reasoning:
 *                  ld's default script, and the kernel places it (see
 *                  elf_load_at). It gets an interpreter, which is what
 *                  makes it a dynamic program at all.
 *   otherwise      static, at 512 GiB, under the script every lean_os
 *                  program has always linked with.
 */
#undef LINK_SPEC
#define LINK_SPEC                                                       \
  "%{shared:-shared} "                                                  \
  "%{!shared:%{pie:-pie -dynamic-linker /lib/ld-lean.so}"               \
  "%{!pie:-static -T lean_os.ld%s}}"

/* Where the driver looks. Empty rather than /usr/lib, because
 * --sysroot supplies the prefix and a second copy of the path here would
 * be a thing to keep in step. */
#undef STANDARD_STARTFILE_PREFIX
#define STANDARD_STARTFILE_PREFIX "/usr/lib/"

/* No shared libraries yet, so no position-independent default and no
 * .init_array indirection through a loader. M95 is the milestone that
 * turns both of these over. */
#undef TARGET_LIBC_HAS_FUNCTION
#define TARGET_LIBC_HAS_FUNCTION no_c99_libc_has_function

/* This libc has __cxa_atexit? No - it has atexit (M94). Saying so is
 * what makes g++ emit a destructor registration this runtime can
 * actually service rather than a call to a symbol that does not exist;
 * M97 is where the answer changes. */
#undef DEFAULT_USE_CXA_ATEXIT
#define DEFAULT_USE_CXA_ATEXIT 0
