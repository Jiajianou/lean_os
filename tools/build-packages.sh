#!/usr/bin/env bash
# tools/build-packages.sh - M111: the repository this machine installs from.
#
# ---- what this is ------------------------------------------------------
#
# `os install grep` has to install something. This is where that
# something comes from: somebody else's source, downloaded here, built
# with this project's own x86_64-lean_os compiler, `make install`ed into
# a staging directory, and turned into a .osp archive by tools/os-pkg.c.
# The archives and an index go into the disk image as /pkg/repo.
#
# It is the same shape as tools/build-thirdparty.sh and deliberately not
# the same script. That one proves a port COMPILES; this one produces a
# package a person on the machine can install, which is a different
# artifact with a different test attached to it. The overlap is on
# purpose - a package built here is a port that already worked there.
#
# ---- what grep cost, which is the interesting part ---------------------
#
# GNU grep 3.11 builds for this target with **no edit to its source** and
# one line added to its bundled config.sub, which is what every
# distribution does and what M94 already writes down as upstream-shaped.
# Getting there needed four fixes to this project's own C library, and
# every one of them was named by grep's build rather than by a checklist:
#
#   <assert.h> had #pragma once. C11 7.2 says that header is designed to
#   be included more than once and to re-read NDEBUG each time. gnulib's
#   config.h does `#include <assert.h>` then `#undef assert`, taking for
#   granted that the next include puts it back; with the guard, nothing
#   did, and dfa.c stopped 3,200 lines later on "implicit declaration of
#   function 'assert'" naming a header it includes twice.
#
#   mbsinit was missing. Nothing here calls it and grep does not either -
#   it PROBES for it, and gnulib's rule when the probe fails is to decide
#   this platform's mbstate_t cannot be trusted, typedef its own as an
#   int, and substitute its own mbrtowc but not its own wcrtomb. The
#   error was a type mismatch in a function nobody here wrote about a
#   state object nobody here asked for.
#
#   creat was missing - `open` with three flags, under the name code
#   older than those flags still uses. gnulib's creat-safer.c.
#
#   Every function in <ctype.h> was `static inline` and therefore in no
#   object file. A configure script does not include a header, it LINKS,
#   so `checking for isblank... no` - and then gnulib compiled its own
#   isblank, which collided with the one in the header it could not see.
#   The same trap was set for fifteen other names. See ctype.h.
#
# All four are in the same family and it is the family M94 named: a
# missing SYMBOL is not a missing feature, it is a configure answer, and
# the substitution it triggers lands somewhere unrelated to the thing
# that was absent.
#
# Usage: tools/build-packages.sh [name ...]      (default: all of them)
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
if [ ! -x "$PREFIX/bin/x86_64-lean_os-gcc" ]; then
  echo "build-packages: no x86_64-lean_os-gcc - run tools/build-toolchain.sh" >&2
  exit 1
fi
export PATH="$PREFIX/bin:$PATH"

make -s -C "$ROOT" sysroot >/dev/null || exit 1
make -s -C "$ROOT" os-pkg  >/dev/null || exit 1

OS_PKG="$ROOT/build/os-pkg"
SRC="$ROOT/build/thirdparty-src"
WORK="$ROOT/build/packages"
REPO="$ROOT/build/repo"
mkdir -p "$SRC" "$WORK" "$REPO"

# Downloads land in $SRC and not in the working directory. Written out
# because the first version took the filename relative to wherever this
# script happened to be standing, which was the repository root - so the
# first run left two upstream tarballs sitting next to the Makefile.
fetch() {
  [ -f "$SRC/$2" ] && return 0
  echo "build-packages: fetching $2"
  curl -sSL -o "$SRC/$2.part" "$1" && mv "$SRC/$2.part" "$SRC/$2"
}

want() {
  # No names on the command line means all of them.
  [ $# -eq 0 ] && return 0
  return 1
}
SELECTED=("$@")
selected() {
  [ ${#SELECTED[@]} -eq 0 ] && return 0
  for s in "${SELECTED[@]}"; do
    [ "$s" = "$1" ] && return 0
  done
  return 1
}

# ---- grep 3.11 ---------------------------------------------------------
#
# The headline package, because `os install grep` is the sentence this
# milestone exists to make true - and because grep is a real program with
# a real regular-expression engine rather than a demonstration.
#
# --disable-perl-regexp because PCRE is a library this machine does not
# have, and grep's own regex engine is the interesting half anyway.
# --disable-nls because there is no gettext here; the strings stay
# English, which is what every other port on this machine already does.
build_grep() {
  local V=3.11
  fetch https://ftp.gnu.org/gnu/grep/grep-$V.tar.xz grep-$V.tar.xz
  ( cd "$SRC" && rm -rf grep-$V && tar xf grep-$V.tar.xz ) || return 1
  python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
          "$SRC/grep-$V/build-aux/config.sub" >/dev/null || return 1

  local stage="$WORK/grep-stage"
  rm -rf "$stage"
  (
    cd "$SRC/grep-$V" &&
    ./configure --host=x86_64-lean_os --disable-nls --disable-perl-regexp \
                --prefix= >configure.out 2>&1 &&
    make -j8 >make.out 2>&1 &&
    make install DESTDIR="$stage" >install.out 2>&1
  ) || {
    echo "build-packages: grep did not build - $SRC/grep-$V/make.out" >&2
    tail -20 "$SRC/grep-$V/make.out" 2>/dev/null >&2
    return 1
  }

  # `make install` leaves the info and locale trees, which are megabytes
  # of documentation for a machine with no `info` reader. The manual
  # pages stay: `os install grep` putting a man page on the disk that
  # nothing reads yet is a file that costs 8 KiB and is true, and the
  # thing that will read it is a later milestone rather than a fiction.
  rm -rf "$stage/share/info" "$stage/share/locale"
  # egrep and fgrep are shell scripts that call grep; they belong to the
  # package and are kept, which also makes this the first package whose
  # payload is not all ELF.
  cat > "$WORK/grep.manifest" <<'EOF'
name: grep
version: 3.11
summary: GNU grep - print lines matching a pattern
provides: grep egrep fgrep
license: GPL-3.0-or-later
source: https://ftp.gnu.org/gnu/grep/grep-3.11.tar.xz
# No caps: line. grep reads files and writes to the descriptors its
# launcher hands it, and that is the whole of what it needs - so it is
# installed with no capability at all, not even fs-write. A package that
# asks for nothing is the common case and should look like this one.
EOF
  "$OS_PKG" build "$WORK/grep.manifest" "$stage" "$REPO/grep-3.11.osp" || return 1
}

# ---- bzip2 1.0.8 -------------------------------------------------------
#
# A second package, and a different shape: a plain Makefile with no
# configure, already built by tools/build-thirdparty.sh since M94. It is
# here so that the repository has more than one entry and so that `os
# install` is exercised against a package that provides several command
# names for one binary.
build_bzip2() {
  local V=1.0.8
  fetch https://sourceware.org/pub/bzip2/bzip2-$V.tar.gz bzip2-$V.tar.gz
  ( cd "$SRC" && rm -rf bzip2-$V && tar xf bzip2-$V.tar.gz ) || return 1
  local stage="$WORK/bzip2-stage"
  rm -rf "$stage"
  mkdir -p "$stage/bin" "$stage/share/man/man1"
  (
    cd "$SRC/bzip2-$V" &&
    make -s CC=x86_64-lean_os-gcc AR=x86_64-lean_os-ar \
            RANLIB=x86_64-lean_os-ranlib bzip2 >make.out 2>&1
  ) || {
    echo "build-packages: bzip2 did not build - $SRC/bzip2-$V/make.out" >&2
    return 1
  }
  cp "$SRC/bzip2-$V/bzip2" "$stage/bin/bzip2"
  cp "$SRC/bzip2-$V/bzip2.1" "$stage/share/man/man1/bzip2.1"
  cat > "$WORK/bzip2.manifest" <<'EOF'
name: bzip2
version: 1.0.8
summary: bzip2 - a block-sorting file compressor
provides: bzip2
license: bzip2-1.0.6
source: https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
caps: fs-write
# bzip2 asks for fs-write and grep does not, and the difference is the
# point of the field: bzip2 CREATES a file (foo.bz2) and grep never does.
EOF
  "$OS_PKG" build "$WORK/bzip2.manifest" "$stage" "$REPO/bzip2-1.0.8.osp" || return 1
}

# ---- impostor 1.0: a package that tries to be the compositor ----------
#
# Not somebody else's software - a fixture written here (tests/pkg/
# impostor.c), built with the same cross compiler and shipped in the
# repository on purpose. It installs binaries called `compositor` and
# `shutdown`, which are names in the capability grant table with CAP_ALL
# and CAP_POWER attached, and it exits with its own capability mask so
# that user_space/bin/pkgtest.c can assert the number is zero.
#
# A boundary with no adversary in the image is a boundary nothing checks.
build_impostor() {
  local stage="$WORK/impostor-stage"
  rm -rf "$stage"
  mkdir -p "$stage/bin"
  for name in compositor shutdown impostor; do
    x86_64-lean_os-gcc -O1 -o "$stage/bin/$name" "$ROOT/tests/pkg/impostor.c" \
        -I"$ROOT/user_space/lib" || return 1
  done
  # And a `#!` script, which is a second way to be launched and was a
  # second way to be over-granted. Until M111 a script's capabilities
  # came from its INTERPRETER, so a package shipping one line starting
  # `#!/bin/sh` was launched with the shell's CAP_ALL - CAP_PKG_ADMIN
  # included, which is authority over every installed package on the
  # machine. This script tries to use exactly that, and pkgtest requires
  # the file it tries to create not to exist afterwards.
  cat > "$stage/bin/writer" <<'SCRIPT'
#!/bin/sh
# A package script trying to write into the package database. It must
# fail - see user_space/bin/pkgtest.c and docs/packages.md.
echo intruder > /pkg/db/intruder
SCRIPT
  chmod +x "$stage/bin/writer"
  cat > "$WORK/impostor.manifest" <<'EOF'
name: impostor
version: 1.0
summary: a test fixture that tries to be the compositor and must not be
provides: impostor
license: none
source: tests/pkg/impostor.c
# No caps, and that is the assertion: this package asks for nothing, so
# every one of its programs must run with nothing - including the two
# whose file names are in the shipped grant table.
EOF
  "$OS_PKG" build "$WORK/impostor.manifest" "$stage" "$REPO/impostor-1.0.osp" || return 1
}

# ---- ca-certificates 1.0: the thing https was missing ------------------
#
# M100 shipped no certificate authorities and said why (M65's rule: a
# machine that trusts a hundred and fifty authorities it has never
# looked at, shipped by a project whose claim is that you can read
# everything on the disk, would be decoration). It also wrote down the
# condition for reopening that: **a way to update the bundle without
# rebuilding the image.** M111 built it, so this is that.
#
# Why it is a package and not a file in the image, restated because the
# distinction is the whole point:
#
#   - Installing it is a decision somebody makes, by name, once. An
#     image that has never had `os install ca-certificates` run on it
#     trusts nobody, which is the state M100 chose deliberately.
#   - It can be replaced without rebuilding the OS. A CA bundle is the
#     one piece of a system that MUST be updatable on its own: an
#     authority is removed from it when it has done something wrong,
#     and "rebuild the kernel" is not an acceptable answer to that.
#   - It lives under /pkg, so it is subject to M111's rule that a
#     package cannot write outside its own prefix.
#
# The bundle is the HOST's, copied - not downloaded, and not written
# here. That is the honest description: this is somebody else's trust
# store, and a person installing it is trusting whoever curates the
# machine this was built on. It is data rather than code, so it ships in
# a package rather than in the OS, which is exactly the boundary
# CLAUDE.md's first non-negotiable draws.
build_ca_certificates() {
  local V=1.0
  local src=""
  for c in /etc/ssl/cert.pem /etc/ssl/certs/ca-certificates.crt \
           /usr/local/etc/openssl/cert.pem; do
    [ -f "$c" ] && { src="$c"; break; }
  done
  if [ -z "$src" ]; then
    echo "build-packages: no CA bundle on this host - looked at" >&2
    echo "                /etc/ssl/cert.pem and two others" >&2
    return 1
  fi
  local n
  n=$(grep -c "BEGIN CERTIFICATE" "$src")
  if [ "$n" -lt 20 ]; then
    echo "build-packages: $src has only $n certificates - that is not a bundle" >&2
    return 1
  fi
  local stage="$WORK/ca-certificates-stage"
  rm -rf "$stage"
  mkdir -p "$stage/share/ca-certificates"
  cp "$src" "$stage/share/ca-certificates/ca-bundle.pem" || return 1
  cat > "$WORK/ca-certificates.manifest" <<EOF
name: ca-certificates
version: $V
summary: certificate authorities, so https can be verified
provides:
license: MPL-2.0
source: $src ($n certificates, from this build host's trust store)
# No caps, and there is nothing here that could use one: this package
# contains a single file of text and not one executable. It is the
# smallest possible demonstration that a package is data plus a manifest
# rather than a program that runs.
EOF
  "$OS_PKG" build "$WORK/ca-certificates.manifest" "$stage" \
      "$REPO/ca-certificates-$V.osp" || return 1
}

rc=0
for p in grep bzip2 impostor ca_certificates; do
  selected "$p" || continue
  echo "build-packages: $p"
  "build_$p" || { echo "build-packages: $p FAILED" >&2; rc=1; }
done

"$OS_PKG" index "$REPO" || rc=1
echo "build-packages: the repository is $REPO"
ls -la "$REPO"
exit $rc
