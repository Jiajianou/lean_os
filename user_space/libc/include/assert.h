#ifndef LEANOS_ASSERT_FAIL_DECLARED
#define LEANOS_ASSERT_FAIL_DECLARED
#ifdef __cplusplus
extern "C" {
#endif

void __assert_fail(const char *expr, const char *file, int line) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif

#undef assert

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__))
#endif

#if !defined __cplusplus && __STDC_VERSION__ < 202311L
#undef static_assert
#define static_assert _Static_assert
#endif
