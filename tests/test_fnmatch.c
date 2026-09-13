#include "check.h"

#include <fnmatch.h>
#include <libgen.h>
#include <string.h>

#define MATCH(p, s)    CHECK_EQ(fnmatch((p), (s), 0), 0)
#define NOMATCH(p, s)  CHECK_EQ(fnmatch((p), (s), 0), FNM_NOMATCH)

TEST(fnmatch, the_literal_and_single_character_cases) {
    MATCH("abc", "abc");
    NOMATCH("abc", "abd");
    NOMATCH("abc", "ab");
    NOMATCH("ab", "abc");
    MATCH("a?c", "abc");
    MATCH("???", "abc");
    NOMATCH("a?c", "ac");
    NOMATCH("?", "");
    MATCH("", "");
}

TEST(fnmatch, star_matches_any_run_including_none) {
    MATCH("*", "");
    MATCH("*", "anything");
    MATCH("a*", "a");
    MATCH("a*c", "ac");
    MATCH("a*c", "abc");
    MATCH("a*c", "abbbbc");
    NOMATCH("a*c", "ab");
    MATCH("*.c", "wchar.c");
    NOMATCH("*.c", "wchar.h");
    MATCH("a**c", "abc");
    MATCH("**", "abc");
}

TEST(fnmatch, backtracking_is_linear_not_exponential) {
    NOMATCH("a*a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    MATCH("a*a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab");
}

TEST(fnmatch, bracket_expressions) {
    MATCH("[abc]", "b");
    NOMATCH("[abc]", "d");
    MATCH("[a-z]", "q");
    NOMATCH("[a-z]", "Q");
    MATCH("[!abc]", "d");
    NOMATCH("[!abc]", "a");
    MATCH("[^abc]", "d");
    MATCH("x[0-9]y", "x7y");
    NOMATCH("x[0-9]y", "xay");
    MATCH("[]]", "]");
    MATCH("[!]]", "a");
    NOMATCH("[!]]", "]");
    NOMATCH("[z-a]", "m");
}

TEST(fnmatch, an_unterminated_bracket_is_a_literal) {
    MATCH("[abc", "[abc");
    NOMATCH("[abc", "a");
    MATCH("a[", "a[");
}

TEST(fnmatch, escaping) {
    MATCH("\\*", "*");
    NOMATCH("\\*", "anything");
    MATCH("\\?", "?");
    MATCH("\\[", "[");
    CHECK_EQ(fnmatch("\\*", "\\x", FNM_NOESCAPE), 0);
    CHECK_EQ(fnmatch("\\*", "*", FNM_NOESCAPE), FNM_NOMATCH);
}

TEST(fnmatch, pathname_stops_wildcards_at_a_slash) {
    CHECK_EQ(fnmatch("*", "a/b", FNM_PATHNAME), FNM_NOMATCH);
    CHECK_EQ(fnmatch("*", "ab", FNM_PATHNAME), 0);
    CHECK_EQ(fnmatch("*/*", "a/b", FNM_PATHNAME), 0);
    CHECK_EQ(fnmatch("a/*", "a/b", FNM_PATHNAME), 0);
    CHECK_EQ(fnmatch("a/*", "a/b/c", FNM_PATHNAME), FNM_NOMATCH);
    CHECK_EQ(fnmatch("?", "/", FNM_PATHNAME), FNM_NOMATCH);
    MATCH("*", "a/b");
    MATCH("a*c", "a/b/c");
}

TEST(fnmatch, period_hides_dotfiles) {
    CHECK_EQ(fnmatch("*", ".hidden", FNM_PERIOD), FNM_NOMATCH);
    CHECK_EQ(fnmatch(".*", ".hidden", FNM_PERIOD), 0);
    CHECK_EQ(fnmatch("?hidden", ".hidden", FNM_PERIOD), FNM_NOMATCH);
    MATCH("*", ".hidden");
}

TEST(fnmatch, casefold) {
    CHECK_EQ(fnmatch("ABC", "abc", FNM_CASEFOLD), 0);
    CHECK_EQ(fnmatch("[A-Z]", "q", FNM_CASEFOLD), 0);
    CHECK_EQ(fnmatch("ABC", "abc", 0), FNM_NOMATCH);
}

static char scratch[64];

static char *bn(const char *s) {
    strcpy(scratch, s);
    return basename(scratch);
}

static char *dn(const char *s) {
    strcpy(scratch, s);
    return dirname(scratch);
}

TEST(fnmatch, basename_matches_the_posix_table) {
    CHECK_STREQ(bn("/usr/lib"), "lib");
    CHECK_STREQ(bn("/usr/"), "usr");
    CHECK_STREQ(bn("usr"), "usr");
    CHECK_STREQ(bn("/"), "/");
    CHECK_STREQ(bn("//"), "/");
    CHECK_STREQ(bn("."), ".");
    CHECK_STREQ(bn(".."), "..");
    CHECK_STREQ(bn(""), ".");
    CHECK_STREQ(bn("a//b//"), "b");
}

TEST(fnmatch, dirname_matches_the_posix_table) {
    CHECK_STREQ(dn("/usr/lib"), "/usr");
    CHECK_STREQ(dn("/usr/"), "/");
    CHECK_STREQ(dn("usr"), ".");
    CHECK_STREQ(dn("/"), "/");
    CHECK_STREQ(dn("//"), "/");
    CHECK_STREQ(dn("."), ".");
    CHECK_STREQ(dn(".."), ".");
    CHECK_STREQ(dn(""), ".");
    CHECK_STREQ(dn("/usr//lib"), "/usr");
    CHECK_STREQ(dn("a/b"), "a");
}
