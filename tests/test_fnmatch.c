/* tests/test_fnmatch.c - M89
 *
 * user_space/libc/src/fnmatch.c and libgen.c, compiled unmodified and
 * run on the host.
 *
 * These are host tests for the same reason the UTF-8 conversions are:
 * what makes them wrong is a pattern nobody would write on purpose. A
 * booted machine matches the patterns its own shell produces, and those
 * are the ones that already work. The interesting inputs are an
 * unterminated bracket, a range whose ends are backwards, `*` asked to
 * cross a '/', and the exponential backtracking case - none of which
 * appear on this disk and all of which a ported program will eventually
 * hand it.
 *
 * The oracle problem is real here and is worth naming: fnmatch's rules
 * are subtle enough that a test asserting what this implementation
 * happens to do would pass forever while being wrong. So every
 * expectation below is one the POSIX text states or one that `sh`'s own
 * globbing demonstrates, and the awkward ones say which.
 */
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
    NOMATCH("a?c", "ac");     /* ? matches exactly one character, never none */
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
    /* Collapsed stars mean one star. */
    MATCH("a**c", "abc");
    MATCH("**", "abc");
}

TEST(fnmatch, backtracking_is_linear_not_exponential) {
    /* The denial-of-service case, and the reason this implementation
     * records one open star rather than recursing. A naive matcher takes
     * time exponential in the number of stars here; this must return
     * promptly. The assertion is the result - the property being tested
     * is that the test finishes at all. */
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
    MATCH("[^abc]", "d");     /* '^' is the same as '!' - every shell accepts it */
    MATCH("x[0-9]y", "x7y");
    NOMATCH("x[0-9]y", "xay");
    /* A ']' first is a literal ']' rather than an empty expression -
     * POSIX says so explicitly and every shell agrees. */
    MATCH("[]]", "]");
    MATCH("[!]]", "a");
    NOMATCH("[!]]", "]");
    /* A range whose ends are backwards matches nothing rather than
     * everything, which is the failure a subtraction-based comparison
     * gets wrong. */
    NOMATCH("[z-a]", "m");
}

TEST(fnmatch, an_unterminated_bracket_is_a_literal) {
    /* What every shell does: `[abc` is four ordinary characters. An
     * implementation that instead ran off the end of the pattern would
     * read past the string, which is why this is a test and not a
     * comment. */
    MATCH("[abc", "[abc");
    NOMATCH("[abc", "a");
    MATCH("a[", "a[");
}

TEST(fnmatch, escaping) {
    MATCH("\\*", "*");
    NOMATCH("\\*", "anything");
    MATCH("\\?", "?");
    MATCH("\\[", "[");
    /* With FNM_NOESCAPE the backslash is an ordinary character, so the
     * pattern is two characters and matches two. */
    CHECK_EQ(fnmatch("\\*", "\\x", FNM_NOESCAPE), 0);
    CHECK_EQ(fnmatch("\\*", "*", FNM_NOESCAPE), FNM_NOMATCH);
}

TEST(fnmatch, pathname_stops_wildcards_at_a_slash) {
    /* The flag that makes glob mean what a shell means by it: `*` names
     * one component, never a path. */
    CHECK_EQ(fnmatch("*", "a/b", FNM_PATHNAME), FNM_NOMATCH);
    CHECK_EQ(fnmatch("*", "ab", FNM_PATHNAME), 0);
    CHECK_EQ(fnmatch("*/*", "a/b", FNM_PATHNAME), 0);
    CHECK_EQ(fnmatch("a/*", "a/b", FNM_PATHNAME), 0);
    CHECK_EQ(fnmatch("a/*", "a/b/c", FNM_PATHNAME), FNM_NOMATCH);
    CHECK_EQ(fnmatch("?", "/", FNM_PATHNAME), FNM_NOMATCH);
    /* Without the flag, `*` crosses freely. */
    MATCH("*", "a/b");
    MATCH("a*c", "a/b/c");
}

TEST(fnmatch, period_hides_dotfiles) {
    /* The rule that keeps `rm *` from deleting `.config`. */
    CHECK_EQ(fnmatch("*", ".hidden", FNM_PERIOD), FNM_NOMATCH);
    CHECK_EQ(fnmatch(".*", ".hidden", FNM_PERIOD), 0);
    CHECK_EQ(fnmatch("?hidden", ".hidden", FNM_PERIOD), FNM_NOMATCH);
    /* ...and without the flag it is an ordinary character. */
    MATCH("*", ".hidden");
}

TEST(fnmatch, casefold) {
    CHECK_EQ(fnmatch("ABC", "abc", FNM_CASEFOLD), 0);
    CHECK_EQ(fnmatch("[A-Z]", "q", FNM_CASEFOLD), 0);
    CHECK_EQ(fnmatch("ABC", "abc", 0), FNM_NOMATCH);
}

/* ---- basename and dirname -------------------------------------------
 *
 * Every case below is one POSIX tabulates, and the table exists because
 * these two functions are wrong in a first implementation for four
 * different inputs. Both may write to their argument, so each call gets
 * a fresh mutable copy - a string literal would fault, which is itself
 * the thing <libgen.h> warns about.
 */
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
    CHECK_STREQ(bn("/usr/"), "usr");      /* a trailing slash is not part of the name */
    CHECK_STREQ(bn("usr"), "usr");
    CHECK_STREQ(bn("/"), "/");            /* all slashes: the answer is "/" */
    CHECK_STREQ(bn("//"), "/");
    CHECK_STREQ(bn("."), ".");
    CHECK_STREQ(bn(".."), "..");
    CHECK_STREQ(bn(""), ".");             /* an empty path is "." */
    CHECK_STREQ(bn("a//b//"), "b");
}

TEST(fnmatch, dirname_matches_the_posix_table) {
    CHECK_STREQ(dn("/usr/lib"), "/usr");
    CHECK_STREQ(dn("/usr/"), "/");
    CHECK_STREQ(dn("usr"), ".");          /* no slash at all */
    CHECK_STREQ(dn("/"), "/");
    CHECK_STREQ(dn("//"), "/");
    CHECK_STREQ(dn("."), ".");
    CHECK_STREQ(dn(".."), ".");
    CHECK_STREQ(dn(""), ".");
    CHECK_STREQ(dn("/usr//lib"), "/usr"); /* the separator run collapses */
    CHECK_STREQ(dn("a/b"), "a");
}
