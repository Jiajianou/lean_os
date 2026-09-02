/* user_space/libc/include/sys/reboot.h - M89
 *
 * Stopping the machine, in the spelling a ported `reboot` or `poweroff`
 * uses.
 *
 * **Real, over SYS_shutdown.** M47 built an orderly stop - SIGTERM to
 * every task, a bounded grace period, SIGKILL to whatever is left, flush
 * the filesystem, then the tiered ACPI/8042/triple-fault machinery - and
 * this is that call under its POSIX-ish name. A program calling
 * reboot(RB_POWER_OFF) here gets the same shutdown the desktop's own
 * menu item performs.
 *
 * The magic numbers are Linux's, so a program that has them baked into
 * an object file means what it says. Two of them have no counterpart
 * here and are refused rather than approximated:
 *
 *   RB_HALT_SYSTEM - "stop the CPU and leave the power on". This machine
 *     has POWER_OFF and POWER_REBOOT and nothing between them; halting
 *     without powering off would leave a machine that looks hung.
 *   RB_SW_SUSPEND / RB_KEXEC - suspend-to-disk and booting a second
 *     kernel. Neither exists.
 *
 * `reboot` does not return on success, like the syscall under it.
 */
#pragma once

#define RB_AUTOBOOT     0x01234567
#define RB_HALT_SYSTEM  0xcdef0123
#define RB_ENABLE_CAD   0x89abcdef
#define RB_DISABLE_CAD  0x00000000
#define RB_POWER_OFF    0x4321fedc
#define RB_SW_SUSPEND   0xd000fce2
#define RB_KEXEC        0x45584543

int reboot(int cmd);
