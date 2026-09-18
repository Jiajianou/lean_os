#include "check.h"

#include <stddef.h>
#include <string.h>

/* libdl.a's four, which are what a STATICALLY linked program gets where a
 * dynamically linked one gets the loader's.
 *
 * The four names are renamed before the source is included, for the reason
 * tools/scanf-test.sh renames sscanf: this test runs on the host, whose own
 * libdl defines all four, and a second definition would either fail to link
 * or quietly shadow the thing being tested. Including the source rather than
 * linking it is what keeps the renames applying to the definitions as well
 * as to the calls.
 */
#define dlopen lean_dlopen
#define dlsym lean_dlsym
#define dlclose lean_dlclose
#define dlerror lean_dlerror
#include "../user_space/libc/libdl/dlfcn_static.c"
#undef dlopen
#undef dlsym
#undef dlclose
#undef dlerror

TEST(dlfcn_static, opening_a_shared_object_fails_and_says_why) {
    /* Drain anything a previous test left, so this one starts from no error. */
    (void)lean_dlerror();

    CHECK(lean_dlopen("libanything.so", 1) == NULL);

    const char *message = lean_dlerror();
    CHECK(message != NULL);
    /* The message has to say something about WHY, not just be non-empty: a
       caller printing it is the only way a person finds out that this
       program cannot load shared objects at all. */
    CHECK(strstr(message, "statically linked") != NULL);
}

TEST(dlfcn_static, a_symbol_lookup_fails_the_same_way) {
    (void)lean_dlerror();

    CHECK(lean_dlsym(NULL, "anything") == NULL);
    CHECK(lean_dlerror() != NULL);
}

TEST(dlfcn_static, closing_reports_failure_rather_than_success) {
    (void)lean_dlerror();

    /* dlclose(3) returns 0 on success. There is no handle this can have been
       given, so returning 0 would tell a caller that a thing it never opened
       has been closed. */
    CHECK(lean_dlclose(NULL) != 0);
    CHECK(lean_dlerror() != NULL);
}

TEST(dlfcn_static, dlerror_forgets_the_error_it_reported) {
    (void)lean_dlerror();

    CHECK(lean_dlopen("libanything.so", 1) == NULL);
    CHECK(lean_dlerror() != NULL);

    /* dlerror(3) reports the error since the LAST call to dlerror and then
       forgets it. A version that kept returning the same string would make a
       caller that loops until dlerror() is null loop for ever - which is the
       shape of the bug this check exists for. */
    CHECK(lean_dlerror() == NULL);
}

TEST(dlfcn_static, no_error_is_reported_when_nothing_failed) {
    (void)lean_dlerror();

    CHECK(lean_dlerror() == NULL);
}
