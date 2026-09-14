#!/usr/bin/env python3

import ast
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MANIFEST = os.path.join(ROOT, "tools", "rust-port", "libc-constants.list")
OUTPUT = os.path.join(ROOT, "tools", "rust-port", "libc", "lean_os", "constants.rs")
COMPILER = os.path.join(ROOT, "build", "toolchain", "bin", "x86_64-lean_os-gcc")

CAST = re.compile(r"\(\s*(?:unsigned\s+|signed\s+|const\s+)*"
                  r"(?:char|short|int|long|long\s+long|"
                  r"in_addr_t|in_port_t|mode_t|off_t|rlim_t|sighandler_t|"
                  r"size_t|ssize_t|socklen_t|time_t|clock_t|"
                  r"uint8_t|uint16_t|uint32_t|uint64_t|"
                  r"int8_t|int16_t|int32_t|int64_t)"
                  r"(?:\s*\*)*\s*\)")
SUFFIX = re.compile(r"\b(0[xX][0-9a-fA-F]+|\d+)([uU][lL]*|[lL]+[uU]?)\b")
OCTAL = re.compile(r"\b0[0-7]+\b")

ALLOWED = (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant,
           ast.BitOr, ast.BitAnd, ast.BitXor, ast.LShift, ast.RShift,
           ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Div,
           ast.USub, ast.UAdd, ast.Invert)


def read_manifest():
    groups = []
    headers = []
    entries = []
    for line in open(MANIFEST):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("include "):
            headers.append(line[len("include "):].strip())
            continue
        if line.startswith("section "):
            groups.append((line[len("section "):].strip(), []))
            continue
        kind, name = line.split()
        if not groups:
            groups.append(("", []))
        groups[-1][1].append((kind, name))
        entries.append((kind, name))
    return headers, groups, entries


def expand(headers, entries, compiler):
    source = ["#define _GNU_SOURCE 1"]
    source += ['#include <%s>' % h for h in headers]
    for _, name in entries:
        source.append('@@ "%s" %s' % (name, name))
    text = "\n".join(source) + "\n"
    # The headers are read from the tree rather than from build/sysroot, so
    # that what this file says is what user_space/libc/include says today.
    # The two -I directories reproduce the order `make sysroot` installs them
    # in, which is what <signal.h>'s #include_next depends on.
    run = subprocess.run(
        [compiler, "-E", "-P",
         "-I", os.path.join(ROOT, "user_space", "libc", "include"),
         "-I", os.path.join(ROOT, "system_api", "include"),
         "-x", "c", "-"],
        input=text, capture_output=True, text=True)
    if run.returncode != 0:
        sys.stderr.write(run.stderr)
        raise SystemExit("gen-libc-constants: the headers did not preprocess")
    values = {}
    for line in run.stdout.splitlines():
        line = line.strip()
        if not line.startswith("@@"):
            continue
        rest = line[2:].strip()
        match = re.match(r'"([A-Za-z0-9_]+)"\s*(.*)', rest)
        if not match:
            raise SystemExit("gen-libc-constants: cannot read %r" % line)
        values[match.group(1)] = match.group(2).strip()
    return values


def evaluate(name, text):
    text = CAST.sub("", text)
    text = SUFFIX.sub(lambda m: m.group(1), text)
    text = OCTAL.sub(lambda m: "0o" + m.group(0)[1:], text)
    text = text.strip()
    if text == "":
        raise SystemExit("gen-libc-constants: %s expanded to nothing" % name)
    try:
        tree = ast.parse(text, mode="eval")
    except SyntaxError:
        raise SystemExit("gen-libc-constants: %s expanded to %r" % (name, text))
    for node in ast.walk(tree):
        if not isinstance(node, ALLOWED):
            raise SystemExit("gen-libc-constants: %s expanded to %r, which is "
                             "not an integer constant expression" % (name, text))
    return eval(compile(tree, "<constant>", "eval"))


PRELUDE = {
    "c_char", "c_schar", "c_uchar", "c_short", "c_ushort", "c_int", "c_uint",
    "c_long", "c_ulong", "c_longlong", "c_ulonglong", "c_float", "c_double",
    "c_void", "size_t", "ssize_t", "usize", "isize",
    "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64",
}

UNSIGNED = {
    "u8": 8, "u16": 16, "u32": 32, "u64": 64, "usize": 64,
    "c_uchar": 8, "c_ushort": 16, "c_uint": 32, "c_ulong": 64,
    "mode_t": 32, "in_addr_t": 32, "in_port_t": 16, "socklen_t": 32,
    "tcflag_t": 32, "cc_t": 8, "nfds_t": 32, "sigset_t": 32,
    "rlim_t": 64, "dev_t": 64, "ino_t": 64, "nlink_t": 32,
    "blksize_t": 64, "blkcnt_t": 64, "fsblkcnt_t": 64, "fsfilcnt_t": 64,
    "eventfd_t": 64,
}


SIGNED = {
    "c_schar": 8, "c_char": 8, "c_short": 16, "c_int": 32, "c_long": 64,
    "i8": 8, "i16": 16, "i32": 32, "i64": 64, "isize": 64, "ssize_t": 64,
    "pid_t": 32, "off_t": 64, "time_t": 64, "suseconds_t": 64,
    "clock_t": 64, "clockid_t": 32,
}


def render(kind, value):
    if value < 0 and kind in UNSIGNED:
        value += 1 << UNSIGNED[kind]
    # A flag spelled 0x80000000 in a header is an unsigned int in C and an
    # int in the struct field that carries it; the bits are the claim, so the
    # signed spelling of the same bits is what Rust gets.
    if kind in SIGNED and value >= 1 << (SIGNED[kind] - 1):
        value -= 1 << SIGNED[kind]
    return str(value)


def main():
    compiler = COMPILER
    if not os.path.exists(compiler):
        compiler = os.environ.get("LEANOS_CC", "")
    if not compiler or not os.path.exists(compiler):
        raise SystemExit("gen-libc-constants: no x86_64-lean_os-gcc to ask")

    headers, groups, entries = read_manifest()
    values = expand(headers, entries, compiler)

    out = []
    out.append("use crate::prelude::*;")
    for title, members in groups:
        out.append("")
        for kind, name in members:
            if name not in values:
                raise SystemExit("gen-libc-constants: %s never expanded" % name)
            number = evaluate(name, values[name])
            spelled = kind if kind in PRELUDE else "crate::%s" % kind
            out.append("pub const %s: %s = %s;"
                       % (name, spelled, render(kind, number)))
    out.append("")

    text = "\n".join(out)
    if len(sys.argv) > 1 and sys.argv[1] == "--check":
        have = open(OUTPUT).read() if os.path.exists(OUTPUT) else ""
        if have != text:
            raise SystemExit("gen-libc-constants: %s disagrees with the headers"
                             % os.path.relpath(OUTPUT, ROOT))
        return
    with open(OUTPUT, "w") as handle:
        handle.write(text)


main()
