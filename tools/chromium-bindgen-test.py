#!/usr/bin/env python3
"""Grade what bindgen believes this target's types look like.

Chromium generates Rust bindings for its own C++ headers with bindgen, which
parses them with libclang instead of compiling them. The command line it
assembles for that is built out of {{defines}}, {{include_dirs}} and
{{cflags}} - and nothing else. A toolchain that keeps this target's triple and
sysroot in gcc_toolchain's extra_cflags puts them in the tool command string,
which is the one place bindgen cannot look, so the bindings come out for
whatever platform libclang defaulted to. They compile. They are wrong.

M154 moved those flags into //build/config:default_toolchain_flags, which is a
config, which means {{cflags}} carries them. This is the instrument that says
so, and it is a differential one: every number below is computed twice, once
by x86_64-lean_os-gcc from the C headers and once by bindgen from the same
headers, and the two have to agree. Neither side is told the answer.

The last stage is the one that keeps this honest. It runs bindgen a second
time with the flags the build had BEFORE M154 and requires the answers to be
different - because a test that passes either way grades nothing.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE = os.path.join(ROOT, "tests", "chromium", "bindgen_layout.h")

STRUCTURES = [
    ("stat", ["st_mode", "st_size", "st_mtim", "st_atim", "st_ctim",
              "st_nlink", "st_ino", "st_dev", "st_uid", "st_blocks"]),
    ("timespec", ["tv_sec", "tv_nsec"]),
    ("tm", ["tm_sec", "tm_year", "tm_isdst", "tm_gmtoff", "tm_zone"]),
    ("lconv", ["decimal_point", "int_frac_digits", "int_n_sign_posn"]),
    ("dirent", ["d_ino", "d_type", "d_name"]),
    ("sockaddr_un", ["sun_family", "sun_path"]),
    ("sockaddr", ["sa_family", "sa_data"]),
    ("pollfd", ["fd", "events", "revents"]),
    # sa_handler is not named here because it is inside an anonymous union,
    # which bindgen calls __bindgen_anon_1 - a name only bindgen knows, so
    # asking both oracles for it would be asking one of them a question it
    # cannot have an opinion about. The size, the alignment and the two fields
    # after the union pin the layout anyway.
    ("sigaction", ["sa_mask", "sa_flags"]),
    ("bindgen_layout_sigset", ["value"]),
    ("bindgen_layout_identity", ["platform"]),
    ("bindgen_layout_scalars", ["a_long", "a_pointer", "a_long_double",
                                "a_wide_character"]),
]

CONSTANTS = [
    "O_RDONLY", "O_WRONLY", "O_RDWR", "O_CREAT", "O_NONBLOCK", "O_CLOEXEC",
    "SEEK_SET", "SEEK_CUR", "SEEK_END",
    "SIGCHLD", "SIGPIPE", "SIGSEGV", "SA_SIGINFO", "SA_RESTART",
    "AF_UNIX", "AF_INET", "SOCK_STREAM", "SOCK_DGRAM",
    "POLLIN", "POLLOUT", "POLLERR",
    "S_IFMT", "S_IFREG", "S_IFDIR", "S_IFLNK",
    "DT_REG", "DT_DIR", "DT_LNK",
    "CLOCK_REALTIME", "CLOCK_MONOTONIC",
]


def keys():
    out = []
    for name, fields in STRUCTURES:
        out.append("sizeof %s" % name)
        out.append("alignof %s" % name)
        for field in fields:
            out.append("offsetof %s.%s" % (name, field))
    for name in CONSTANTS:
        out.append("constant %s" % name)
    return out


def compiler_expressions():
    out = []
    for name, fields in STRUCTURES:
        out.append("sizeof(struct %s)" % name)
        out.append("_Alignof(struct %s)" % name)
        for field in fields:
            out.append("offsetof(struct %s, %s)" % (name, field))
    for name in CONSTANTS:
        out.append("(unsigned long)(%s)" % name)
    return out


def ask_the_compiler(gcc, work):
    """The first oracle: this target's own C compiler, folding at compile time.

    The values are read out of the assembly rather than out of a program that
    ran, for the reason M142 wrote down - a value this test had to execute
    something to obtain is a value the thing under test could have supplied.
    The generated translation unit calls nothing, and the check below says so.
    """
    source = os.path.join(work, "layout_probe.c")
    with open(source, "w") as handle:
        handle.write('#include <stddef.h>\n#include "%s"\n' % FIXTURE)
        handle.write("const unsigned long layout_probe_values[] = {\n")
        for expression in compiler_expressions():
            handle.write("    (unsigned long)(%s),\n" % expression)
        handle.write("};\n")
    assembly = os.path.join(work, "layout_probe.s")
    result = subprocess.run([gcc, "-std=gnu11", "-O2", "-S", "-o", assembly,
                             source], capture_output=True, text=True)
    if result.returncode != 0:
        return None, result.stderr.strip()
    text = open(assembly).read()
    if re.search(r"^\s*call\b", text, re.M):
        return None, "the probe translation unit calls something"
    values = [int(m.group(1)) for m in
              re.finditer(r"^\s*\.quad\s+(-?\d+)\s*$", text, re.M)]
    expected = len(compiler_expressions())
    if len(values) != expected:
        return None, "the assembly holds %d values, not %d" % (len(values),
                                                               expected)
    return dict(zip(keys(), values)), None


def ask_bindgen(source, flags, resource_directory, work, label):
    """The second oracle: libclang, reading the same headers bindgen reads.

    -resource-dir is not in cflags and is not this port's to choose:
    run_bindgen.py adds it from clang_base_path, and it decides which clang's
    builtin headers the parse gets. It is passed here for the same reason the
    flags come out of args.gn - so that what this grades is the invocation the
    build performs rather than one that resembles it.
    """
    output = os.path.join(work, "bindings_%s.rs" % label)
    command = [os.path.join(source, "third_party", "rust-toolchain", "bin",
                            "bindgen"),
               "--no-rustfmt-bindings", "--output", output, FIXTURE, "--",
               "-x", "c", "-resource-dir", resource_directory] + flags
    environment = dict(os.environ)
    environment["LIBCLANG_PATH"] = os.path.join(
        source, "third_party", "rust-toolchain", "lib")
    environment["DYLD_LIBRARY_PATH"] = os.path.join(
        source, "third_party", "llvm-build", "Release+Asserts", "lib")
    environment.pop("TARGET", None)
    result = subprocess.run(command, cwd=source, env=environment,
                            capture_output=True, text=True)
    if result.returncode != 0:
        return None, result.stderr.strip().split("\n")[0]
    return parse_bindings(open(output).read()), None


def parse_bindings(text):
    found = {}
    for match in re.finditer(
            r'\["Size of (\w+)"\]\s*\[[^\]]*?-\s*(\d+)usize\]', text):
        found["sizeof %s" % match.group(1)] = int(match.group(2))
    for match in re.finditer(
            r'\["Alignment of (\w+)"\]\s*\[[^\]]*?-\s*(\d+)usize\]', text):
        found["alignof %s" % match.group(1)] = int(match.group(2))
    for match in re.finditer(
            r'\["Offset of field: (\w+)::(\w+)"\]\s*\[[^\]]*?-\s*(\d+)usize\]',
            text):
        found["offsetof %s.%s" % (match.group(1), match.group(2))] = \
            int(match.group(3))
    for match in re.finditer(
            r'pub const (\w+)\s*:\s*\w+\s*=\s*(-?\d+)\s*;', text):
        found["constant %s" % match.group(1)] = int(match.group(2))
    return found


def read_configured_flags(args_gn):
    """The flags the build actually uses, read out of the build it uses them in.

    A copy of the list kept here would grade a copy. This grades args.gn.
    """
    text = open(args_gn).read()
    match = re.search(r"^default_toolchain_cflags\s*=\s*\[(.*?)\]", text,
                      re.M | re.S)
    if not match:
        return None
    return re.findall(r'"([^"]*)"', match.group(1))


def read_resource_directory(args_gn):
    """Which clang's builtin headers bindgen parses with.

    Chromium derives this from clang_base_path, which is the compiler the
    build is configured around - right whenever that is also the compiler
    building the target, and wrong here, because clang_base_path is a global
    the HOST toolchain reads too. M154's patch 0025 makes it an argument, and
    this reads the argument rather than re-deriving it.
    """
    text = open(args_gn).read()
    named = re.search(r'^libclang_resource_dir\s*=\s*"([^"]*)"', text, re.M)
    if named and named.group(1):
        return named.group(1)
    base = re.search(r'^clang_base_path\s*=\s*"([^"]*)"', text, re.M)
    version = re.search(r'^clang_version\s*=\s*"([^"]*)"', text, re.M)
    if not base or not version:
        return None
    return os.path.join(base.group(1), "lib", "clang", version.group(1))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-name",
                        default=os.environ.get("LEANOS_CHROMIUM_OUT",
                                               "LeanOS"))
    options = parser.parse_args()

    source = os.path.join(ROOT, "build", "chromium", "src")
    gcc = os.path.join(ROOT, "build", "toolchain", "bin", "x86_64-lean_os-gcc")
    bindgen = os.path.join(source, "third_party", "rust-toolchain", "bin",
                           "bindgen")
    args_gn = os.path.join(source, "out", options.out_name, "args.gn")

    for path, why in ((source, "no checkout - run tools/fetch-chromium.sh"),
                      (gcc, "no cross toolchain - run tools/build-toolchain.sh"),
                      (bindgen, "no Rust toolchain in the checkout"),
                      (args_gn, "no configured build in out/%s - run "
                                "tools/build-chromium.sh" % options.out_name)):
        if not os.path.exists(path):
            print("chromium-bindgen-test: %s - skipping" % why)
            return 0

    passed = 0
    failed = 0

    def check(condition, description):
        nonlocal passed, failed
        if condition:
            print("chromium-bindgen-test: pass - %s" % description)
            passed += 1
        else:
            print("chromium-bindgen-test: FAIL - %s" % description,
                  file=sys.stderr)
            failed += 1

    flags = read_configured_flags(args_gn)
    check(flags is not None,
          "the configured build names this target's flags in a config")
    if flags is None:
        return 1

    named = " ".join(flags)
    check("--target=x86_64-lean_os" in named, "and the triple is among them")
    check("--sysroot=" in named, "and the sysroot is among them")
    check("-D__lean_os__=1" in flags,
          "and the identity build_config.h branches on")

    resource_directory = read_resource_directory(args_gn)
    check(resource_directory is not None and
          os.path.isdir(os.path.join(resource_directory, "include")),
          "and the resource directory bindgen parses with is named too")
    if resource_directory is None:
        return 1
    # Joined, not two arguments: a cflag beginning with a slash reads as a
    # clang-cl option to anything downstream that parses this list. M155's
    # V8 build is where that stopped being theoretical.
    check(("-idirafter" + os.path.join(resource_directory, "include"))
          in flags,
          "and it is the same one -idirafter names, joined, which is the "
          "whole condition")
    check(not any(flag.startswith("/") for flag in flags),
          "and no flag in the list begins with a slash, which reads as "
          "clang-cl to a tool parsing them")

    work = tempfile.mkdtemp()
    try:
        truth, why = ask_the_compiler(gcc, work)
        check(truth is not None,
              "x86_64-lean_os-gcc answers for every type and constant%s"
              % ("" if truth is not None else " - %s" % why))
        if truth is None:
            return 1

        seen, why = ask_bindgen(source, flags, resource_directory, work,
                                  "configured")
        check(seen is not None,
              "bindgen parses this target's headers%s"
              % ("" if seen is not None else " - %s" % why))
        if seen is None:
            return 1

        missing = [key for key in truth if key not in seen]
        check(not missing, "and reports all %d facts%s" %
              (len(truth), "" if not missing else
               " - missing %d, first %s" % (len(missing), missing[0])))

        wrong = [(key, truth[key], seen[key]) for key in truth
                 if key in seen and truth[key] != seen[key]]
        for key, want, got in wrong[:10]:
            print("chromium-bindgen-test:   %s: gcc says %d, bindgen says %d"
                  % (key, want, got), file=sys.stderr)
        check(not wrong,
              "and agrees with the compiler about every one of them%s"
              % ("" if not wrong else " - %d disagree" % len(wrong)))

        # What this would have said before M154: the triple //build/config
        # derives from current_os, and nothing else. A pass here would mean the
        # comparison above proves nothing.
        stale, why = ask_bindgen(source, ["--target=x86_64-unknown-linux-gnu"],
                                 resource_directory, work, "stale")
        if stale is None:
            differs = True
            how = "bindgen cannot even parse them (%s)" % why[:60]
        else:
            disagreements = [key for key in truth
                             if key not in stale or truth[key] != stale[key]]
            differs = bool(disagreements)
            how = "%d of %d facts come out different" % (len(disagreements),
                                                         len(truth))
        check(differs,
              "and the pre-M154 flags would have got a different answer - %s"
              % how)

        # And the define on its own. The fixture sizes one struct by whether
        # __lean_os__ is set, so this says whether the identity reached the
        # parser rather than whether the headers did - two different flags
        # failing in two different ways, and a single check could not tell
        # which of them was doing the work.
        without_identity = [flag for flag in flags
                            if not flag.startswith("-D__lean_os")]
        unnamed, why = ask_bindgen(source, without_identity, resource_directory, work,
                                   "unnamed")
        key = "sizeof bindgen_layout_identity"
        check(unnamed is not None and unnamed.get(key) != truth[key],
              "and a build that did not name the platform would size "
              "bindgen_layout_identity %s rather than %d"
              % ("not at all" if unnamed is None else str(unnamed.get(key)),
                 truth[key]))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("chromium-bindgen-test: %d passed, %d failed" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
