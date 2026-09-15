#pragma once

struct long_double_case {
    long double first;
    long double second;
    long double expected;
    long long expected_integer;
};

struct long_double_function {
    const char *name;
    const struct long_double_case *cases;
    unsigned count;
    unsigned two_arguments;
    unsigned integer_result;
    long double max_ulp;
};

extern const struct long_double_function LONG_DOUBLE_FUNCTIONS[];
extern const unsigned LONG_DOUBLE_FUNCTION_COUNT;
