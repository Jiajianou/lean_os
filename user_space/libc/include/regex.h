/* user_space/libc/include/regex.h - M89
 *
 * POSIX regular expressions: `regcomp`, `regexec`, `regerror`,
 * `regfree`.
 *
 * This is the largest single piece of the C library written here, and it
 * is here because `grep`, `sed`, `awk`, `find -regex` and `expr` are all
 * one function call away from being useless without it. It is also the
 * one part of this libc where a *plausible* implementation is genuinely
 * dangerous, so three properties are stated here rather than left to be
 * discovered.
 *
 * **Leftmost-longest, not leftmost-first.** POSIX specifies that the
 * match is the longest one starting at the earliest position; Perl and
 * most hand-written backtrackers return the first alternative that
 * happens to succeed. Against `(a|ab)` and the subject `ab`, POSIX says
 * `ab` and a leftmost-first engine says `a`. That difference is not
 * academic - it is what makes `sed 's/a\|ab/X/'` replace a different
 * span - so this engine tries every alternative at a start position and
 * keeps the longest.
 *
 * **The worst case is exponential, and this is the honest warning.**
 * The matcher backtracks, which is what makes backreferences possible at
 * all (POSIX BRE requires `\1`, and no linear-time automaton can do
 * them). A pattern like `(a*)*b` against a long run of `a`s will take
 * time this machine does not have. A Thompson construction would be
 * linear and would have to refuse backreferences; that trade is worth
 * revisiting the day something here matches a pattern it did not write,
 * and the condition for revisiting it is a measurement rather than a
 * mood - see M69's rule. Until then: patterns come from the person at
 * the keyboard, and the person can stop it with ^C (M85).
 *
 * **Subexpression capture follows the winning path, not POSIX's own
 * subexpression rules.** POSIX specifies, on top of the overall
 * leftmost-longest rule, that each subexpression should itself be as
 * long as possible consistent with the whole - a rule that in general
 * needs a different algorithm than backtracking. What this returns is
 * the capture set belonging to the longest overall match found. For
 * every pattern anything here writes the two agree; for a pattern
 * constructed to tell them apart they may not, and that is written down
 * because a caller reading `pmatch[1]` deserves to know which rule it
 * got.
 *
 * ---- two places POSIX does not decide, and this engine does ----------
 *
 * `tools/regex-test.sh` grades this engine against the host's own, and
 * every case in `tests/regex/cases.tsv` has to agree exactly. Two do
 * not, and both are questions the standard leaves open rather than bugs
 * on either side:
 *
 *  - **An empty pattern matches the empty string** rather than being a
 *    compile error. POSIX says an empty BRE or ERE is undefined; the
 *    host's engine rejects it. `grep ""` is a thing people type and
 *    scripts generate, and matching is more useful than failing.
 *
 *  - **REG_ICASE folds a backreference too.** `\(a\)\1` matches "aA"
 *    here and does not on the host, which compares the referenced text
 *    case-sensitively. REG_ICASE says to ignore case when matching, and
 *    a backreference is matching.
 */
#pragma once

#include <stddef.h>

/* regcomp() cflags */
#define REG_EXTENDED 0x01 /* ERE rather than BRE - see the note in regex.c on how much they differ */
#define REG_ICASE    0x02
#define REG_NOSUB    0x04 /* report only whether it matched; pmatch is not filled */
#define REG_NEWLINE  0x08 /* '.' and a negated class do not match '\n'; ^ and $ match at one */

/* regexec() eflags */
#define REG_NOTBOL   0x10 /* the subject does not begin a line, so ^ must not match at its start */
#define REG_NOTEOL   0x20

/* Return codes. REG_NOMATCH is the one a caller tests every time; the
 * rest are compilation errors, and each names a distinct mistake so
 * regerror() can say something better than "bad pattern". */
#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7  /* unbalanced [ ] */
#define REG_EPAREN    8  /* unbalanced ( ) */
#define REG_EBRACE    9  /* unbalanced { } */
#define REG_BADBR    10  /* the contents of {} are not a valid count */
#define REG_ERANGE   11  /* an invalid range endpoint, e.g. [z-a] */
#define REG_ESPACE   12  /* out of memory */
#define REG_BADRPT   13  /* a repeat with nothing to repeat: *abc, a** */

typedef long regoff_t;

/* An opaque compiled pattern. `re_nsub` is the only member a caller may
 * read - it is how many capturing subexpressions the pattern has, and
 * therefore how big a pmatch array has to be (re_nsub + 1, because entry
 * zero is the whole match). Everything else belongs to regcomp/regfree. */
typedef struct {
    size_t re_nsub;
    void  *re_prog;   /* the parsed pattern; freed by regfree */
    int    re_cflags;
} regex_t;

typedef struct {
    regoff_t rm_so; /* byte offset of the start, or -1 for a group that did not participate */
    regoff_t rm_eo; /* one past the end */
} regmatch_t;

int    regcomp(regex_t *preg, const char *pattern, int cflags);
int    regexec(const regex_t *preg, const char *string, size_t nmatch,
               regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);
void   regfree(regex_t *preg);
