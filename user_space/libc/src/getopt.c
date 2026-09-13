#include <getopt.h>

#include <stdio.h>
#include <string.h>

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;

static int nextchar;

static void report(const char *argv0, const char *message, int c) {
    if (!opterr) {
        return;
    }
    fprintf(stderr, "%s: %s -- %c\n", argv0 ? argv0 : "?", message, c);
}

static int do_getopt(int argc, char *const argv[], const char *optstring,
                     const struct option *longopts, int *longindex,
                     int long_only) {
    int quiet = 0;
    if (optstring && optstring[0] == ':') {
        quiet = 1;
        optstring++;
    }
    if (optstring && optstring[0] == '+') {
        optstring++;
    }

    optarg = 0;
    if (optind >= argc || !argv[optind]) {
        return -1;
    }
    if (nextchar == 0) {
        const char *w = argv[optind];
        if (w[0] != '-' || w[1] == '\0') {
            return -1;
        }
        if (w[1] == '-' && w[2] == '\0') {
            optind++;
            return -1;
        }
        int is_long = (w[1] == '-') || (long_only && longopts);
        if (is_long && longopts) {
            const char *name = w[1] == '-' ? w + 2 : w + 1;
            const char *eq = strchr(name, '=');
            size_t nlen = eq ? (size_t)(eq - name) : strlen(name);
            const struct option *match = 0;
            int index = -1;
            int ambiguous = 0;
            for (int i = 0; longopts[i].name; i++) {
                if (strncmp(longopts[i].name, name, nlen) != 0) {
                    continue;
                }
                if (strlen(longopts[i].name) == nlen) {
                    match = &longopts[i];
                    index = i;
                    ambiguous = 0;
                    break;
                }
                if (match) {
                    ambiguous = 1;
                } else {
                    match = &longopts[i];
                    index = i;
                }
            }
            if (long_only && !match && w[1] != '-') {
                goto short_option;
            }
            optind++;
            if (!match || ambiguous) {
                if (!quiet) {
                    report(argv[0], ambiguous ? "option is ambiguous"
                                              : "unrecognized option",
                           name[0]);
                }
                optopt = 0;
                return '?';
            }
            if (longindex) {
                *longindex = index;
            }
            if (match->has_arg == required_argument) {
                if (eq) {
                    optarg = (char *)eq + 1;
                } else if (optind < argc) {
                    optarg = argv[optind++];
                } else {
                    optopt = match->val;
                    return quiet ? ':' : '?';
                }
            } else if (match->has_arg == optional_argument) {
                optarg = eq ? (char *)eq + 1 : 0;
            } else if (eq) {
                if (!quiet) {
                    report(argv[0], "option takes no argument", name[0]);
                }
                return '?';
            }
            if (match->flag) {
                *match->flag = match->val;
                return 0;
            }
            return match->val;
        }
    short_option:
        nextchar = 1;
    }

    const char *w = argv[optind];
    int c = (unsigned char)w[nextchar];
    nextchar++;
    if (w[nextchar] == '\0') {
        optind++;
        nextchar = 0;
    }

    const char *spec = optstring ? strchr(optstring, c) : 0;
    if (!spec || c == ':') {
        optopt = c;
        if (!quiet) {
            report(argv[0], "invalid option", c);
        }
        return '?';
    }
    if (spec[1] == ':') {
        int optional = (spec[2] == ':');
        if (nextchar != 0) {
            optarg = (char *)w + nextchar;
            optind++;
            nextchar = 0;
        } else if (optional) {
            optarg = 0;
        } else if (optind < argc) {
            optarg = argv[optind++];
        } else {
            optopt = c;
            if (!quiet) {
                report(argv[0], "option requires an argument", c);
            }
            return quiet ? ':' : '?';
        }
    }
    return c;
}

int getopt(int argc, char *const argv[], const char *optstring) {
    return do_getopt(argc, argv, optstring, 0, 0, 0);
}

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    return do_getopt(argc, argv, optstring, longopts, longindex, 0);
}

int getopt_long_only(int argc, char *const argv[], const char *optstring,
                     const struct option *longopts, int *longindex) {
    return do_getopt(argc, argv, optstring, longopts, longindex, 1);
}
