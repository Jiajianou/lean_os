#!/usr/bin/env python3

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MANIFEST = os.path.join(ROOT, "tools", "rust-port", "abi-facts.list")
C_OUTPUT = os.path.join(ROOT, "tests", "rust", "abi_facts.c")
RUST_OUTPUT = os.path.join(ROOT, "tests", "rust", "std_probe", "src", "abi_facts.rs")

HEADERS = [
    "dirent.h", "errno.h", "fcntl.h", "grp.h", "limits.h", "locale.h", "netdb.h",
    "netinet/in.h", "poll.h", "pthread.h", "pwd.h", "sched.h", "signal.h",
    "stddef.h", "sys/epoll.h", "sys/mman.h", "sys/resource.h", "sys/select.h",
    "sys/socket.h", "sys/stat.h", "sys/statvfs.h", "sys/time.h", "sys/times.h",
    "sys/types.h", "sys/uio.h", "sys/un.h", "sys/utsname.h", "termios.h",
    "stdlib.h", "time.h", "unistd.h", "sys/eventfd.h",
]

SIGNED = {"c_int", "c_long", "c_short", "c_char", "i8", "i16", "i32", "i64",
          "isize", "ssize_t", "pid_t", "off_t", "time_t", "suseconds_t",
          "clock_t", "clockid_t"}


def read():
    entries = []
    for line in open(MANIFEST):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        kind, rest = line.split(" ", 1)
        parts = [part.strip() for part in rest.split("|")]
        entries.append((kind, parts))
    return entries


def main():
    entries = read()
    c_rows = []
    rust_rows = []

    for kind, parts in entries:
        if kind in ("struct", "scalar"):
            c_name, rust_name = parts[0], parts[1]
            fields = parts[2].split() if len(parts) > 2 else []
            c_rows.append('    {"sizeof(%s)", (unsigned long)sizeof(%s)},'
                          % (c_name, c_name))
            c_rows.append('    {"alignof(%s)", (unsigned long)_Alignof(%s)},'
                          % (c_name, c_name))
            rust_rows.append('    ("sizeof(%s)", size_of::<libc::%s>() as u64),'
                             % (c_name, rust_name))
            rust_rows.append('    ("alignof(%s)", align_of::<libc::%s>() as u64),'
                             % (c_name, rust_name))
            for field in fields:
                c_field, _, rust_field = field.partition("=")
                rust_field = rust_field or c_field
                c_rows.append('    {"offsetof(%s,%s)", (unsigned long)offsetof(%s, %s)},'
                              % (c_name, c_field, c_name, c_field))
                rust_rows.append(
                    '    ("offsetof(%s,%s)", core::mem::offset_of!(libc::%s, %s) as u64),'
                    % (c_name, c_field, rust_name, rust_field))
        elif kind == "const":
            name, kind_name = parts[0], parts[1]
            c_rows.append('    {"%s", (unsigned long)(long long)(%s)},' % (name, name))
            if kind_name in SIGNED:
                rust_rows.append('    ("%s", libc::%s as i64 as u64),' % (name, name))
            else:
                rust_rows.append('    ("%s", libc::%s as u64),' % (name, name))
        else:
            raise SystemExit("gen-abi-facts: unknown kind %r" % kind)

    c_text = "\n".join(
        ["#include <%s>" % h for h in HEADERS] + [
        "",
        "struct leanos_abi_fact {",
        "    const char *name;",
        "    unsigned long value;",
        "};",
        "",
        "static const struct leanos_abi_fact FACTS[] = {",
    ] + c_rows + [
        "};",
        "",
        "unsigned long leanos_abi_fact_count(void) {",
        "    return sizeof(FACTS) / sizeof(FACTS[0]);",
        "}",
        "",
        "const struct leanos_abi_fact *leanos_abi_facts(void) {",
        "    return FACTS;",
        "}",
        "",
    ])

    rust_text = "\n".join(
        ["pub static FACTS: &[(&str, u64)] = &["] + rust_rows + [
        "];",
        "",
    ])

    if len(sys.argv) > 1 and sys.argv[1] == "--check":
        for path, text in ((C_OUTPUT, c_text), (RUST_OUTPUT, rust_text)):
            have = open(path).read() if os.path.exists(path) else ""
            if have != text:
                raise SystemExit("gen-abi-facts: %s disagrees with abi-facts.list"
                                 % os.path.relpath(path, ROOT))
        return

    for path, text in ((C_OUTPUT, c_text), (RUST_OUTPUT, rust_text)):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as handle:
            handle.write(text)


main()
