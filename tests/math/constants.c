/* The XSI constants, seen through THIS libc's <math.h> rather than the
   host's. tools/math-test.sh compiles this translation unit with
   -I user_space/libc/include, so <math.h> here is ours; main.c is compiled
   without it, so <math.h> there is the host's and the two cannot be the same
   header by accident. What main.c then compares against is not the host's own
   M_ macros - which would only say that two files agree about a decimal
   literal - but values the host's LIBM computes: acos(-1), sqrt(2), log(2). */

#include <math.h>

const double lean_math_constants[] = {
    M_E,      M_LOG2E, M_LOG10E,   M_LN2,   M_LN10,
    M_PI,     M_PI_2,  M_PI_4,     M_1_PI,  M_2_PI,
    M_2_SQRTPI, M_SQRT2, M_SQRT1_2,
};

const char *const lean_math_constant_names[] = {
    "M_E",      "M_LOG2E", "M_LOG10E",   "M_LN2",   "M_LN10",
    "M_PI",     "M_PI_2",  "M_PI_4",     "M_1_PI",  "M_2_PI",
    "M_2_SQRTPI", "M_SQRT2", "M_SQRT1_2",
};

const int lean_math_constant_count =
    (int)(sizeof(lean_math_constants) / sizeof(lean_math_constants[0]));
