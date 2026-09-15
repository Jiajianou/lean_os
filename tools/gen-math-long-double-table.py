#!/usr/bin/env python3
"""Generate the long double reference table graded by /bin/mathltest.

Nothing here decides what a function's answer is. Every expected value in the
generated file is written as a __builtin_<name>l() call on constant
arguments, which GCC folds with MPFR at compile time, at this target's own
64-bit mantissa. tools/math-long-double-test.sh then requires the compiled
object to reference no math symbol, because a call left behind would be the
implementation under test answering its own exam.

The arguments carry a full 64-bit mantissa rather than a double's 53, drawn
from a fixed seed, so the eleven bits that only the extended format has are
part of what is being graded.
"""

import os
import random
import sys
from fractions import Fraction

MANTISSA_BITS = 64
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CASES = os.path.join(ROOT, "tests/math/long_double_cases.tsv")

TWO_ARGUMENT_KINDS = ("2", "P")
INTEGER_RETURNING = ("lroundl", "llroundl", "ilogbl")


def binary_exponent(value):
    """floor(log2(value)) for a positive Fraction, without walking binades -
    the sweeps here reach 2^-16000, where one step per binade is minutes."""
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    while Fraction(2) ** exponent > value:
        exponent -= 1
    while Fraction(2) ** (exponent + 1) <= value:
        exponent += 1
    return exponent


def round_to_long_double(value):
    value = Fraction(value)
    if value == 0:
        return Fraction(0)
    sign = -1 if value < 0 else 1
    magnitude = abs(value)
    exponent = binary_exponent(magnitude)
    step = Fraction(2) ** (exponent - MANTISSA_BITS + 1)
    units = int(magnitude / step)
    if (magnitude - units * step) * 2 >= step:
        units += 1
    return sign * units * step


def hex_literal(value):
    if value == 0:
        return "0.0L"
    sign = "-" if value < 0 else ""
    magnitude = abs(value)
    exponent = binary_exponent(magnitude)
    scaled = (magnitude / Fraction(2) ** exponent) * Fraction(2) ** (MANTISSA_BITS - 1)
    if scaled.denominator != 1:
        raise ValueError("%s does not fit a %d-bit mantissa" % (value, MANTISSA_BITS))
    fraction = int(scaled) - (1 << (MANTISSA_BITS - 1))
    digits = ("%016x" % (fraction << 1)).rstrip("0") or "0"
    return "%s0x1.%sp%+dL" % (sign, digits, exponent)


def with_full_mantissa(value, source):
    """Give a sweep point the low bits a double could not have carried.

    The jitter always shrinks the magnitude. A sweep states its endpoints as
    a domain - asinl over [-1, 1], acoshl from 1 - and eleven random bits
    added outward would step off it, which is a domain error rather than a
    hard case."""
    rounded = round_to_long_double(value)
    if rounded == 0:
        return rounded
    exponent = binary_exponent(abs(rounded))
    step = Fraction(2) ** (exponent - MANTISSA_BITS + 1)
    sign = -1 if rounded < 0 else 1
    return rounded - sign * source.getrandbits(11) * step


def linear_points(low, high, count, source):
    """Both endpoints exactly, everything between them jittered inward."""
    span = Fraction(high) - Fraction(low)
    out = []
    for index in range(count):
        value = Fraction(low) + span * Fraction(index, count - 1)
        if index == 0 or index == count - 1:
            out.append(round_to_long_double(value))
        else:
            out.append(with_full_mantissa(value, source))
    return out


def edge_points(low, high, count, source):
    """low + (high - low) * 2^-k. The only sweep that reaches the place
    acosh and its kind are hard: arbitrarily close to their domain edge."""
    span = Fraction(high) - Fraction(low)
    out = []
    for index in range(count):
        shift = index * 80 // (count - 1)
        value = Fraction(low) + span / (Fraction(2) ** shift)
        out.append(with_full_mantissa(value, source) if index else
                   round_to_long_double(value))
    return out


def without_zero(points, replacement):
    """A sweep across a symmetric range lands on zero, and zero is outside
    the domain of anything that asks a value for its exponent."""
    return [replacement if value == 0 else value for value in points]


def log_points(low_exponent, high_exponent, count, source):
    """Sweep the EXPONENT, which is the only spacing exp and log care about."""
    out = []
    for index in range(count):
        exponent = low_exponent + (high_exponent - low_exponent) * index // (count - 1)
        mantissa = Fraction(1) + Fraction(source.getrandbits(63), 1 << 63)
        out.append(round_to_long_double(mantissa * Fraction(2) ** int(exponent)))
    return out


NO_ZERO_ARGUMENT = ("ilogbl", "logbl")


def arguments_for(row, source):
    kind, low, high, count = row["kind"], row["lo"], row["hi"], row["points"]
    if kind in ("1", "i"):
        points = linear_points(low, high, count, source)
        if row["name"] in NO_ZERO_ARGUMENT:
            points = without_zero(points, round_to_long_double(
                Fraction(high) / Fraction(count * 7)))
        return [(value,) for value in points]
    if kind == "E":
        return [(value,) for value in edge_points(low, high, count, source)]
    if kind == "L":
        return [(value,) for value in log_points(int(low), int(high), count, source)]
    if kind == "2":
        left = linear_points(low, high, count, source)
        # A divisor of zero is a domain question, and a domain question is
        # not what a sweep grades - grade_special_values takes those.
        right = without_zero(linear_points(high, low, count, source),
                             round_to_long_double(Fraction(high) / 3))
        return list(zip(left, right))
    if kind == "P":
        bases = log_points(int(low), int(high), count, source)
        out = []
        for index, base in enumerate(bases):
            exponent = Fraction(low) + (Fraction(high) - Fraction(low)) * \
                Fraction(index, count - 1)
            out.append((abs(base), round_to_long_double(exponent / 8)))
        return out
    raise ValueError("unknown kind %r" % kind)


def read_cases(path):
    rows = []
    with open(path) as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 7:
                raise SystemExit("%s: malformed row %r" % (path, line))
            name, kind, low, high, count, tolerance, note = fields[:7]
            if kind == "-":
                rows.append({"name": name, "kind": kind, "note": note})
                continue
            rows.append({
                "name": name,
                "kind": kind,
                "lo": float(low),
                "hi": float(high),
                "points": int(count),
                "tolerance": tolerance,
                "note": note,
            })
    return rows


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: gen-math-long-double-table.py <output.c>")
    rows = read_cases(CASES)
    source = random.Random(20260915)

    out = [
        "/* Generated by tools/gen-math-long-double-table.py from",
        "   tests/math/long_double_cases.tsv. Do not edit by hand: the test",
        "   regenerates this file and compares, so an edit here is a failure",
        "   rather than a change.",
        "",
        "   Every expected value below is a __builtin_ call that GCC folds",
        "   with MPFR at compile time, at this target's own 64-bit mantissa.",
        "   tools/math-long-double-test.sh requires the object built from",
        "   this file to reference no math symbol at all, which is what",
        "   proves the folding happened rather than a call being left for",
        "   the library under test to answer. */",
        "",
        '#include "math_long_double_table.h"',
        "",
    ]

    functions = []
    for row in rows:
        if row["kind"] == "-":
            continue
        name = row["name"]
        pairs = arguments_for(row, source)
        two = row["kind"] in TWO_ARGUMENT_KINDS
        integer = name in INTEGER_RETURNING
        out.append("static const struct long_double_case case_%s[] = {" % name)
        for pair in pairs:
            call = "__builtin_%s(%s)" % (
                name, ", ".join(hex_literal(value) for value in pair))
            second = hex_literal(pair[1]) if two else "0.0L"
            if integer:
                out.append("    {%s, %s, 0.0L, (long long)(%s)},"
                           % (hex_literal(pair[0]), second, call))
            else:
                out.append("    {%s, %s, %s, 0},"
                           % (hex_literal(pair[0]), second, call))
        out.append("};")
        out.append("")
        functions.append((name, len(pairs), two, integer, row["tolerance"]))

    out.append("const struct long_double_function LONG_DOUBLE_FUNCTIONS[] = {")
    for name, count, two, integer, tolerance in functions:
        out.append('    {"%s", case_%s, %d, %d, %d, %sL},'
                   % (name, name, count, 1 if two else 0,
                      1 if integer else 0, tolerance))
    out.append("};")
    out.append("")
    out.append("const unsigned LONG_DOUBLE_FUNCTION_COUNT =")
    out.append("    (unsigned)(sizeof(LONG_DOUBLE_FUNCTIONS) /")
    out.append("               sizeof(LONG_DOUBLE_FUNCTIONS[0]));")
    out.append("")

    with open(sys.argv[1], "w") as handle:
        handle.write("\n".join(out))


if __name__ == "__main__":
    main()
