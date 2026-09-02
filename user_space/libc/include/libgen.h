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

char *basename(char *path);
char *dirname(char *path);
