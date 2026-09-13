#undef DRIVER_SELF_SPECS
#define DRIVER_SELF_SPECS                                               \
  "%{!mcmodel=*:"                                                       \
  "%{shared|pie|fpic|fPIC|fpie|fPIE:-mcmodel=small;:-mcmodel=large}} "  \
  "%{!fplt:%{!fno-plt:%{shared|pie|fpic|fPIC|fpie|fPIE:-fno-plt}}} "    \
  "%{!mred-zone:-mno-red-zone} "                                        \
  "%{!ftls-model=*:%{shared|fpic|fPIC:-ftls-model=initial-exec}} "      \
  "%{!fpic:%{!fPIC:%{!fpie:%{!fPIE:"                                    \
  "%{shared:-fPIC;pie:-fPIE;:-fno-pic}}}}}"

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

#undef CPP_SPEC
#define CPP_SPEC "%{pthread:-D_REENTRANT}"

#undef STARTFILE_SPEC
#define STARTFILE_SPEC                                                  \
  "%{shared:crti.o%s crtbeginS.o%s} "                                   \
  "%{!shared:%{pie:Scrt1.o%s crti.o%s crtbeginS.o%s}"                   \
  "%{!pie:crt1.o%s crti.o%s crtbegin.o%s}}"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC                                                    \
  "%{shared|pie:crtendS.o%s crtn.o%s} "                                 \
  "%{!shared:%{!pie:crtend.o%s crtn.o%s}}"

#undef LIB_SPEC
#define LIB_SPEC "%{!shared:%{pie:-l:ld-lean.so}} -lc"

#undef LIBGCC_SPEC
#define LIBGCC_SPEC                                                     \
  "%{shared|pie:-lgcc_s -lgcc} "                                        \
  "%{!shared:%{!pie:-lgcc -lgcc_eh}}"

#undef LINK_SPEC
#define LINK_SPEC                                                       \
  "%{shared:-shared} "                                                  \
  "%{!shared:%{pie:-pie -dynamic-linker /lib/ld-lean.so}"               \
  "%{!pie:-static --no-relax -T lean_os.ld%s}}"

#undef STANDARD_STARTFILE_PREFIX
#define STANDARD_STARTFILE_PREFIX "/usr/lib/"

#undef TARGET_LIBC_HAS_FUNCTION
#define TARGET_LIBC_HAS_FUNCTION no_c99_libc_has_function

#undef DEFAULT_USE_CXA_ATEXIT
#define DEFAULT_USE_CXA_ATEXIT 1
