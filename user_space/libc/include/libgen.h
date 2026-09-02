/* user_space/libc/include/libgen.h - M89
 *
 * `basename` and `dirname`, and the thing worth knowing about both is
 * that they may modify the string they are given. That is not this
 * implementation being careless - it is what POSIX specifies, it is why
 * the argument is `char *` and not `const char *`, and a caller passing
 * a string literal will fault. Stated here because the alternative
 * (returning a pointer into a static buffer) is what the GNU version
 * does for basename and is a different function with the same name.
 */
#pragma once

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

char *basename(char *path);
char *dirname(char *path);

#ifdef __cplusplus
}
#endif
