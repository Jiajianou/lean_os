# Booting lean_os on real hardware

Every other milestone in this project is verified against QEMU/OVMF -
that's a controlled, scriptable environment `tools/qemu-serial-test.sh`
can boot headlessly and grade automatically. This one can't be: it needs
a physical USB drive and a physical machine to plug it into, so verifying
it means actually doing that, not running a script. This doc is the
runbook for that manual step - what to do, what to expect, and what's
worth knowing going in so a real quirk doesn't look like a bug in this
OS.

## What's already true, and why this has a real chance of working

`kernel/boot/uefi/boot.c` was written from the start to distinguish "the
disk this app was loaded from" from "the first disk QEMU happens to
enumerate" - it walks its own UEFI device path back to the physical disk
containing the ESP it booted from, rather than guessing. QEMU only ever
gave this kernel one disk to be confused between, so that distinction
was untested until now: it's been verified here by attaching a second,
decoy disk in QEMU and confirming the kernel still loads correctly from
the real one, not whichever handle the firmware happened to enumerate
first.

The disk image itself (`build/os-image.bin`) is already exactly what a
real machine's firmware expects for USB boot: `kernel/boot/mbr.asm`
writes a legacy MBR partition table with one entry of type `0xEF` (EFI
System Partition), which is the standard way non-GPT removable media
tells UEFI firmware where to find its ESP - and that ESP contains
`EFI/BOOT/BOOTX64.EFI`, the path UEFI's boot manager tries by default for
any removable device with no explicit boot entry of its own. This is the
same mechanism `tools/run-qemu.sh` already relies on via OVMF; real
firmware implements the same spec.

## Step 1: build

```sh
make clean && make all
```

`build/os-image.bin` is the artifact everything below writes to a drive.

## Step 2: write it to a USB drive

```sh
tools/write-usb.sh /dev/diskN     # macOS - find N via `diskutil list`
tools/write-usb.sh /dev/sdX       # Linux - find X via `lsblk`
```

**This destroys every byte currently on that drive with no undo** - the
script refuses obviously-wrong targets (the machine's own system disk,
non-removable media) and asks for a typed confirmation of the exact
device path before writing, but the only real safeguard is picking the
right device in the first place. Use a USB drive with nothing on it you
need.

## Step 3: firmware settings on the target machine

- **Secure Boot: off.** `BOOTX64.EFI` isn't signed - no third-party boot
  code of any kind is linked into it (see milestones.md's ground rules),
  which also means there's nothing to sign it with.
- **Boot mode: UEFI, not CSM/Legacy.** If the firmware's boot menu lists
  the USB drive twice - once as a plain device name, once prefixed
  `UEFI:` - pick the `UEFI:` entry specifically. The MBR's boot code area
  (`kernel/boot/mbr.asm`) is deliberately all zeroed - there's no legacy
  boot path here anymore (M26 removed it) for a Legacy/CSM entry to run.
- Get into the one-time boot menu (varies by vendor - Esc/F10/F12/F2 at
  power-on) rather than permanently reordering the boot priority, unless
  you want this to be the default.

## Step 4: what to expect

Boot output looks exactly like `tools/qemu-serial-test.sh`'s captured
log, just on the actual screen instead of a serial file - the same
`lean_os uefi: ...` firmware-stage lines, then the same GDT/IDT/E820/PMM/
VMM/heap/framebuffer/console milestone-by-milestone self-test log,
ending in the desktop shell if every self-test passes. If the machine has
a real COM1 serial port (or a USB-to-serial adapter wired to one),
`kernel/drivers/serial.c` talks to it exactly the way `-serial file:...`
does in `tools/qemu-serial-test.sh` - a terminal program on the other end
of that cable sees the identical log.

Two self-tests are written to degrade gracefully rather than panic when
their hardware isn't there, because unlike this kernel's PC-standard
assumptions (a PIT, PS/2 controllers, an ATA-addressable boot disk),
neither is something a real machine can be assumed to have:

- **Networking** (M27): almost no real hardware has the specific legacy
  RTL8139 chip this driver targets - `[net] no RTL8139 NIC found -
  networking untested this boot` is the *expected* line on real hardware,
  not a failure. If the target machine genuinely has an RTL8139-compatible
  PCI NIC (some old desktops, certain PCI expansion cards), the same ICMP
  echo self-test QEMU runs will run for real against whatever's on the
  wire.
- **Keyboard/mouse** (M6/M18): PS/2, not USB HID - most modern laptops
  have no physical PS/2 controller. Firmware USB legacy support usually
  makes a USB keyboard look like a PS/2 one to boot-time code like this,
  but isn't guaranteed. `[kbd]`/`[mouse]` already log "no keypress/
  movement within timeout - driver is installed, just untested" rather
  than failing either way, same as any QEMU boot nobody typed into.

## If it doesn't boot at all

- Double-check Secure Boot is actually off - some firmware calls it
  something else in its menu (e.g. "OS Type: Other OS" as a proxy toggle).
  A silent refusal to even attempt `BOOTX64.EFI` is the usual symptom.
- Confirm the `UEFI:`-prefixed boot entry was selected, not a legacy one.
- If it hangs at a blank screen before any text appears, that's most
  likely `init_framebuffer` (`boot.c`) failing to find a
  `PixelBlueGreenRedReserved8BitPerColor` Graphics Output mode - rare, but
  possible on unusual firmware. There's no fallback text-mode path left
  (M26 removed VGA text mode as anything but a pre-framebuffer stage) to
  report that failure through; a fix would need to add one.

## Reporting back

This step can't be verified by an agent - there's no physical machine or
USB port in this environment to plug into. Once you've tried it, the one
thing worth recording in milestones.md is what actually happened: which
machine, whether it booted, and if not, what point it stopped at (which
is exactly why this doc's `[net]`/`[kbd]`/`[mouse]` degrade-instead-of-
panic behavior matters - a real failure will be a lot more obvious for
not being buried under expected-but-cosmetic ones).
