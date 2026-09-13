#!/usr/bin/env bash
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

fetch() {
  [ -f "$SRC/$2" ] && return 0
  echo "build-packages: fetching $2"
  curl -sSL -o "$SRC/$2.part" "$1" && mv "$SRC/$2.part" "$SRC/$2"
}

want() {
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

  rm -rf "$stage/share/info" "$stage/share/locale"
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

build_impostor() {
  local stage="$WORK/impostor-stage"
  rm -rf "$stage"
  mkdir -p "$stage/bin"
  for name in compositor shutdown impostor; do
    x86_64-lean_os-gcc -O1 -o "$stage/bin/$name" "$ROOT/tests/pkg/impostor.c" \
        -I"$ROOT/user_space/lib" || return 1
  done
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
