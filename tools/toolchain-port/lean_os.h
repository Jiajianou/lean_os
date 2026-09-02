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
/* ---- M97: and the three cases these have, matching LINK_SPEC's -------
 *
 * M94 wrote one answer here and it was right for the one kind of thing
 * this target could produce. There are three now, and the differences
 * are not cosmetic:
 *
 *   an executable   crt1.o, which is this project's crt0 - the entry
 *                   point, the argv unpacking, the call to main.
 *   a PIE           Scrt1.o, the same startup with its two calls routed
 *                   through the PLT (user_space/lib/crt0-pie.asm), and
 *                   crtbeginS/crtendS, which are the -fPIC crtstuff.
 *   a shared object NO crt1 of any kind. A library has no entry point
 *                   and no main, and linking one in is not a harmless
 *                   extra: crt0's call to `main` is a PC-relative
 *                   reference to a symbol that does not exist, which
 *                   stops the link with "relocation R_X86_64_PC32
 *                   against symbol `__lean_start' can not be used when
 *                   making a shared object" - a message about PIC, from
 *                   a file that should not have been on the line.
 *
 * crti/crtn bracket _init and _fini in all three, for the reason
 * user_space/lib/crti.asm gives: they are built out of fragments, so
 * crti.o opens what crtbegin contributes to and crtn.o closes it. */
#undef STARTFILE_SPEC
#define STARTFILE_SPEC                                                  \
  "%{shared:crti.o%s crtbeginS.o%s} "                                   \
  "%{!shared:%{pie:Scrt1.o%s crti.o%s crtbeginS.o%s}"                   \
  "%{!pie:crt1.o%s crti.o%s crtbegin.o%s}}"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC                                                    \
  "%{shared|pie:crtendS.o%s crtn.o%s} "                                 \
  "%{!shared:%{!pie:crtend.o%s crtn.o%s}}"

/* The library. -lc is enough because this libc is one archive; -lgcc is
 * added by the driver itself. Written with the group so that a program
 * whose object references something in libc that references libgcc links
 * without the caller having to order them. */
#undef LIB_SPEC
#define LIB_SPEC "-lc"

/* ---- M97: which libgcc, and it is not always the archive -------------
 *
 * A static program gets libgcc.a, which is what M94 assumed and what the
 * default spec says. Anything dynamic has to get libgcc_s.so instead,
 * and the reason is not size:
 *
 * libgcc's exception machinery keeps a STATIC REGISTRY of the .eh_frame
 * tables it has been told about. Link the archive into an executable and
 * into a shared object and there are two registries. The library
 * registers its frames in its own copy at load time; the executable
 * throws through its own; the throw finds no handler, calls
 * std::terminate, and the program aborts with no message - from a build
 * whose single-object exception tests all pass. One unwinder is the
 * whole point of libgcc_s existing, and this is the line that asks for
 * it.
 *
 * `-lgcc` still follows it: libgcc_s exports the unwinder and the
 * runtime, and the compiler-internal helpers (__udivti3 and friends)
 * live only in the archive.
 *
 * And the static case gained `-lgcc_eh`, which is the other half of the
 * same change and was found the hard way. Asking for a shared libgcc
 * SPLITS the archive: the exception machinery moves out of libgcc.a and
 * into libgcc_eh.a, so a static link that used to get the unwinder for
 * free stops getting it. It does not fail at once, either - only a
 * program that reaches a part of libstdc++ which can throw pulls in the
 * object that needs it, so the first failure was one testsuite file out
 * of three with "undefined reference to `_Unwind_Resume`" while every
 * fixture in this milestone still linked. */
#undef LIBGCC_SPEC
#define LIBGCC_SPEC                                                     \
  "%{shared|pie:-lgcc_s -lgcc} "                                        \
  "%{!shared:%{!pie:-lgcc -lgcc_eh}}"

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

/* Which libc functions the compiler may assume exist when it optimises
 * one call into another - `printf("x\n")` into `puts`, a `sin`/`cos` pair
 * into `sincos`. M94 said "none of the C99 ones", which was conservative
 * and cost nothing.
 *
 * M97 leaves it alone, and that is deliberate rather than an oversight:
 * libstdc++'s configure probes for what it needs directly and does not
 * consult this, and widening it would let the compiler synthesise calls
 * into a libc this project is still filling in - which is a link error
 * in somebody else's build, at the end of a long compile. It becomes
 * worth revisiting when a measurement shows the missing transformations
 * cost something. */
#undef TARGET_LIBC_HAS_FUNCTION
#define TARGET_LIBC_HAS_FUNCTION no_c99_libc_has_function

/* This libc has __cxa_atexit? Since M97, yes - user_space/libc/src/env.c.
 *
 * M94 answered no, which was true and was the right answer for C: GCC
 * falls back to one destructor per translation unit registered with
 * plain atexit, and that services `static struct S s;` perfectly well.
 * It cannot service C++ with shared objects, because the fallback has
 * nowhere to record which object a static belongs to - so dlclose either
 * destroys nothing or destroys somebody else's. The three-argument form
 * carries the handle, which is the only reason it has three arguments. */
#undef DEFAULT_USE_CXA_ATEXIT
#define DEFAULT_USE_CXA_ATEXIT 1
