/* tests/test_getopt.c - M89
 *
 * user_space/libc/src/getopt.c, compiled unmodified and run on the host.
 *
 * **Why this is not a differential test.** The shell, the regex engine
 * and scanf are all graded against the host's own implementation,
 * because their whole job is to agree with every other implementation.
 * getopt is the one where that is not true: GNU's permutes argv so that
 * options may follow operands, and BSD's does not, so "the host's
 * answer" depends on which machine the test runs on. There is no oracle
 * to defer to.
 *
 * So the expectations here are the specification, and <getopt.h> states
 * it: this implementation stops at the first non-option, which is the
 * POSIX behaviour. Every case below either follows from that sentence or
 * from a rule POSIX states outright, and the ones that are choices say
 * so.
 *
 * The state getopt keeps between calls is global, which is why every
 * test resets `optind` and `optarg` before it starts - a test that
 * inherited the previous one's position would pass or fail depending on
 * the order they ran in.
 */
#include "check.h"

#include <getopt.h>
#include <string.h>

/* getopt writes diagnostics to stderr for an unknown option. Silenced
 * for every test here: what is under test is the return value, and a
 * test suite that printed "invalid option" nine times would train
 * somebody to ignore its output. */
static void reset(void) {
    optind = 1;
    optarg = 0;
    optopt = 0;
    opterr = 0;
}

TEST(getopt, clustered_and_separate_flags_are_the_same_thing) {
    char *a1[] = {"prog", "-a", "-b", "-c", 0};
    char *a2[] = {"prog", "-abc", 0};

    reset();
    CHECK_EQ(getopt(4, a1, "abc"), 'a');
    CHECK_EQ(getopt(4, a1, "abc"), 'b');
    CHECK_EQ(getopt(4, a1, "abc"), 'c');
    CHECK_EQ(getopt(4, a1, "abc"), -1);

    reset();
    CHECK_EQ(getopt(2, a2, "abc"), 'a');
    CHECK_EQ(getopt(2, a2, "abc"), 'b');
    CHECK_EQ(getopt(2, a2, "abc"), 'c');
    CHECK_EQ(getopt(2, a2, "abc"), -1);
}

TEST(getopt, an_option_argument_attached_or_separate) {
    char *attached[] = {"prog", "-ofile", 0};
    char *separate[] = {"prog", "-o", "file", 0};

    reset();
    CHECK_EQ(getopt(2, attached, "o:"), 'o');
    CHECK(optarg && strcmp(optarg, "file") == 0);
    CHECK_EQ(getopt(2, attached, "o:"), -1);

    reset();
    CHECK_EQ(getopt(3, separate, "o:"), 'o');
    CHECK(optarg && strcmp(optarg, "file") == 0);
    CHECK_EQ(getopt(3, separate, "o:"), -1);
}

TEST(getopt, a_missing_argument_is_reported_and_which_one) {
    char *argv[] = {"prog", "-o", 0};

    /* Without a leading ':' the answer is '?'; with one it is ':', and
     * either way optopt names the option that wanted an argument. That
     * pair is the whole interface for "you forgot the filename". */
    reset();
    CHECK_EQ(getopt(2, argv, "o:"), '?');
    CHECK_EQ(optopt, 'o');

    reset();
    CHECK_EQ(getopt(2, argv, ":o:"), ':');
    CHECK_EQ(optopt, 'o');
}

TEST(getopt, an_unknown_option_is_a_question_mark) {
    char *argv[] = {"prog", "-z", 0};
    reset();
    CHECK_EQ(getopt(2, argv, "ab"), '?');
    CHECK_EQ(optopt, 'z');
}

TEST(getopt, parsing_stops_at_the_first_operand_and_at_a_double_dash) {
    /* This is the documented choice - see <getopt.h>. A GNU getopt would
     * permute and return 'b' here; this one leaves it as an operand. */
    char *operand_first[] = {"prog", "file", "-b", 0};
    reset();
    CHECK_EQ(getopt(3, operand_first, "ab"), -1);
    CHECK_EQ(optind, 1); /* "file" was not consumed */

    /* "--" ends the options and IS consumed, so optind points past it. */
    char *dashdash[] = {"prog", "-a", "--", "-b", 0};
    reset();
    CHECK_EQ(getopt(4, dashdash, "ab"), 'a');
    CHECK_EQ(getopt(4, dashdash, "ab"), -1);
    CHECK_EQ(optind, 3);

    /* A bare "-" is an operand, not an option - it means stdin to every
     * program that takes one, and getopt must hand it back rather than
     * treat it as an empty option cluster. */
    char *dash[] = {"prog", "-", 0};
    reset();
    CHECK_EQ(getopt(2, dash, "ab"), -1);
    CHECK_EQ(optind, 1);
}

TEST(getopt_long, a_long_option_with_and_without_an_argument) {
    static const struct option opts[] = {
        {"verbose", no_argument, 0, 'v'},
        {"output", required_argument, 0, 'o'},
        {"level", optional_argument, 0, 'l'},
        {0, 0, 0, 0},
    };
    int idx = -1;

    char *argv[] = {"prog", "--verbose", "--output=f", "--level", 0};
    reset();
    CHECK_EQ(getopt_long(4, argv, "vo:l::", opts, &idx), 'v');
    CHECK_EQ(idx, 0);
    CHECK_EQ(getopt_long(4, argv, "vo:l::", opts, &idx), 'o');
    CHECK(optarg && strcmp(optarg, "f") == 0);
    CHECK_EQ(getopt_long(4, argv, "vo:l::", opts, &idx), 'l');
    CHECK(optarg == 0); /* optional, and none was attached */
    CHECK_EQ(getopt_long(4, argv, "vo:l::", opts, &idx), -1);
}

TEST(getopt_long, a_separate_argument_and_an_unambiguous_prefix) {
    static const struct option opts[] = {
        {"output", required_argument, 0, 'o'},
        {"verbose", no_argument, 0, 'v'},
        {0, 0, 0, 0},
    };
    char *argv[] = {"prog", "--output", "f", "--verb", 0};
    reset();
    CHECK_EQ(getopt_long(4, argv, "o:v", opts, 0), 'o');
    CHECK(optarg && strcmp(optarg, "f") == 0);
    /* An unambiguous prefix is the long option - which is what lets a
     * person type `--verb`, and what makes an ambiguous one an error
     * rather than a guess. */
    CHECK_EQ(getopt_long(4, argv, "o:v", opts, 0), 'v');
}

TEST(getopt_long, an_ambiguous_prefix_is_refused_and_an_exact_name_is_not) {
    static const struct option opts[] = {
        {"verbose", no_argument, 0, 'v'},
        {"version", no_argument, 0, 'V'},
        {"ver", no_argument, 0, 'r'},
        {0, 0, 0, 0},
    };
    char *ambiguous[] = {"prog", "--verb", 0};
    reset();
    /* "verb" is a prefix of only "verbose" - so this one is NOT
     * ambiguous, which is the case a naive "count the prefixes"
     * implementation gets wrong in the other direction. */
    CHECK_EQ(getopt_long(2, ambiguous, "", opts, 0), 'v');

    char *really_ambiguous[] = {"prog", "--ve", 0};
    reset();
    CHECK_EQ(getopt_long(2, really_ambiguous, "", opts, 0), '?');

    /* An exact name wins over being a prefix of a longer one: "ver" is a
     * prefix of both "verbose" and "version" and is also a name in its
     * own right. Without the exact-match rule this is ambiguous, which
     * would make an option impossible to type. */
    char *exact[] = {"prog", "--ver", 0};
    reset();
    CHECK_EQ(getopt_long(2, exact, "", opts, 0), 'r');
}

TEST(getopt_long, the_flag_pointer_form_returns_zero_and_writes_the_value) {
    int flag = 0;
    struct option opts[] = {
        {"set", no_argument, &flag, 7},
        {0, 0, 0, 0},
    };
    char *argv[] = {"prog", "--set", 0};
    reset();
    CHECK_EQ(getopt_long(2, argv, "", opts, 0), 0);
    CHECK_EQ(flag, 7);
}

TEST(getopt_long, short_options_still_work_alongside_long_ones) {
    static const struct option opts[] = {
        {"long", no_argument, 0, 'L'},
        {0, 0, 0, 0},
    };
    char *argv[] = {"prog", "-a", "--long", "-b", 0};
    reset();
    CHECK_EQ(getopt_long(4, argv, "ab", opts, 0), 'a');
    CHECK_EQ(getopt_long(4, argv, "ab", opts, 0), 'L');
    CHECK_EQ(getopt_long(4, argv, "ab", opts, 0), 'b');
    CHECK_EQ(getopt_long(4, argv, "ab", opts, 0), -1);
}
