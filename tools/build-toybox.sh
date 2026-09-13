#!/usr/bin/env bash
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
  echo "build-toybox: GNU sed (gsed) is required - brew install gnu-sed" >&2
  exit 1
fi

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
  if ! patch -p0 --forward --silent < "$p"; then
    echo "build-toybox: $p did not apply - the vendored toybox may have moved" >&2
    exit 1
  fi
done

LEANOS_CFLAGS="-ffreestanding -fno-stack-protector -fno-pic -mcmodel=large \
  -mno-red-zone -D__lean_os__ \
  -I$ROOT/user_space/libc/include -I$ROOT/user_space/library -I$ROOT/system_api/include"

cd "$WORK"
echo "build-toybox: configuring"
make defconfig >/dev/null 2>&1 || { echo "build-toybox: defconfig failed" >&2; exit 1; }

LEANOS_SKIP="
  toys/lsb/mount.c
  toys/lsb/umount.c
  toys/lsb/su.c
  toys/lsb/dmesg.c:KEEP
  toys/other/swapon.c
  toys/other/swapoff.c
  toys/other/mkswap.c
  toys/other/losetup.c
  toys/other/chroot.c
  toys/other/insmod.c
  toys/other/rmmod.c
  toys/other/lsmod.c
  toys/other/modinfo.c
  toys/pending/modprobe.c
  toys/other/switch_root.c
  toys/other/blkid.c
  toys/other/blockdev.c
  toys/other/blkdiscard.c
  toys/other/fsfreeze.c
  toys/other/mountpoint.c
  toys/net/ifconfig.c
  toys/net/netstat.c
  toys/net/host.c
  toys/net/ping.c
  toys/net/sntp.c
  toys/net/rfkill.c
  toys/net/tunctl.c
  toys/other/vmstat.c
  toys/other/hwclock.c
  toys/other/chrt.c
  toys/posix/nice.c
  toys/posix/renice.c
  toys/other/taskset.c
  toys/other/uclampset.c
  toys/other/ionice.c
  toys/other/devmem.c
  toys/other/fallocate.c
  toys/other/flock.c
  toys/other/gpiod.c
  toys/other/i2ctools.c
  toys/other/mkpasswd.c
  toys/other/lsattr.c
  toys/other/mix.c
  toys/other/memeater.c
  toys/other/oneit.c
  toys/other/openvt.c
  toys/other/partprobe.c
  toys/other/nsenter.c
  toys/other/readahead.c
  toys/other/rtcwake.c
  toys/other/vconfig.c
  toys/other/nbd_client.c
  toys/other/nbd_server.c
  toys/other/inotifyd.c
  toys/other/pivot_root.c
  toys/other/freeramdisk.c
  toys/other/eject.c
  toys/other/watchdog.c
  toys/other/login.c
  toys/other/sysctl.c
  toys/other/lsusb.c
  toys/other/chcon.c
  toys/other/setfattr.c
  toys/other/linux32.c
  toys/posix/iconv.c
  toys/other/w.c
  toys/posix/who.c
  toys/posix/ulimit.c
  toys/other/pmap.c
  toys/other/pwdx.c
  toys/other/acpi.c
"
for f in $LEANOS_SKIP; do
  case "$f" in
    *.c) ;;
    *) continue ;;
  esac
  [ -f "$f" ] || { echo "build-toybox: $f is not in this toybox" >&2; exit 1; }
  for sym in $(gsed -n 's/^config \([A-Z_0-9]*\)$/\1/p' "$f"); do
    gsed -i "s/^CONFIG_$sym=y\$/# CONFIG_$sym is not set/" .config
  done
done
yes "" 2>/dev/null | make oldconfig >/dev/null 2>&1 || true

echo "build-toybox: building"
export LDOPTIMIZE="-Wl,--gc-sections -Wl,--as-needed"
export STRIP="strip"

USER_LIBOBJS=$(make -s -C "$ROOT" print-USER_LIBOBJS)
LIBOBJS_ABS=""
for o in $USER_LIBOBJS; do
  LIBOBJS_ABS="$LIBOBJS_ABS $ROOT/$o"
done
if [ -z "$USER_LIBOBJS" ] || [ ! -f "$ROOT/build/user_obj/crt0.o" ]; then
  echo "build-toybox: run make first - this links against build/user_obj" >&2
  exit 1
fi
export LDFLAGS="-static -nostdlib -Wl,-T,$ROOT/user_space/library/user.ld $LIBOBJS_ABS"

make CROSS_COMPILE=x86_64-elf- CC=gcc HOSTCC=cc CFLAGS="$LEANOS_CFLAGS" "$@"
