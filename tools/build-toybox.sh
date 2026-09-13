#!/usr/bin/env bash
# tools/build-toybox.sh - M89: build toybox for lean_os.
#
# ---- why this script exists rather than a Makefile rule ---------------
#
# Toybox has its own build system, and the whole point of porting it is
# that this project does not get to replace that. So this drives toybox's
# `make` the way any packager would, and everything lean_os-specific is
# in the arguments.
#
# ---- the patch series, and why third_party/ is not edited ------------
#
# Toybox supports a closed set of operating systems. lib/portability.c
# has three sites that read
#
#   #if defined(__linux__) ... #elif defined(__APPLE__) ...
#   #elif defined(__FreeBSD__)... #else #error #endif
#
# so a system that is none of those does not fail to link - it fails to
# compile, on purpose, with the author telling you to come and add your
# case. That is the honest design for a portability layer and it means
# porting toybox is *editing toybox*, which CLAUDE.md's first
# non-negotiable forbids doing in third_party/.
#
# The way out is the one M94 already writes down for the GCC target port:
# "a target port is upstream-shaped configuration; a patch to the
# compiler's own passes is the thing M63's rule exists to forbid." Adding
# an OS to a portability layer is the upstream-shaped kind. So the tree
# in third_party/ stays byte-identical to the published tarball, this
# script copies it, and tools/toybox-port/*.patch is applied to the copy.
# The patches are ours, they are small, they are readable, and a diff
# against the tarball is still meaningful - which is the property that
# would be lost by editing in place.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SRC=third_party/toybox
WORK=build/toybox
PATCHES=tools/toybox-port

if [ ! -d "$SRC" ]; then
  echo "build-toybox: $SRC is missing" >&2
  exit 1
fi
if [ -z "$(command -v gsed 2>/dev/null)" ]; then
  # Toybox's own scripts need GNU sed and look for it under this name;
  # macOS's is BSD sed and fails at the first code-generation step. See
  # docs/toolchain.md.
  echo "build-toybox: GNU sed (gsed) is required - brew install gnu-sed" >&2
  exit 1
fi

# ---- the second host tool, and why it is a shim rather than a package -
#
# scripts/make.sh builds the compressed help text with
#
#   instlist --help | gzip -9 | od -Anone -vtx1 | sed 's/ /,0x/g;...'
#
# which turns each space between two hex bytes into ",0x". GNU od emits
# exactly one space before each byte and nothing else; BSD od (which is
# what macOS has) indents the line and pads it out to a fixed width with
# trailing spaces, so the same sed produces ",0x,0x,0x1f" and the
# compiler stops at "invalid suffix 'x' on integer constant".
#
# The gsed requirement above is the same class of problem and was solved
# by asking for the GNU tool. This one is not, deliberately: coreutils is
# a second host package to require for one command in one build step, and
# the normalization is three substitutions. So a shim goes first on PATH
# for the duration of this build, and third_party/ and the patch series
# both stay out of it - this is a difference between two `od`s on the
# machine doing the building, not anything about lean_os.
SHIM=$(cd "$(dirname "$0")/.." && pwd)/build/toybox-hostbin
rm -rf "$SHIM"
mkdir -p "$SHIM"
cat > "$SHIM/od" <<'SHIMEOF'
#!/bin/sh
# GNU-shaped `od` output, for toybox's zhelp generation - see
# tools/build-toybox.sh for why this exists.
/usr/bin/od "$@" | sed -e 's/[[:space:]]*$//'                        -e 's/^[[:space:]][[:space:]]*/ /'                        -e 's/[[:space:]][[:space:]]*/ /g'
SHIMEOF
chmod +x "$SHIM/od"
PATH="$SHIM:$PATH"
export PATH

echo "build-toybox: unpacking a clean copy of $SRC"
rm -rf "$WORK"
cp -R "$SRC" "$WORK"

for p in "$PATCHES"/*.patch; do
  [ -e "$p" ] || continue
  echo "build-toybox: applying $(basename "$p")"
  # -p0 with the paths as generated, from the tree root, and --forward so
  # a re-run on an already-patched copy is an error rather than a mess.
  if ! patch -p0 --forward --silent < "$p"; then
    echo "build-toybox: $p did not apply - the vendored toybox may have moved" >&2
    exit 1
  fi
done

# The flags lean_os builds everything with, plus the define the port
# above keys on. -ffreestanding because this libc is not the host's, and
# the three include paths are the same three every program in
# user_space/bin compiles against.
# ---- the compile flags, and the two that are not optional -------------
#
# The three include paths are the same three every program in
# user_space/bin compiles against, and -ffreestanding is because this
# libc is not the host's. The two that matter and are easy to leave out:
#
#   -mcmodel=large  because a lean_os program is linked high (see
#                   user_space/lib/user.ld and USER_IMAGE_BASE), and the
#                   small model's 32-bit relocations do not reach. Left
#                   out, the compile succeeds and the LINK fails with a
#                   page of "relocation truncated to fit", which is a
#                   confusing way to be told about a code model.
#   -fno-pic        because there is no dynamic loader on this machine
#                   (M95 is where that changes) and a GOT nothing fills
#                   in is a null dereference at the first global.
#
# -D__lean_os__ is what the port patch keys on.
LEANOS_CFLAGS="-ffreestanding -fno-stack-protector -fno-pic -mcmodel=large \
  -mno-red-zone -D__lean_os__ \
  -I$ROOT/user_space/libc/include -I$ROOT/user_space/lib -I$ROOT/system_api/include"

cd "$WORK"
echo "build-toybox: configuring"
make defconfig >/dev/null 2>&1 || { echo "build-toybox: defconfig failed" >&2; exit 1; }

# ---- the commands lean_os does not have a kernel for -------------------
#
# This is the other half of the port, and it is deliberately a *config*
# rather than a set of stubs. Toybox's build system exists to select
# which commands are built; using it to leave out the ones whose kernel
# interface does not exist here is what a packager does on any system,
# and it is the difference between "lean_os has no mount(2)" - true, and
# visible - and a `mount` on the disk that fails at run time for a reason
# the person typing it cannot see.
#
# **Listed by source file, not by config symbol, and that matters.** One
# .c file often declares several commands (toys/other/taskset.c declares
# both `taskset` and `nproc`), and turning off one of them still compiles
# the file. So each line below names a file, every `config` symbol in it
# is turned off, and the reason is written next to it - the reason being
# the fact, and the command list following from it.
LEANOS_SKIP="
  toys/lsb/mount.c        # no mount(2): the mount table is fixed by the kernel (M87)
  toys/lsb/umount.c
  toys/lsb/su.c           # no second principal to become, and no crypt(3) (M65)
  toys/lsb/dmesg.c:KEEP   # kept - klogctl() is real here, over M70's kernel log
  toys/other/swapon.c     # no swap: deferred with a condition since M82
  toys/other/swapoff.c
  toys/other/mkswap.c
  toys/other/losetup.c    # no loop devices, and no block-device namespace
  toys/other/chroot.c     # a root a process cannot escape is a boundary this
                          # machine expresses as capabilities, not as a path
  toys/other/insmod.c     # this kernel is not modular
  toys/other/rmmod.c
  toys/other/lsmod.c
  toys/other/modinfo.c
  toys/pending/modprobe.c
  toys/other/switch_root.c # mount(2) again
  toys/other/blkid.c      # ioctls on a block device nothing here exposes
  toys/other/blockdev.c
  toys/other/blkdiscard.c
  toys/other/fsfreeze.c
  toys/other/mountpoint.c # asks via st_dev, which is 0 for everything here
  toys/net/ifconfig.c     # SIOCGIFCONF: this stack is configured through
                          # SYS_netconf, and netconf is the program that does it
  toys/net/netstat.c
  toys/net/host.c         # raw DNS through <resolv.h>; M73's client is a
                          # resolver, not a res_query() surface
  toys/net/ping.c         # raw sockets, which this stack does not have
  toys/net/sntp.c         # adjtimex(2); nettime is the lean_os client
  toys/net/rfkill.c       # <linux/rfkill.h>
  toys/net/tunctl.c       # <linux/if_tun.h>
  toys/other/vmstat.c     # /proc/stat, which this /proc does not have
  toys/other/hwclock.c    # /dev/rtc: the RTC is read at boot and is not a file
  toys/other/chrt.c       # SCHED_FIFO: this scheduler has two classes decided
                          # by behaviour (M69) and no policy a process can set
  toys/posix/nice.c       # nice(2)/setpriority(2): same - there is no
                          # priority a process can ask for here
  toys/posix/renice.c
  toys/other/taskset.c    # sched_setaffinity - and nproc goes with it,
                          # which is the cost of the file-level rule above
  toys/other/uclampset.c
  toys/other/ionice.c     # ioprio_get/set
  toys/other/devmem.c     # /dev/mem, which devfs deliberately does not have
  toys/other/fallocate.c  # leanfs allocates on write; no way to reserve
  toys/other/flock.c      # advisory locks - see <fcntl.h> for the refusal
  toys/other/gpiod.c      # <linux/gpio.h>
  toys/other/i2ctools.c   # <linux/i2c.h>
  toys/other/mkpasswd.c   # crypt(3), and no password file for the result
  toys/other/lsattr.c     # ext2 inode flags through <linux/fs.h>
  toys/other/mix.c        # OSS mixer ioctls; M44 has SYS_audio_volume
  toys/other/memeater.c   # mlock(2): every page is resident and there is no swap
  toys/other/oneit.c      # an init; lean_os has its own (user_space/init)
  toys/other/openvt.c     # <linux/vt.h>: the console here is one framebuffer
  toys/other/partprobe.c  # BLKRRPART
  toys/other/nsenter.c    # namespaces, which this kernel does not have
  toys/other/readahead.c  # readahead(2) - M104 is where a cache hint lands
  toys/other/rtcwake.c    # <linux/rtc.h>, and no wake-from-off
  toys/other/vconfig.c    # <linux/if_vlan.h>
  toys/other/nbd_client.c # a block device over the network
  toys/other/nbd_server.c
  toys/other/inotifyd.c   # no file-change notification of any kind
  toys/other/pivot_root.c # mount(2)
  toys/other/freeramdisk.c # a device this machine does not enumerate
  toys/other/eject.c
  toys/other/watchdog.c   # /dev/watchdog
  toys/other/login.c      # one principal, and no crypt(3)
  toys/other/sysctl.c     # /proc/sys, which this /proc does not have
  toys/other/lsusb.c      # no USB - M107 is where that changes
  toys/other/chcon.c      # SELinux
  toys/other/setfattr.c   # extended attributes, which leanfs cannot hold
  toys/other/linux32.c    # personality(2)
  toys/posix/iconv.c      # character-set conversion tables: this system is
                          # UTF-8 and has no other encoding to convert to
  toys/other/w.c          # utmpx: nothing here records a login, because
                          # nothing here logs in (M65)
  toys/posix/who.c        # same
  toys/posix/ulimit.c     # prlimit(2): getrlimit/setrlimit here report and
                          # refuse (M88), and there is no per-process limit
                          # to set on another process at all
  toys/other/pmap.c       # /proc/N/maps, which this /proc does not have
  toys/other/pwdx.c       # /proc/N/cwd, same
  toys/other/acpi.c       # /sys/class/power_supply: no battery, no /sys
"
for f in $LEANOS_SKIP; do
  case "$f" in
    *.c) ;;
    *) continue ;;   # a reason, not a file
  esac
  [ -f "$f" ] || { echo "build-toybox: $f is not in this toybox" >&2; exit 1; }
  for sym in $(gsed -n 's/^config \([A-Z_0-9]*\)$/\1/p' "$f"); do
    gsed -i "s/^CONFIG_$sym=y\$/# CONFIG_$sym is not set/" .config
  done
done
yes "" 2>/dev/null | make oldconfig >/dev/null 2>&1 || true

echo "build-toybox: building"
# ---- the link line, and why it has to be set here ---------------------
#
# scripts/portability.sh picks the linker flags from `uname`, so a build
# hosted on a Mac asks for -Wl,-dead_strip and `strip` with no arguments
# - which are the Apple linker's spellings, and this link is done by
# x86_64-elf-ld. Setting LDOPTIMIZE and STRIP from outside is the
# supported way to override that (they are `: ${VAR:=default}` in that
# script), so nothing needs patching: what is wrong is the assumption
# that the host and the target agree, and naming the target's flags is
# the answer.
export LDOPTIMIZE="-Wl,--gc-sections -Wl,--as-needed"
# `strip` and not `x86_64-elf-strip`: make.sh prepends $CROSS_COMPILE.
export STRIP="strip"

# -static and the lean_os startup files. Toybox's build produces one
# binary; this is where it is told that the binary is for a machine with
# no dynamic loader (M95 is where that changes) and that the runtime it
# links against is this tree's, not the cross-toolchain's.
#
# LDFLAGS lands AFTER the object files on toybox's link line (see
# scripts/make.sh's $LINK), which is exactly where this tree's runtime
# has to go: crt0 first, then the same USER_LIBOBJS every lean_os program
# links against, under the same linker script. Read out of the Makefile
# rather than restated here, so this cannot drift from what `make` does -
# the same trick tools/build-user-program.sh already uses.
USER_LIBOBJS=$(make -s -C "$ROOT" print-USER_LIBOBJS)
LIBOBJS_ABS=""
for o in $USER_LIBOBJS; do
  LIBOBJS_ABS="$LIBOBJS_ABS $ROOT/$o"
done
if [ -z "$USER_LIBOBJS" ] || [ ! -f "$ROOT/build/user_obj/crt0.o" ]; then
  echo "build-toybox: run make first - this links against build/user_obj" >&2
  exit 1
fi
export LDFLAGS="-static -nostdlib -Wl,-T,$ROOT/user_space/lib/user.ld $LIBOBJS_ABS"

make CROSS_COMPILE=x86_64-elf- CC=gcc HOSTCC=cc CFLAGS="$LEANOS_CFLAGS" "$@"
