/* user_space/libc/include/assert.h - M63.
 *
 * ---- M111: the one standard header that must NOT be guarded ----------
 *
 * This file had `#pragma once` at the top for forty-eight milestones,
 * because every other header here has one and it looked like a
 * consistency fix waiting to happen. It is a bug, and C11 7.2 says so in
 * one sentence: <assert.h> "is designed to be included more than once,
 * and its behavior depends on the current setting of NDEBUG at the time
 * it is included". An include guard defeats exactly that.
 *
 * It was found by building GNU grep 3.11 with this project's own
 * compiler, and the failure is nothing like the cause. gnulib's config.h
 * contains, deliberately and with a comment:
 *
 *     #include <assert.h>
 *     #undef assert
 *
 * - it wants <assert.h>'s static_assert and not its assert, and it takes
 * for granted that the next `#include <assert.h>` in the translation
 * unit will put `assert` back. With the guard, nothing put it back, and
 * lib/dfa.c stopped 3,200 lines later with "implicit declaration of
 * function 'assert'" - naming a header it does include, twice.
 *
 * So: no guard, and an explicit `#undef` before each definition, which
 * is what makes a second inclusion after `#define NDEBUG` mean what the
 * standard says it means rather than being silently ignored.
 */

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell.
 *
 * The declaration below is the one part of this file that IS guarded:
 * re-declaring a function is legal, but re-opening `extern "C"` around
 * it on every inclusion is noise, and __assert_fail does not depend on
 * NDEBUG. */
#ifndef LEANOS_ASSERT_FAIL_DECLARED
#define LEANOS_ASSERT_FAIL_DECLARED
#ifdef __cplusplus
extern "C" {
#endif

void __assert_fail(const char *expr, const char *file, int line) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif /* LEANOS_ASSERT_FAIL_DECLARED */

#undef assert

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__))
#endif

/* C11 7.2's other requirement, and it is a separate sentence:
 * `static_assert` is a macro that expands to `_Static_assert`. gnulib
 * includes this header specifically to get it - the `#undef assert`
 * above is the price it pays for that - so a version of this file that
 * only had `assert` in it would have sent it down its fallback path. */
#if !defined __cplusplus && __STDC_VERSION__ < 202311L
#undef static_assert
#define static_assert _Static_assert
#endif
