/* user_space/libc/include/fnmatch.h - M89
 *
 * Shell wildcard matching: `*`, `?`, `[...]` and their negations.
 *
 * This is the pattern language a shell uses for filenames, and it is a
 * different and much smaller language than <regex.h>'s. Keeping them
 * separate matters: `*` here means "any run of characters" and in a
 * regular expression it means "zero or more of the previous thing", and
 * a program that got one where it asked for the other would match the
 * wrong files silently.
 */
#pragma once

#define FNM_NOMATCH 1

#define FNM_NOESCAPE 0x01 /* a backslash is an ordinary character */
#define FNM_PATHNAME 0x02 /* a '/' must be matched by a literal '/' - never by * or ? */
#define FNM_PERIOD   0x04 /* a leading '.' must be matched literally, so * does not find dotfiles */
#define FNM_LEADING_DIR 0x08
#define FNM_CASEFOLD 0x10

int fnmatch(const char *pattern, const char *string, int flags);
