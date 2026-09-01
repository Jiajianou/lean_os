/* kernel/dev/fwcfg.h - Q1
 *
 * QEMU's firmware configuration device, and the one question this kernel
 * currently asks it: should this boot run its self-tests?
 *
 * ---- Why this device, and not a compile-time flag -------------------
 *
 * For ninety-three milestones every boot ran every self-test, because
 * there was no way to say otherwise and because the tests were cheap.
 * They stopped being cheap around M52: most of them now spawn a real
 * process and wait for it, and the boot takes ~140 s of which the
 * overwhelming majority is a test waiting on the scheduler. That is the
 * right price for `tools/run-tests.sh` and the wrong one for
 * `tools/run-qemu.sh`, which is somebody trying to use the machine.
 *
 * The obvious fix - two builds, one with the tests compiled out - was
 * rejected on the grounds that it ships a binary nothing tested. The
 * artifact that boots on a user's disk should be, byte for byte, the
 * artifact the harness graded. So the switch is at *boot* time and it
 * comes from outside the image.
 *
 * fw_cfg is how a QEMU guest is told things by whoever started it. It is
 * a real paravirtual interface (two I/O ports, a directory of named
 * blobs), it survives ExitBootServices because it is raw port I/O rather
 * than a firmware service, and - the property that decides it - it is
 * *absent* on real hardware. A machine booted from a USB stick reads no
 * signature at port 0x510, fwcfg_present() returns 0, and self-tests are
 * off. That is the correct default for the one case where a wrong answer
 * costs somebody a reboot.
 *
 * ---- The interface ---------------------------------------------------
 *
 * Port 0x510 selects an item (16-bit write, little-endian on x86). Port
 * 0x511 streams that item's bytes (8-bit reads). Item 0x0000 is the
 * signature "QEMU" and is how presence is decided. Item 0x0019 is a
 * directory: a big-endian count followed by that many 64-byte entries of
 * {be32 size, be16 selector, be16 reserved, char name[56]}.
 *
 * Note the endianness split, which is the only genuinely surprising part
 * of the device: the *selector port* takes a little-endian value on x86,
 * while every integer *inside* the directory is big-endian. Both are in
 * QEMU's spec and neither is negotiable.
 */
#pragma once

#include <stdint.h>

/* Probes for the device. Safe to call on hardware that has no such
 * thing - it reads four bytes from an unclaimed port, gets 0xFF, and
 * concludes correctly. Must be called before fwcfg_read_file. */
void fwcfg_init(void);

/* Non-zero when a QEMU fw_cfg device answered the signature probe. */
int fwcfg_present(void);

/* Copies up to `max` bytes of the named blob into `dst` and returns the
 * number copied, or -1 if the device is absent or has no such file.
 * Names are the ones QEMU was started with, e.g. "opt/leanos/selftest".
 *
 * A blob larger than `max` is truncated rather than refused: every
 * caller here wants a short string and a caller that wanted the whole
 * thing would have to know the size in advance anyway. */
int fwcfg_read_file(const char *name, void *dst, uint32_t max);

/* ---- The one question, and its default ------------------------------
 *
 * True when `-fw_cfg name=opt/leanos/selftest,string=1` was passed. Any
 * other value, a missing file, or a machine with no fw_cfg at all means
 * false - see the header comment on why "off" is the right default for
 * the ambiguous cases rather than "on". */
int boot_selftests_enabled(void);
