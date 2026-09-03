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

/* ---- M103: the I/O APIC, and a default chosen by measurement ----------
 *
 * True when `-fw_cfg name=opt/leanos/ioapic,string=1` was passed.
 *
 * **The default is the 8259, and that is a measurement rather than a
 * preference.** With every legacy line routed through the I/O APIC and
 * acknowledged at the local APIC, this machine runs at roughly HALF
 * speed under QEMU:
 *
 *     TSC calibration   999 cycles/us (PIC)   494 cycles/us (I/O APIC)
 *     1 MiB from disk   4953 us               10062 us
 *     boot to desktop   228 s                 302 s
 *
 * and `syscall_null_cycles` did not move (990 -> 1040), which is what
 * says the guest's own instruction stream is unchanged: the cost is in
 * QEMU's emulation of an enabled APIC, not in lean_os. On real hardware
 * the trade is the other way round entirely - an I/O APIC is how a
 * modern machine delivers interrupts at all, and MSI (M107, M108) has no
 * other route.
 *
 * So both are supported paths on exactly the terms CLAUDE.md already
 * sets for `QEMU_DISK=ide`, and the one that costs nothing on the only
 * machine this project can currently run on is the default. M110 is the
 * milestone that reverses it, and it will reverse it with a number.
 *
 * The switch comes from OUTSIDE the image rather than from a build flag,
 * for the same reason the self-test switch does: the image is
 * byte-identical either way, so "the tested image is the shipped image"
 * stays true. */
int boot_ioapic_enabled(void);

/* ---- M98: the build this machine is asked to run for itself ----------
 *
 * True when `-fw_cfg name=opt/leanos/bootstrap,string=1` was passed.
 *
 * A third switch rather than a fourth self-test, because what it turns
 * on is not a self-test: it is a *measurement*, it takes minutes rather
 * than seconds, and its output is numbers rather than a pass. M98's
 * fourth box asks for the peak resident set of the largest translation
 * unit, the disk a build tree costs and the wall-clock of a bootstrap,
 * all taken on the machine; tools/bootstrap-test.sh is what asks, and
 * this is the question it asks with.
 *
 * Independent of the self-test switch on purpose. The battery costs
 * ~335 s of scheduler-bound waiting that has nothing to do with a build,
 * and a measurement that ran after it would be timing a machine that had
 * just spawned two hundred processes. Passing both is allowed and is
 * what a full run does; passing only this one is the measurement's own
 * boot. */
int boot_bootstrap_enabled(void);
