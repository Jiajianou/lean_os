# The devices a real machine has

M107. Three controllers, built because `kernel/drivers/keyboard.c:12`'s
port 0x60 and `kernel/drivers/ata.c`'s IDE registers describe a machine
made in about 1998, and every other milestone in this project has been
graded on an emulator that still pretends to be one.

| | driver | found by | what it is |
|---|---|---|---|
| **AHCI** | `kernel/drivers/ahci.c` | PCI class `01:06:01` | SATA, 2005-2015 |
| **NVMe** | `kernel/drivers/nvme.c` | PCI class `01:08:02` | the disk in anything recent |
| **xHCI** | `kernel/drivers/xhci.c` | PCI class `0C:03:30` | USB, and therefore the keyboard |

All three are **found by class, not by vendor**, which is the difference
that makes one driver drive everybody's silicon. `pci_find_device` was
the right question for virtio-blk and the RTL8139 - each of those speaks
one manufacturer's registers. "AHCI" is a standardised interface, and the
class code is the field that says so.

## The block layer picks one and nothing above it can tell

`kernel/drivers/blk.c` probes in order - NVMe, AHCI, virtio, ATA - and
everything above `blk_read`/`blk_write` is unchanged. The order is not a
performance ranking: a machine with an NVMe disk and a legacy IDE
controller in its chipset has both, and the IDE controller is not where
the filesystem is.

`QEMU_DISK=nvme`, `ahci`, `virtio` and `ide` are four supported
configurations on the terms `CLAUDE.md` already sets - the image is
byte-identical across all four, and the kernel picks by probing rather
than by being told. Nothing on the QEMU command line reaches the guest as
a flag.

### Four numbers, because they differ

The boot battery is run once per backend and `[m107]` names the one it
got, so four passes of an identical test are distinguishable in a log.
The numbers below are from that run - same image, same host, same boot,
one device model apart:

| | virtio-blk | AHCI | NVMe | ATA PIO |
|---|---|---|---|---|
| `disk_1mib_cold_us` | 5210 | 4905 | see below | see below |
| `disk_1mib_write_through_us` | 83246 | 46791 | | |

The write-through row is the one worth looking at: AHCI moves the same
megabyte in a little over half the time virtio does, on the same host,
through a driver in this tree that is doing strictly more work per
request. That is a measurement rather than an explanation, and it is the
kind of thing that only exists once there is more than one backend to
compare.

## What is deliberately not built

Each of these is a decision with a condition, not an omission:

- **NCQ**, and more than one NVMe queue pair. Both buy overlapping
  commands, and there is nothing here to overlap: `leanfs` holds one lock
  across a request, so the second command cannot be issued until the
  first has returned. The condition is per-inode locking in the
  filesystem, not a faster disk.
- **MSI-X.** M107's own bullet asked for it by name and the honest
  finding on arriving was that it is the second half of a change whose
  first half nobody needs: an interrupt that completes a command is worth
  having only if the CPU has something else to do while the command runs.
  It joins `milestones.md`'s existing "interrupt-driven virtio" deferral
  on the same condition - a latency measured on metal, with the machine
  shown to be idle across it. `INTMS` and `PxIE` are masked so no
  controller can raise a line nothing is listening on.
- **USB hubs.** A device behind a hub is not enumerated. Every machine
  this is aimed at has its keyboard and its trackpad on root ports; a USB
  keyboard through a dock is the case this misses, and it is written here
  rather than discovered later.
- **Anything but boot-protocol HID.** The driver asks for boot protocol,
  which is a fixed 8-byte keyboard report and a 3-or-4-byte mouse report
  every HID device must support precisely so a BIOS need not parse a
  report descriptor. A device that refuses `SET_PROTOCOL` is skipped
  rather than guessed at.
- **Hotplug**, on any of the three. A disk that arrives after boot is a
  disk leanfs has not mounted.

## The keyboard is the same keyboard

`kernel/drivers/xhci.c` delivers through `keyboard_inject()` and
`mouse_inject()` - the same ring buffers the PS/2 IRQ handlers feed since
M51 and M56. There is no second input path, no second kind of key event,
and no change anywhere above the driver.

That is what makes the proof cheap. `LEANOS_QEMU_INPUT=usb
tools/qemu-input-test.sh` runs the **whole existing input suite** on a
machine with `-machine pc,i8042=off` - no PS/2 controller at all - and a
USB keyboard and mouse on an xHCI controller instead. Not one test
changes. With the 8042 gone, a key that reaches the guest reached it over
USB, and every assertion in that suite is about what the machine did with
the keystroke rather than about which wire carried it.

## What is tested where, and why

Three pieces are extracted into units the host tests compile, and the
rule for what gets extracted is the one `rtl8139_ring.h` states: the part
that can be **plausibly wrong** rather than loudly wrong.

- `usb_hid.c` - a boot report is *state*, not events, so a keystroke is a
  difference between two reports. A decoder that forgets to diff turns
  every held key into a keystroke per poll: typing "hello" produces
  "hhhhheeeeellllllllllooooo", and the machine looks like it is working.
  No boot marker and no serial log can see that.
- `xhci_ring.c` - the cycle bit is the entire handshake with the
  controller. Flip it one entry early and the controller stops at the
  Link TRB forever: a keyboard that works for fifteen keystrokes and then
  goes silent. Fifteen, because the ring has fifteen usable entries. The
  host test wraps the ring a thousand times against a consumer written
  the way the specification describes one.
- `nvme_split.c` - **the one path QEMU cannot reach at all.** QEMU's
  namespace has 512-byte blocks, so a 4Kn drive's read-modify-write path
  is dead code on the only machine this project has ever run on, and
  would first execute on hardware, on somebody's filesystem. The test is
  a property test over every alignment and length: the chunks must tile
  the request exactly, no chunk may span a block boundary unless aligned
  at both ends, and a 512-byte namespace must produce no partial chunks
  at all.

`tests/test_pci.c` grades BAR decoding against a modelled configuration
space (`tests/fakes/fake_pci.c`), which is the Q11 move applied to the
dword ports. Every wrong answer there is a *plausible* address: a 64-bit
BAR read as two independent 32-bit ones gives a real address in the low
4 GiB belonging to something else, and a driver would map it and write a
doorbell into it.

## Where a driver test may write

`[m107]` writes a sector and eight sectors and reads them back, and
*where* took two tries. The first choice was an LBA a gigabyte into the
disk, "past the filesystem". It is not past it: `LEANFS_START_LBA` is
8192 and leanfs's data region runs to the end of the image, so that
sector is an ordinary data block which happened to be free because the
disk is mostly empty and leanfs allocates from the front.

The sixteen sectors immediately **before** the filesystem are the right
place, and for a reason that is not a new assumption: `$(IMAGE)` writes
the MBR and the kernel from sector 0 and lays leanfs down at 8192, so a
kernel that reached 8176 would already be overwriting the superblock at
build time. The invariant is one the image build already rests on.

## The bug this milestone found in something else

The NVMe driver page-faulted the machine on its first doorbell written
after the scheduler switched to a user process - and the cause was in
`kernel/mm/vmm.c`, not in the driver.

Every address space shares `PML4[0]` and nothing else, which is what
`KERNEL_HEAP_VIRT_BASE`'s own note says is why the kernel heap sits at
256 GiB. QEMU puts a 64-bit PCI BAR at **768 GiB**. An identity mapping
of that BAR lands under `PML4[1]`: visible while the kernel's own address
space is loaded - that is, throughout probing, which is exactly why every
register access during init worked - and invisible the moment anything
else runs.

So MMIO gets a window of its own inside `PML4[0]` (`vmm_map_mmio`, 384
GiB), and `virt != phys` there. It costs nothing: a BAR is registers, and
registers are never handed to a device as a DMA address.

AHCI never found this. Its BAR is below 4 GiB, so identity-mapping it
happened to land under `PML4[0]` and worked - which is the useful part of
the story. The first driver was correct by luck of address, and the
second one was the one that asked the question.

## And one thing QEMU will never exercise

`kernel/drivers/xhci.c`'s `take_ownership()` performs the `USBLEGSUP`
handshake: on a real machine the firmware owns the controller when the
kernel starts, because it has been reading a USB keyboard in its own boot
menu. A driver that programs the controller without asking first is
fighting SMM code for the same registers, and the resulting hang is not
one a serial log can explain.

QEMU implements no such capability, so the function is a no-op here and
stays untested until M110. It is written now for the same reason
`VMM_FLAG_NOCACHE` is: both are assumptions this project would otherwise
ship unexamined, and M107's whole "graded under QEMU first" bullet exists
so that what is debugged on metal is the assumptions rather than the
drivers.

See `milestones.md`'s M107 entry, and `docs/real-hardware.md` for the
boot that has still never happened.
