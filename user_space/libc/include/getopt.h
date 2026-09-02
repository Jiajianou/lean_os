/* user_space/libc/include/getopt.h - M89
 *
 * Option parsing. `getopt` is POSIX and belongs in <unistd.h>, which is
 * where it is also declared; `getopt_long` is GNU's and lives only here,
 * which is why a program that wants long options includes this file.
 *
 * This is a real implementation with no compromises to report, because
 * option parsing needs nothing from an operating system - it is a loop
 * over `argv`. It is here because every program somebody else wrote uses
 * it and this library did not have it.
 *
 * Two behaviours worth naming, because they are the ones that differ
 * between implementations:
 *
 *  - **Permutation.** GNU's getopt reorders `argv` so that options may
 *    follow operands ("tar file -x"). This one does NOT: it stops at the
 *    first non-option, which is POSIX's rule and what a leading '+' in
 *    the option string asks for elsewhere. A program relying on
 *    permutation sees its options as operands - and every configure
 *    script and every toybox command puts its options first, so this has
 *    not cost anything. Written down rather than discovered.
 *  - `opterr` and the leading ':'. A ':' at the start of the option
 *    string, or opterr set to 0, means "return ':' for a missing
 *    argument and do not print anything". Both are honoured.
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

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char *const argv[], const char *optstring);

#define no_argument       0
#define required_argument 1
#define optional_argument 2

struct option {
    const char *name;
    int has_arg;
    int *flag;   /* if non-NULL, *flag = val and getopt_long returns 0 */
    int val;
};

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);
int getopt_long_only(int argc, char *const argv[], const char *optstring,
                     const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif
