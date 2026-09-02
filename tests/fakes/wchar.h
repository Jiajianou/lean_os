/* tests/fakes/wchar.h - M88
 *
 * Not a fake. A redirect, and it is here for the reason the Makefile's
 * note gives for tests/fakes coming first on the include path: a header
 * placed here shadows the one the compiler would otherwise find.
 *
 * The thing under test is user_space/libc/src/wchar.c - this project's
 * own UTF-8 conversions - and it says `#include <wchar.h>`. Compiled for
 * the host, that would find the host's, whose `mbstate_t` is a different
 * struct with different members, and the file would not build. Putting
 * user_space/libc/include on the include path instead would shadow the
 * host's <string.h> and <stdio.h> for every other test in this binary,
 * which is a much larger blast radius for the same result.
 *
 * So: two headers are redirected by name (this one and errno.h) and
 * nothing else is. The code under test is the code that ships, which is
 * the property this whole tier is built to keep.
 */
#pragma once
#include "../../user_space/libc/include/wchar.h"
