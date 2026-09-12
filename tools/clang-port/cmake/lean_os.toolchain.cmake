# tools/clang-port/cmake/lean_os.toolchain.cmake - M121.
#
# A CMake toolchain file for x86_64-lean_os, used by
# tools/build-libcxx.sh. CLAUDE.md's fourth non-negotiable is about how
# *this project* is built - plain Makefiles, no CMake - and it holds:
# nothing in lean_os itself is configured by this file. libc++ is
# somebody else's source with its own build system, ported against this
# system the way CPython's autotools and NetSurf's own makefiles are.
#
# Expects two variables on the cmake command line:
#   LEANOS_PREFIX   the toolchain prefix (holds bin/clang and x86_64-lean_os/)
#   LEANOS_SYSROOT  build/sysroot
set(CMAKE_SYSTEM_NAME lean_os)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Where Platform/lean_os.cmake is. Set before the platform is determined,
# which is why it belongs in the toolchain file rather than on the
# command line.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

# ---- and the two of them have to survive a try_compile -----------------
#
# CMake re-includes a toolchain file inside the throwaway project it
# builds to identify the compiler, and that project does not inherit the
# parent's cache - so -DLEANOS_PREFIX= is simply absent there and this
# file failed with its own error message from inside CMakeSystem.cmake.
# CMAKE_TRY_COMPILE_PLATFORM_VARIABLES is the documented way to say which
# variables a toolchain file needs carried across, and it has to be set
# here rather than on the command line for the same reason.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LEANOS_PREFIX LEANOS_SYSROOT)

if(NOT LEANOS_PREFIX)
  message(FATAL_ERROR "pass -DLEANOS_PREFIX=<toolchain prefix>")
endif()
if(NOT LEANOS_SYSROOT)
  message(FATAL_ERROR "pass -DLEANOS_SYSROOT=<sysroot>")
endif()

set(CMAKE_C_COMPILER   "${LEANOS_PREFIX}/bin/clang")
set(CMAKE_CXX_COMPILER "${LEANOS_PREFIX}/bin/clang++")
set(CMAKE_ASM_COMPILER "${LEANOS_PREFIX}/bin/clang")

# The compiler's default target is already x86_64-lean_os (see
# LLVM_DEFAULT_TARGET_TRIPLE in tools/build-clang.sh), and saying it again
# here is deliberate: it is what makes the tree's own `--target=` checks
# agree with the compiler, and it keeps this file honest if that default
# ever changes.
set(CMAKE_C_COMPILER_TARGET   x86_64-lean_os)
set(CMAKE_CXX_COMPILER_TARGET x86_64-lean_os)

set(CMAKE_AR      "${LEANOS_PREFIX}/bin/x86_64-lean_os-ar")
set(CMAKE_RANLIB  "${LEANOS_PREFIX}/bin/x86_64-lean_os-ranlib")
set(CMAKE_NM      "${LEANOS_PREFIX}/bin/x86_64-lean_os-nm")
set(CMAKE_OBJDUMP "${LEANOS_PREFIX}/bin/x86_64-lean_os-objdump")
set(CMAKE_STRIP   "${LEANOS_PREFIX}/bin/x86_64-lean_os-strip")

set(CMAKE_SYSROOT "${LEANOS_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${LEANOS_SYSROOT}" "${LEANOS_PREFIX}/x86_64-lean_os")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---- why the compiler check builds an archive rather than a program ----
#
# Not because a program would not link - it does, and tools/clang-test.sh
# proves it four different ways. Because CMake's default check is
# compile-AND-LINK-AND-sometimes-run, and a check that tries to run an
# x86_64-lean_os binary on this Mac fails for a reason that has nothing to
# do with the compiler. This is the standard answer for a cross target and
# it keeps the configure deterministic.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
