# tools/clang-port/cmake/Platform/lean_os.cmake - M121.
#
# CMake's description of this operating system, which is the documented
# way to add one: with `Platform/<CMAKE_SYSTEM_NAME>.cmake` on
# CMAKE_MODULE_PATH, `set(CMAKE_SYSTEM_NAME lean_os)` stops being
# "System is unknown to cmake" and starts being a supported cross target.
#
# ---- why this file exists rather than CMAKE_SYSTEM_NAME=Linux ----------
#
# Because that would be a lie with consequences. A project configured
# against a Linux platform description believes it has a dynamic loader
# with an ldconfig cache, a /proc, versioned shared-library soname rules,
# and `dlopen` in libdl - and libc++'s own CMake asks about several of
# those. This says what is actually here, and the places it differs from
# Linux.cmake are the interesting lines:
#
#   CMAKE_DL_LIBS is empty. dlopen/dlsym/dlclose are in ld-lean.so (M95,
#   M99) and there has never been a libdl holding stubs - which is
#   glibc's own arrangement since 2.34.
#
#   No rpath-link, no ldconfig: this loader reads DT_SONAME, searches
#   /lib and honours LD_LIBRARY_PATH, and has no cache. That is the same
#   set of true statements tools/toolchain-port/apply.py gives libtool.
#
# Nothing here is third-party source: it is this project's own
# description of its own OS, in the format CMake documents for the
# purpose.

set(UNIX 1)

# No libdl - see above.
set(CMAKE_DL_LIBS "")

set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".so")

set(CMAKE_SHARED_LIBRARY_C_FLAGS "-fPIC")
set(CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS "-shared")
set(CMAKE_SHARED_LIBRARY_CXX_FLAGS "-fPIC")
set(CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS "-shared")
set(CMAKE_SHARED_LIBRARY_SONAME_C_FLAG "-Wl,-soname,")
set(CMAKE_SHARED_LIBRARY_SONAME_CXX_FLAG "-Wl,-soname,")

# The loader honours LD_LIBRARY_PATH and searches /lib; it has no cache,
# so a runtime search path is the only thing that can be recorded.
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG "-Wl,-rpath,")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG_SEP ":")
set(CMAKE_SHARED_LIBRARY_RUNTIME_CXX_FLAG "-Wl,-rpath,")
set(CMAKE_SHARED_LIBRARY_RUNTIME_CXX_FLAG_SEP ":")

set(CMAKE_EXECUTABLE_FORMAT "ELF")

# The usual /usr prefixes, re-rooted through CMAKE_SYSROOT.
include(Platform/UnixPaths)
