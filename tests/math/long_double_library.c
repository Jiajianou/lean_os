/* This library's long double half, wherever the compile's TARGET can run it.

   tools/math-test.sh links this and tests/math/host_long_double.c into every
   build, and the two are each other's complement, decided here by the target
   the compiler is building for rather than by a probe of the host in the
   script: where long double is the x87's 80 bits (an x86_64 Mac) this is
   math_long_double.c and the stand-in is empty; anywhere else (an arm64 Mac,
   where long double is double) this is empty and the stand-in answers.

   That is what lets tools/cross-arch-test.sh build the OTHER Mac's
   arrangement. It repeats every compile for the other architecture with the
   same arguments, so a choice made in the script picks one file for both -
   and an arm64 Mac never compiled math_long_double.c the way an Intel one
   does (Apple clang, -Wall -Wextra -Werror, the renames), nor nm-checked it.
   With the choice made here, each architecture's compile of this one file
   is that architecture's arrangement.

   MATH_TEST_LONG_DOUBLE=host defines LEAN_MATH_TEST_LONG_DOUBLE_HOST, which
   empties this on an x86_64 host too, so the arm64 arrangement can be run
   on a Mac that cannot run arm64. */
#if defined(__x86_64__) && __LDBL_MANT_DIG__ == 64 &&                         \
    !defined(LEAN_MATH_TEST_LONG_DOUBLE_HOST)
#include "../../user_space/libc/src/math_long_double.c"
#else
typedef int lean_math_test_long_double_is_the_stand_in;
#endif
