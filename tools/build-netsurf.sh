#!/usr/bin/env bash
# tools/build-netsurf.sh - M100: a web browser, built for this machine.
#
# M100's fifth bullet is "a real but small engine - NetSurf has its own
# layout engine and a framebuffer front end". This is that, and it is
# the last thing in this project's library arc that is a PROGRAM rather
# than a library: HTML through libhubbub, the DOM through libdom, CSS
# through libcss, layout and painting in NetSurf itself, JavaScript
# through Duktape, images through libpng/libjpeg/libnsgif/libnsbmp/
# libsvgtiny, text through freetype, and http/https through libcurl over
# mbedtls over M66's TCP.
#
# ---- what is NOT here, and it is the interesting part -----------------
#
# There is no NetSurf front end for lean_os. A front end is a permanent
# fork of somebody else's program, and this project's rule (CLAUDE.md,
# third_party) is that outside source is ported *against* rather than
# merged into.
#
# What there is instead is ONE FILE - user_space/bin/nsfb_leanos.c -
# which registers a libnsfb display surface at runtime and is added to
# libnsfb.a here. NetSurf then selects it by name, `-f leanos`, exactly
# as it would select SDL. libnsfb's `_nsfb_register_surface` is a
# runtime call, the front end picks its surface by name at runtime, and
# the front end already links libnsfb with --whole-archive. Three facts
# upstream chose for its own reasons, and together they are the seam.
#
# Source edits to NetSurf: **two**, both in tools/netsurf-port/apply.py,
# and neither of them about lean_os. One is `/bin/which`, which does not
# exist on macOS; the other is `echo -n`, which this desk's /bin/sh
# prints literally. Both would be needed to cross-compile NetSurf to
# ANY target from this machine. Nothing about the browser was changed.
#
# ---- what building it found, which is the point (M63's rule) ----------
#
# Every line below is a fact about THIS system, named by a build rather
# than by a checklist:
#
#   pread/pwrite did not exist.  libnsutils wraps them for NetSurf's
#   disc cache. They are now SYS_pread/SYS_pwrite (111/112) and honest
#   rather than an lseek sandwich - vfs_handle_read had always taken the
#   offset, so the descriptor's `offset` field was the only thing in the
#   way. See system_api/include/syscall.h.
#
#   The lround family did not exist.  libsvgtiny calls lroundf. round()
#   had been here since M99 and its four integer spellings had not,
#   which is the usual shape of a libm gap: the hard part written, the
#   easy part missing. Graded by tools/math-test.sh, which needed a new
#   kind of row - the first functions in <math.h> that do not return a
#   floating-point type.
#
#   scandir/alphasort did not exist.  NetSurf's file: fetcher builds the
#   index page for a directory out of them. That is how a browser shows
#   you a folder.
#
#   STDIN_FILENO did not exist.  curl's terminal.c asks whether it is a
#   tty. fd 0 has been stdin on this machine since M14; the three names
#   POSIX gives them had never been written down.
#
#   <iconv.h> did not exist AT ALL.  NetSurf includes it unconditionally
#   - utils/utf8.c - because reading a document whose bytes are in a
#   charset the document itself names is most of what a browser does.
#   So this libc has an iconv now: 26 single-byte charsets from a
#   generated table plus the UTF family, graded against the host's by
#   tools/iconv-test.sh over 2.58 million conversions.
#
#   <setjmp.h> compiles only in the right search order.  This is the
#   sharpest one and it is a fact about this project's own sysroot, not
#   about a missing function. system_api's <signal.h> and this libc's
#   <signal.h> have the SAME NAME; the libc one is found first and
#   reaches the other with `#include_next`. curl's configure adds
#   `-isystem <sysroot>/usr/include` when told where mbedtls is, which
#   puts system_api's copy in front - so a plain `#include <signal.h>`
#   stopped reaching the libc's, sigset_t vanished, and <setjmp.h>
#   failed to compile. Any third-party build that names the sysroot's
#   include directory hits this. Worked around below by pinning
#   usr/local/include ahead of it on the compiler line, and recorded in
#   milestones.md as a fragility rather than treated as curl's fault.
#
# ---- host tools -------------------------------------------------------
#
# Beyond docs/toolchain.md's list this needs `bison` 3.x (macOS ships
# 2.3, from 2006, which cannot parse libnslog's grammar) and the host's
# libpng (NetSurf builds a host tool that turns its toolbar PNGs into C
# arrays). Both are dev-time only and neither goes near the image.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
if [ ! -x "$PREFIX/bin/x86_64-lean_os-gcc" ]; then
  echo "build-netsurf: no x86_64-lean_os-gcc - run tools/build-toolchain.sh" >&2
  exit 1
fi

# bison 3.x, wherever it is. Homebrew keeps it keg-only because macOS
# ships its own, so the Cellar path is tried before PATH's.
BISON_DIR=""
for d in /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin; do
  [ -x "$d/bison" ] && BISON_DIR="$d" && break
done
export PATH="${BISON_DIR:+$BISON_DIR:}$PREFIX/bin:$PATH"
BISON_VER=$(bison --version 2>/dev/null | head -1 | grep -oE '[0-9]+' | head -1)
if [ -z "$BISON_VER" ] || [ "$BISON_VER" -lt 3 ]; then
  echo "build-netsurf: need bison 3.x - libnslog's grammar uses" >&2
  echo "               '%destructor { } <tag>', which bison 2.3 (what" >&2
  echo "               macOS ships) cannot parse. 'brew install bison'." >&2
  exit 1
fi

# The host's libpng, for NetSurf's convert_image build tool.
HOST_PNG_CFLAGS=""
HOST_PNG_LDFLAGS="-lpng"
for d in /opt/homebrew/opt/libpng /usr/local/opt/libpng; do
  if [ -f "$d/include/png.h" ]; then
    HOST_PNG_CFLAGS="-I$d/include"
    HOST_PNG_LDFLAGS="-L$d/lib -lpng"
    break
  fi
done

make -s -C "$ROOT" sysroot >/dev/null || exit 1

SRC="$ROOT/build/thirdparty-src"
STAGE="$ROOT/build/thirdparty-sysroot"
SYSROOT="$ROOT/build/sysroot"
OUT="$ROOT/build/netsurf"
mkdir -p "$SRC" "$STAGE" "$OUT"
cd "$SRC"

fetch() {
  [ -f "$2" ] && return 0
  echo "build-netsurf: fetching $2"
  curl -sSL -o "$2.part" "$1" && mv "$2.part" "$2"
}

NS_VER=3.11
CURL_VER=8.11.1

# ---- libcurl, because NetSurf 3.11 does not build without it ----------
#
# NETSURF_USE_CURL=NO removes the pkg-config flags and NOT the source:
# content/fetch.c includes content/fetchers/curl.h unconditionally and
# calls fetch_curl_register(). So a browser here needs a real libcurl,
# and it gets one over mbedtls - the TLS library M100's eighth increment
# already put in this sysroot - so `https://` reaches M66's TCP through
# two libraries nobody here wrote and one stack that was written here.
fetch https://curl.se/download/curl-$CURL_VER.tar.xz curl-$CURL_VER.tar.xz
if [ ! -f "$STAGE/usr/lib/libcurl.a" ]; then
  rm -rf "curl-$CURL_VER"
  tar xf "curl-$CURL_VER.tar.xz"
  python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
          "curl-$CURL_VER/config.sub" || exit 1
  (
    cd "curl-$CURL_VER" || exit 1
    # The -isystem is the setjmp.h fix described in this file's header:
    # it pins this libc's headers ahead of the ones configure adds for
    # mbedtls, so `#include <signal.h>` keeps reaching the right one.
    # In CC rather than CFLAGS because CFLAGS lands after configure's
    # own -isystem on the command line, and the order is the whole point.
    CC="x86_64-lean_os-gcc -isystem $SYSROOT/usr/local/include" \
    ./configure --host=x86_64-lean_os --prefix=/usr \
        --disable-shared --enable-static \
        --with-mbedtls --with-zlib --without-libpsl \
        --disable-ldap --disable-ldaps --disable-threaded-resolver \
        --disable-ipv6 --disable-manual \
        --without-brotli --without-zstd >configure.log 2>&1 || {
      echo "build-netsurf: curl's configure failed:" >&2
      tail -30 configure.log >&2
      exit 1
    }
    make -j4 >build.log 2>&1 || {
      echo "build-netsurf: curl did not build:" >&2
      grep -E "error:" build.log | head -20 >&2
      exit 1
    }
    make install DESTDIR="$STAGE" >install.log 2>&1 || exit 1
  ) || exit 1
  # No .la files - see tools/build-thirdparty.sh's stage_into_sysroot for
  # why a libtool archive staged under DESTDIR names files that do not
  # exist.
  rm -f "$STAGE"/usr/lib/*.la
  make -s -C "$ROOT" sysroot >/dev/null || exit 1
  echo "build-netsurf: libcurl $CURL_VER (mbedTLS) -> $STAGE/usr/lib/libcurl.a"
fi

# ---- NetSurf and its fourteen libraries -------------------------------
fetch https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-$NS_VER.tar.gz \
      netsurf-all-$NS_VER.tar.gz
NSDIR="$SRC/netsurf-all-$NS_VER"
if [ ! -d "$NSDIR" ]; then
  tar xf "netsurf-all-$NS_VER.tar.gz"
fi
python3 "$ROOT/tools/netsurf-port/apply.py" "$NSDIR" || exit 1

cd "$NSDIR"

# WITHOUT_ICONV_FILTER: libparserutils' own build option. Its charset
# handling then uses its built-in codecs (ASCII, the 8859s, UTF-8,
# UTF-16) rather than iconv, which is a supported upstream configuration
# and not a workaround - NetSurf's own utils/utf8.c is where this libc's
# iconv actually gets used.
#
# _GNU_SOURCE is the truthful answer rather than a lie: NetSurf's
# utils/config.h uses it to decide whether the system already has
# strcasestr and strndup, and this one has both - strcasestr in
# <strings.h>, which is where POSIX puts it. Without this NetSurf
# compiles its own strcasestr and the link fails on a duplicate symbol.
# M117: frame pointers, so an assertion or a crash in the browser leaves
# the chain of return addresses on the serial log (this libc's
# __assert_fail walks it) rather than one line. NetSurf on apple.com
# stopped on an assertion in a function with eleven callers and nothing
# on the machine could say which; the same build for the host rendered
# the page. One register the compiler keeps, for a program this size
# and this far from its authors.
export CFLAGS="-DWITHOUT_ICONV_FILTER -D_GNU_SOURCE -fno-omit-frame-pointer"

NSLIBS="libwapcaplet libnslog libparserutils libcss libhubbub libdom \
        libnsbmp libnsgif librosprite libnsutils libutf8proc libnspsl \
        libsvgtiny libnsfb"

# Installed into the sysroot staging tree with PREFIX=/usr, exactly as
# the other nine libraries in this stack are, so that
# x86_64-lean_os-pkg-config answers for all of them from one place. A
# header the next library cannot find is indistinguishable from a
# library that does not exist (M100's first increment).
for L in $NSLIBS; do
  make -C "$L" install HOST=x86_64-lean_os NSSHARED="$NSDIR/buildsystem" \
       PREFIX=/usr DESTDIR="$STAGE" Q=@ WARNFLAGS='-Wall -W -Wno-error' \
       PKG_CONFIG=x86_64-lean_os-pkg-config >"$OUT/$L.log" 2>&1 || {
    echo "build-netsurf: $L did not build:" >&2
    tail -20 "$OUT/$L.log" >&2
    exit 1
  }
done
echo "build-netsurf: 14 NetSurf libraries -> $STAGE/usr/lib"

# nsgenbind runs on THIS machine - it generates the JavaScript bindings
# from the WebIDL - so it is built for the host, with the host's cc.
make -C nsgenbind install NSSHARED="$NSDIR/buildsystem" \
     PREFIX="$NSDIR/inst-host" DESTDIR= Q=@ >"$OUT/nsgenbind.log" 2>&1 || {
  echo "build-netsurf: nsgenbind (host tool) did not build:" >&2
  tail -20 "$OUT/nsgenbind.log" >&2
  exit 1
}
export PATH="$NSDIR/inst-host/bin:$PATH"

# ---- the lean_os display surface --------------------------------------
#
# Compiled here and added to the installed libnsfb.a. See the file's own
# header for why this is a surface rather than a front end, and why that
# means NetSurf itself needed no edit. libnsfb's private headers come
# from the source tree because a surface is an internal thing - it
# touches nsfb_t's fields and calls select_plotters - and the install
# exports only the public API.
make -s -C "$ROOT" sysroot >/dev/null || exit 1
x86_64-lean_os-gcc -O2 -std=c99 -Wall -Wextra -Werror -c \
  -I "$NSDIR/libnsfb/include" -I "$NSDIR/libnsfb/src" \
  -I "$ROOT/system_api/include" -I "$ROOT/user_space/lib" \
  -o "$OUT/nsfb_leanos.o" "$ROOT/user_space/bin/nsfb_leanos.c" || {
  echo "build-netsurf: the lean_os surface did not compile" >&2
  exit 1
}
x86_64-lean_os-ar r "$STAGE/usr/lib/libnsfb.a" "$OUT/nsfb_leanos.o" || exit 1
make -s -C "$ROOT" sysroot >/dev/null || exit 1
echo "build-netsurf: the lean_os surface -> libnsfb.a"

# ---- the browser ------------------------------------------------------
#
# GCCSDK_INSTALL_CROSSBIN is NetSurf's own cross-compilation hook and
# the reason no edit was needed for the toolchain: frontends/framebuffer/
# Makefile.tools sets CC from it. The library builds above use a
# different mechanism (buildsystem/makefiles/Makefile.tools derives
# $(HOST)-gcc), which is why both are passed.
#
# PKG_CONFIG and PKGCONFIG are two different variables in two different
# makefiles for one idea, and both have to point at the sysroot's
# pkg-config or half the lookups answer for the host.
make -C netsurf TARGET=framebuffer HOST=x86_64-lean_os PREFIX=/usr \
     NSSHARED="$NSDIR/buildsystem" Q=@ WARNFLAGS='-Wall -W -Wno-error' \
     GCCSDK_INSTALL_CROSSBIN="$PREFIX/bin" \
     PKG_CONFIG=x86_64-lean_os-pkg-config \
     PKGCONFIG=x86_64-lean_os-pkg-config \
     BUILD_LIBPNG_CFLAGS="$HOST_PNG_CFLAGS" \
     BUILD_LIBPNG_LDFLAGS="$HOST_PNG_LDFLAGS" \
     NETSURF_USE_CURL=YES NETSURF_USE_OPENSSL=NO \
     NETSURF_USE_WEBP=NO NETSURF_USE_JPEGXL=NO NETSURF_USE_HARU_PDF=NO \
     NETSURF_USE_LIBICONV_PLUG=YES NETSURF_FB_FONTLIB=freetype \
     >"$OUT/netsurf.log" 2>&1 || {
  echo "build-netsurf: NetSurf did not build:" >&2
  grep -E "error:|undefined reference" "$OUT/netsurf.log" | head -20 >&2
  exit 1
}

# ---- which surface a bare `netsurf` picks, asserted rather than hoped -
#
# The framebuffer front end chooses a default surface like this
# (frontends/framebuffer/gui.c):
#
#     static enum nsfb_type_e fetype = NSFB_SURFACE_COUNT;
#     ... if (type < fetype) { fename = name; }
#
# `fetype` is never assigned, so the condition is true for every
# registered surface and the LAST one to register wins. Registration is
# a constructor, so "last" means last in .init_array, which means last
# in link order - and this project's surface is last only because its
# object is appended to libnsfb.a above.
#
# That is a real thing to depend on and it is invisible when it breaks:
# the browser would come up on the `ram` surface, draw a whole page
# into memory nobody displays, and show an empty window with no error
# anywhere. So it is checked here, in the linked binary, and a build
# that would have done that fails instead.
#
# Not fixed by editing gui.c: the bug is upstream's and reporting it
# there is the right channel, while an edit here would be a fork of
# somebody else's program to work around a line this check can simply
# watch.
python3 - "$PREFIX/bin/x86_64-lean_os-nm" \
         "$PREFIX/bin/x86_64-lean_os-readelf" netsurf/nsfb <<'PYCHECK' || exit 1
import re, subprocess, sys
nm, readelf, binary = sys.argv[1], sys.argv[2], sys.argv[3]

syms = {}
for line in subprocess.run([nm, binary], capture_output=True, text=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[2].endswith("_register_surface"):
        syms[int(parts[0], 16)] = parts[2]

words = []
for line in subprocess.run([readelf, "-x", ".init_array", binary],
                           capture_output=True, text=True).stdout.splitlines():
    m = re.match(r"\s+0x[0-9a-f]+((?:\s[0-9a-f]{8})+)", line)
    if m:
        for i, w in enumerate(m.group(1).split()):
            words.append(w)
# `readelf -x` dumps RAW BYTES grouped in fours, not 32-bit words - so
# the eight bytes of a pointer are in file order and the value is the
# little-endian read of them. Getting this wrong is silent: it produces
# plausible 64-bit numbers that match no symbol, and the check then
# reports "no surface constructors found" rather than a parse error.
ptrs = [int.from_bytes(bytes.fromhex(words[i] + words[i + 1]), "little")
        for i in range(0, len(words) - 1, 2)]
registered = [syms[p] for p in ptrs if p in syms]
if not registered:
    print("build-netsurf: no surface constructors found in .init_array - the",
          file=sys.stderr)
    print("               check itself has stopped working, which is worse",
          file=sys.stderr)
    print("               than the thing it checks. Look at it.", file=sys.stderr)
    sys.exit(1)
if registered[-1] != "leanos_register_surface":
    print("build-netsurf: the last surface to register is %s, not this" %
          registered[-1], file=sys.stderr)
    print("               project's. A bare `netsurf` would come up on it and", file=sys.stderr)
    print("               draw into a buffer nothing displays. Order in", file=sys.stderr)
    print("               .init_array: %s" % ", ".join(registered), file=sys.stderr)
    sys.exit(1)
print("build-netsurf: surface constructors in order: %s - a bare `netsurf`"
      % ", ".join(s.replace("_register_surface", "") for s in registered))
print("               picks leanos")
PYCHECK

cp netsurf/nsfb "$OUT/netsurf"

# The resources it reads at run time: the default stylesheet (which is
# most of what "renders like a browser" means), the messages catalogue,
# the about: pages, and the fonts.
RES="$OUT/res"
rm -rf "$RES"
mkdir -p "$RES"
for f in adblock.css credits.html default.css internal.css licence.html \
         netsurf.png quirks.css welcome.html favicon.png; do
  cp "netsurf/frontends/framebuffer/res/$f" "$RES/" || exit 1
done
cp netsurf/frontends/framebuffer/res/en/Messages "$RES/Messages" || exit 1

# ---- Choices: the options NetSurf reads at startup --------------------
#
# The framebuffer front end parses its OWN command line with getopt
# (-f/-b/-w/-h) and rejects anything else before nsoption_commandline
# ever sees it, so `--enable_javascript=1` on the command line is an
# error rather than an option. The supported way to set an option is
# this file, found on NETSURF_FB_RESPATH - which is why it is built
# here rather than typed by whoever runs the browser.
#
# JavaScript is ON. That is a decision worth stating: it is the whole
# reason Duktape is in the binary, it is what makes this a browser
# rather than a document viewer, and the capability model is what makes
# it defensible - a page's script runs inside a process holding
# CAP_FS_WRITE and CAP_NETWORK and nothing else (system_api/include/
# caps.h). It cannot paint the screen, read the clipboard, list
# processes or switch the machine off.
cat > "$RES/Choices" <<'CHOICES'
# Choices - NetSurf's options on lean_os. Written by
# tools/build-netsurf.sh; see its header for why this file exists at all
# rather than being command-line flags.
enable_javascript:1
# The local welcome page, so a browser started with no argument shows
# something on a machine whose network may not be up yet.
homepage_url:file:///usr/share/netsurf/welcome.html
# 12.8pt. NetSurf stores font size in tenths of a point.
font_size:128
# The window. NetSurf's own window_width/window_height options override
# whatever the surface reports as its default (see leanos_defaults in
# user_space/bin/nsfb_leanos.c, whose 900x640 is therefore only what a
# build with no Choices file would get). Sized to leave the taskbar and
# the icon column visible on this machine's 1024x768 default.
window_width:800
window_height:600
# Anti-aliased text. The compositor's window is 32bpp and freetype's
# grey rendering is what M100's [m100c] self-test already grades.
fb_font_monochrome:0
# ---- https, and where the certificate authorities come from ----------
# CURLOPT_CAINFO, pointed at a PACKAGE rather than at a file in the
# image, and that is the decision rather than an accident.
#
# This OS still ships no certificate authorities: a fresh image has
# nothing at this path and https fails cert verification, which is
# M65's rule (docs/browser.md). What M114 changed is that there is now
# a supported way to add them that is not "rebuild the OS" -
#
#     os install ca-certificates
#
# - which was the exact condition M100 recorded for reopening this, and
# M111's package manager is what met it. A bundle has to be updatable on
# its own, because an authority is removed from one when it has done
# something wrong.
#
# The version is in the path because /pkg/<name>/<version>/ is where an
# installed package lives (docs/packages.md). It is pinned here and in
# tools/build-packages.sh, and those two are the only places it appears.
ca_bundle:/pkg/ca-certificates/1.0/share/ca-certificates/ca-bundle.pem
CHOICES

DEJAVU=$(echo "$SRC"/dejavu-fonts-ttf-*/ttf/DejaVuSans.ttf | head -1)
if [ ! -f "$DEJAVU" ]; then
  echo "build-netsurf: no DejaVu fonts - run tools/build-thirdparty.sh" >&2
  echo "               first; it downloads them for [m100c]." >&2
  exit 1
fi
mkdir -p "$RES/fonts"
cp "$(dirname "$DEJAVU")"/DejaVuSans.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSans-Bold.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSans-Oblique.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSans-BoldOblique.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSerif.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSerif-Bold.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSansMono.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSansMono-Bold.ttf "$RES/fonts/" || exit 1

SZ=$(( $(wc -c < "$OUT/netsurf") / 1024 ))
echo "build-netsurf: /bin/netsurf -> $OUT/netsurf (${SZ}K)"
echo "build-netsurf: resources    -> $RES"
echo "build-netsurf: now run tools/install-netsurf.sh to put it on the disk"
