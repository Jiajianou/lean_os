# Packages

`os install grep`, and what is behind it.

Added in **M111**. This is the design, the threat model, and — the part
worth reading twice — the list of things it does not claim.

## What it is

```
os install <name>     install a package and whatever it needs
os remove  <name>     take one out again
os list               what is installed
os available          what the repository has
os info    <name>     one package, in detail
os verify  [name]     re-hash installed files against the archive
os caps               what this process may do, and what a package may
```

A package is a `.osp` archive: a header, a manifest, a file table and a
payload. It is built on the machine doing the porting by
`tools/os-pkg.c`, from a directory somebody else's `make install`
produced, and read on this machine by `/bin/os`. Both ends are
`user_space/lib/ospkg.c` — one reader, compiled twice.

```
/pkg/repo/<name>-<version>.osp   the repository, shipped in the image
/pkg/repo/index                  what is in it, with each archive's SHA-256
/pkg/<name>/<version>/...        an installed package, in its own prefix
/pkg/bin/<command>               a link to a command a package provides
/pkg/db/installed/<name>         the manifest of the installed version
/pkg/db/caps                     what each installed program may do
```

## The five properties

Each of these is something a person can check, not a word in a sentence.

### 1. Nothing runs at install time

**The format has no hooks.** No pre-install script, no post-install
action, no declarative rule that runs a program. Installing a package is:
verify it, then copy files. So the answer to *what can installing this
package do to my machine* is one sentence — it writes the files it
lists, under a directory named after itself — rather than an audit.

Every general-purpose package manager has the hook and regrets it. The
reason they have it is that a package sometimes needs to compile
something, register something or fix up a database, and on this machine
those are the package *manager's* jobs: done by code that shipped with
the OS and can be read once, rather than by code that arrives with each
package and has to be read every time.

If a package one day genuinely cannot be installed by copying, that is
worth discovering as a refusal rather than papering over with a
mechanism nothing needs yet.

### 2. Every byte is hashed, three times, for three different questions

| hash | over | answers |
|---|---|---|
| `index`'s `sha256` | the whole `.osp` | did the right archive arrive |
| the header's `body_sha256` | manifest + table + payload | is this archive intact |
| each record's `sha256` | one file's content | is what is on the disk now still what was installed |

The third is not redundant with the second: it is what `os verify`
compares against months later, when the archive may be long gone.

The hash is SHA-256, written here (`user_space/lib/sha256.c`) because
`os` is part of the OS and no third-party code ships in the OS. It is
graded by the standard's own vectors (`tests/test_sha256.c`) and by
`tools/pkg-test.sh`, which hashes 250 real files from this tree and
requires every answer to match the host's `shasum -a 256`.

leanfs's FNV-1a was not reused, and the reason is the whole point: FNV
is the right hash for catching a bit that rotted on a disk this machine
owns, and the wrong one for deciding whether bytes that arrived from
somewhere else are the bytes somebody meant to send.

### 3. A package cannot escape its prefix

Every path in an archive is checked against `ospkg_check_path` — no
absolute paths, no `..`, no `.`, no empty component, no backslash, no
control byte, no trailing slash — **before anything is created**. A
package manager that unpacks first and validates second is the one with
the traversal bug in it.

Then the joined destination is checked *again*, against the install
root, because the two can fail independently: the first asks whether the
name is allowed, the second asks whether what came out of the join is
still under the prefix.

### 4. A package cannot shadow a program this OS ships

Its commands go in `/pkg/bin`, never in `/bin`. `os install grep`
installs GNU grep as `/pkg/bin/grep`; `/bin/grep` stays toybox's (M89).
Which one a person gets is decided by their `PATH`, not by whoever
installed last.

### 5. The kernel decides what a package may do

This is the half that needed kernel work, and it exists because a
package manager opens a hole that was not there before.

`caps_for_program()` has matched on the **basename** since M65, and its
comment says why: `/bin/settings` and `settings` are the same program.
That was true for forty-six milestones because every program on the disk
came out of this repository. A package manager ends it — if an installed
executable is called `compositor`, the grant table hands it `CAP_ALL`,
not through a bug but by doing exactly what it was written to do, to a
file the table was never written about. The impersonation costs an
attacker nothing: it is a filename.

So the rule is about a **place** rather than a name:

- A program under `/pkg` never matches the shipped grant table, whatever
  it is called.
- What it gets is what `/pkg/db/caps` records for its path, intersected
  with `CAP_PKG_MAX` **in the kernel**.
- A program under `/pkg` the registry does not name gets
  `CAP_PKG_UNLISTED`, which is **zero** — not the default, nothing.

`CAP_PKG_MAX` is `fs-write | network | audio`. Not the framebuffer, not
power, not kill-any, not the process list, not the clipboard, not the
display mode, not the clock, not the log, and not pkg-admin. The
reasoning for each is in `system_api/include/caps.h`.

And the registry is not a file anything can edit, which is what makes it
worth reading at all:

**`CAP_PKG_ADMIN` gates every write whose path resolves under `/pkg`.**
Create, write, truncate, mkdir, rmdir, unlink, rename (both ends),
symlink and hard link (both ends), checked in the kernel at the same
door `CAP_FS_WRITE` is checked, on the *normalized* path. `/bin/os` is
the only shipped program that holds it.

Both ends of `rename` and `link` are gated, and the source end of `link`
is the one that matters: a hard link is a second name for an inode, so
`link("/pkg/grep/3.11/bin/grep", "/tmp/x")` followed by writing to
`/tmp/x` would modify an installed binary through a path that is not
under `/pkg`. Gating the destination alone would miss it entirely.

## What it does not claim

### A script outside `/pkg` still gets its interpreter's capabilities

A `#!` script under `/pkg` is intersected with what its package asked
for — that is the same rule as for a binary, and `[m111]` checks it. A
script *anywhere else* still runs with the grant of whatever its `#!`
line names, so a downloaded `.sh` launched from the terminal, whose
parent holds `CAP_ALL`, gets more than a downloaded *binary* in the same
directory would.

That asymmetry is older than this milestone and is not closed by it.
Closing it means intersecting every script with the shipped grant table,
which was tried and reverted: scripts are not in that table, so it took
every script on the machine to `CAP_APP_DEFAULT` — including the twenty
`#!/bin/sh` fixtures this kernel's own self-tests write into `/tmp`,
several of which launch programs that need the network. It closes
properly when a script has somewhere to declare what it needs.

### There are no signatures

Integrity, yes: these are the bytes that were built. **Authenticity, no:
nothing here says who built them.**

That is a decision rather than an omission. A signature needs a key, a
key needs distribution, and distribution needs somebody other than the
machine you are standing at. This OS has one principal, no login and no
key store; a signature check here would verify a key that shipped in the
same image as the thing it signs, which is a check that cannot fail and
is therefore not one — the exact fake check M65 was written to refuse.

**The condition under which this becomes real work:** when a package can
arrive from a machine this one did not build, which means `os install`
fetching over the network. TLS landed in M100 and the client exists, so
this is a near thing rather than a distant one — and it is the milestone
that should pay for a trust root, because it is the first one that has
somebody to trust.

### It is not a defence against editing the disk

`CAP_PKG_ADMIN` is a rule this kernel enforces while it is running.
Nothing stops another operating system, or a host tool, from writing
anything it likes into this image. The same is true of every file on
this machine and is stated here because a page about installing software
is where somebody would expect the opposite.

### It is not a sandbox

A package's programs run with the capabilities its manifest asked for,
which is a real and enforced boundary — but a package granted `fs-write`
can write anywhere on the disk except `/pkg`. There are no file owners
here (see [capabilities.md](capabilities.md)), so "this program may
write only its own data directory" is not a sentence this OS can
currently make true, and inventing a check for it would be decoration.

What *is* true: a package cannot modify another package, cannot modify
itself, cannot change what it is allowed to do, and cannot become the
compositor by being called one.

### One version at a time

`os install` refuses to install over a different installed version
rather than upgrading in place. Two versions' files in one directory is
the state that makes everything afterwards ambiguous, and an upgrade
that is `remove` then `install` is honest about what it is doing.

### A package cannot contain an empty directory

Directories are implied by the paths of the files in them. Nothing has
needed one; the day something does, the format gains a flag.

## Building a package

```sh
tools/build-packages.sh          # all of them
tools/build-packages.sh grep     # one
make packages                    # write the repository into the disk image
```

A manifest is `key: value` lines:

```
name: grep
version: 3.11
summary: GNU grep - print lines matching a pattern
provides: grep egrep fgrep
license: GPL-3.0-or-later
source: https://ftp.gnu.org/gnu/grep/grep-3.11.tar.xz
caps: network          # omitted entirely means "nothing at all"
requires: zlib         # installed first, recursively
```

An unknown **key** is ignored; an unknown **capability name** is
refused. The asymmetry is the rule *refuse what you cannot enforce,
ignore what you do not need*: a package built by a later version of this
tool should still install if everything this version enforces is
present, and a capability name this machine cannot enforce is either a
typo or a package built for a different machine.

## What is in the repository

| package | version | what it is |
|---|---|---|
| `grep` | 3.11 | GNU grep, built here with `x86_64-lean_os-gcc`, no source edits |
| `bzip2` | 1.0.8 | a plain Makefile port, asks for `fs-write` because it creates files |
| `impostor` | 1.0 | a fixture that installs binaries called `compositor` and `shutdown` and must get nothing |

The third is shipped on purpose. A boundary with no adversary in the
image is a boundary nothing checks.

## What grep cost

GNU grep builds for this target with **no edit to its source** and one
line added to its bundled `config.sub` — the upstream-shaped edit M94
already writes down. Getting there took four fixes to this project's own
C library, and every one was named by grep's build rather than by a
checklist:

- **`<assert.h>` had `#pragma once`.** C11 7.2 says that header is
  designed to be included more than once and to re-read `NDEBUG` each
  time. gnulib's `config.h` does `#include <assert.h>` then `#undef
  assert`, taking for granted that the next include puts it back; with
  the guard, nothing did, and `dfa.c` stopped 3,200 lines later on
  "implicit declaration of function 'assert'" — naming a header it
  includes twice.
- **`mbsinit` was missing.** Nothing here calls it and grep does not
  either: it *probes* for it, and gnulib's rule on a failed probe is to
  decide this platform's `mbstate_t` cannot be trusted, typedef its own
  as an `int`, and substitute its own `mbrtowc` but not its own
  `wcrtomb`. The error was a type mismatch about a state object nobody
  here asked for.
- **`creat` was missing** — `open` with three flags, under the name code
  older than those flags still uses.
- **Every function in `<ctype.h>` was `static inline`** and therefore in
  no object file. A configure script does not include a header, it
  *links*: `checking for isblank... no`, and then gnulib compiled its own
  `isblank`, which collided with the one in the header it could not see.
  The same trap was set for fifteen other names.

All four are the same family, and it is the one M94 named: **a missing
symbol is not a missing feature, it is a configure answer** — and the
substitution it triggers lands somewhere with no relation to the thing
that was absent.

## How it is graded

- `tests/test_ospkg.c`, `tests/test_sha256.c` — every refusal in the
  reader, and the hash against FIPS 180-4's vectors. Host tier, in
  `--fast`.
- `tools/pkg-test.sh` — this project's SHA-256 against the host's over
  250 real files, a round trip compared with `cmp` and `diff -r`,
  determinism, and a corrupted archive refused. In `--fast`.
- `[m111]`, `user_space/bin/pkgtest.c` — on the machine: `os install
  grep`, GNU grep run from `/pkg/bin` with both the positive and the
  negative answer checked, a package binary named `compositor` getting
  `0x0`, and — with `CAP_PKG_ADMIN` dropped by `sys_dropcaps` — every
  one of the nine write paths under `/pkg` refused, one at a time.

The last of those is the only instrument that can grade a kernel
enforcing something, and the nine separate refusals are separate on
purpose: they were nine edits in `kernel/arch/x86_64/syscall.c` and any
one of them could have been left out.
