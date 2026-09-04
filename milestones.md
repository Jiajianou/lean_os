# lean_os — Milestones

**The live record: where this project is, what is open, and what comes
next.** Read this file before touching a subsystem, and update it as soon
as work lands — status, what it cost, what went wrong, and what that
taught. The last part is the most valuable thing in it.

*Snapshot written 2026-09-04, at commit `b927826`, after M99 landed.*
*Entries added since are in* Landed since this snapshot, *above the snapshot body.*

**The history moved.** Everything from M0 to M106 and Q1 to Q20 — every
entry, every cost, every bug and what it taught — is now
[milestones-archive.md](milestones-archive.md), 14,900 lines of it. That
file is frozen and is where you go for detail: *why* leanfs orders its
writes the way it does, *what* the pseudo-terminal cost, *which* bug the
mutation harness found first. This file carries the state and the
conclusions. When an entry here says "see M85", it means
`grep -n '^### M85' milestones-archive.md`, and the archive index at the
end of this file lists every heading you can grep for.

---

## How to read this file

**Two identifier series, one scheme.**

- **`M<n>` — build milestones.** The main line: bootloader, kernel,
  drivers, filesystem, desktop, userland. One at a time, in order.
- **`Q<n>` — the testing arc.** Work on the instruments rather than on
  the machine. It runs alongside the M line rather than inside it, which
  is why it carries its own numbers. `Q` identifiers are referenced from
  [Makefile](Makefile) and from [docs/](docs/), so they are stable and are
  never renumbered into the M series.

Commits are always `M<n>: <what changed>`. Q work commits under whichever
M number it landed alongside — there has never been a `Q<n>:` commit.

**Status marks:** `[ ]` not started · `[~]` partly done · `[x]` done ·
`[⊘]` abandoned. Anything that is not a plain `[x]` says what is left.

**Lowercase `[m50]`, `[m99]`, `[wm30]` and the like are not milestone
references.** They are the literal marker tags the boot self-tests grep
for in the serial log — see
[tools/qemu-serial-test.sh](tools/qemu-serial-test.sh). They keep their
own casing on purpose; changing one breaks the harness.

**Numbers in this file are measurements, not estimates.** Where a number
appears without a source, it came from `tests/budgets.tsv` or from the
run recorded in the archive entry named beside it. A claim about speed,
size or memory with no number behind it does not belong here.

---

## Where this is, in one page

A desktop OS for x86-64 that boots its own hand-written UEFI loader,
manages its own memory and disk, runs a windowed desktop, and — since the
last arc — **builds and runs other people's software with a toolchain it
carries itself.**

The goal sentence in `CLAUDE.md` has two halves. The first half is done:
this is a Unix-interface-compatible OS complete enough to build and run
somebody else's software against. GCC, binutils and GNU make run *on the
machine*; CPython 3.12.7 runs on it and passes 1,275 of 1,332 of its own
regression tests; toybox supplies 143 command names; every object this
machine's compiler produced from bzip2's source is byte-identical with
the cross compiler's. The second half — *on a machine that manages its
own CPU, memory and disk honestly* — is where the open work is: real
device drivers, four cores under the full battery, and a boot on metal
that has never happened.

| | state |
|---|---|
| **Milestones** | M0–M110 numbered: 101 `[x]`, 4 `[~]` (M28, M92, M99, M103), 1 `[⊘]` (M80), 5 not started (M100, M107–M110) |
| **Testing arc** | Q1–Q20 written, 18 `[x]`; Q7 half landed, Q14 not started |
| **Head of the queue** | **M100** — M99 closed 2026-09-04 |
| **Held by instruction** | all real-hardware work: M110, M28's last box, M108's link half, M103's two hardware-conditioned boxes |
| **Host unit tests** | 247/247 passing, 3 slow ones skipped in `--fast` |
| **Boot markers** | 110 required, graded on every self-test boot |
| **Performance budgets** | 31 rows in `tests/budgets.tsv`, all inside their ceilings |
| **Source** | ~50k lines kernel, ~50k user space, ~3.3k system_api, ~9.5k tests |
| **Working tree** | clean at `b927826`; nothing half-landed anywhere |

**Nothing is half-finished across milestones.** Every `[~]` in this file
is a milestone that landed what it could and *named* what it did not,
with a condition attached. There is no partial work sitting in the tree.

---

## The machine as it stands

What actually exists, subsystem by subsystem, with the numbers that
matter. This is the inventory to read before proposing anything — a
surprising amount is already here.

### Boot and CPU

- Hand-written PE32+ EFI application (`kernel/boot/uefi/boot.c`), built
  with clang+lld. No GRUB, no Multiboot, no Limine. The legacy BIOS path
  was removed in M26 and is not coming back.
- Long mode, paging, GDT/IDT/TSS, SSE enabled, NX honoured.
- **SMP works and is tested** — but every harness pins `QEMU_CPUS=1` on
  purpose. Four cores boot and share work: four equal tasks cost 114–200%
  of what one task costs across runs, where 100% is perfect scaling and
  400% would be one core doing all of it. It is a noisy number — which is
  why the kernel takes the best of three rounds inside the guest and the
  budget sits at 250 rather than near the observation. `tools/smp-test.sh`
  grades it in the default tier. The full battery is *not* green on four cores; see *Known
  divergences*.
- I/O APIC with MADT interrupt source overrides, PIC masked rather than
  deleted; per-vector per-CPU interrupt statistics in `/proc`. The 8259
  is still the default because the I/O APIC costs this machine 2x under
  QEMU — a measurement, recorded in M103, that M110 is meant to re-take
  on metal.

### Memory

- Physical memory sized from the firmware's map, not a constant. 4 GiB
  default, `QEMU_MEM=128` is a supported configuration.
- Per-process address space measured in hundreds of gigabytes; NX,
  `mprotect`, `MAP_FIXED`, `MAP_SHARED`, file-backed `mmap`, demand
  paging, a stack that grows when touched, and mappings that coalesce.
- Resident-set accounting per address space, carried out through
  `getrusage`'s `ru_maxrss` — the first memory number this kernel could
  report, added in M98 to decide M102's swap question.
- **Swap does not exist and is refused on a number:** the largest C++
  translation unit this machine compiles peaks at **217 MiB**, a
  nineteenth of the 4 GiB it is configured with, and nothing the whole
  bootstrap does comes near M90's ceiling. The condition that reopens it
  is in the same units — a translation unit whose peak exceeds what a
  machine of this size has free, meaning a compile roughly ten times this
  one. `build_cxx_tu_peak_rss_kib`'s 512 MiB ceiling is the row that
  fires first.
- OOM is honest: the kernel reports it rather than dying, graded by two
  boot markers.

### Scheduling

- Pre-emptive, multi-core capable, one coarse `sched_lock` — **measured
  and kept.** Per-CPU run queues and work stealing were refused in M106 on
  the measurement above: four tasks of equal work came in as low as 114%
  of one task's time, so that lock is not what stops this machine using
  its cores.
- Wait queues, no busy loops. Latency measured as a distribution
  (best/median/worst, idle and loaded), six budget rows.
- `MAX_TASKS` 128, `MAX_FDS` 128. High-water marks are budget rows that
  fail at 100, so the day the headroom goes the number says so before a
  spawn does. An ordinary boot peaks at 9 tasks; CPython's regression
  suite peaked at **90**.
- CPU affinity is deferred with a condition: it becomes worth building
  when the full battery is green on four cores.

### Storage and filesystem

- virtio-blk (polled DMA) with ATA PIO as the fallback that boots
  anything; `QEMU_DISK=ide` is a supported configuration, byte-identical
  image.
- Block cache with **writeback and a barrier** (M104). Readahead exists
  and is **off by default**, because measured it was slower — 9.1 ms
  against 4.9 ms for the same megabyte.
- leanfs v3: directories, hard links, symlinks, files to 4 GiB, a source
  tree's worth of files, and a host-side image builder (`tools/leanfs-put`).
- **No journal, refused three times** — most recently with both of M71's
  conditions under pressure at once. The restated condition is in
  *Deferred*, and it is now a condition on this project's own work rather
  than on the world.
- Crash consistency: 16 SIGKILLs across the heaviest metadata window,
  every one survived, verified by an independent Python reader.

| what | cold | warm |
|---|---|---|
| 1 MiB through virtio-blk | 5.2 ms | 2.4 ms |
| 1 MiB through ATA PIO | 95 ms | 2.4 ms |
| 1 MiB written, absorbed by writeback + barrier | 15.9 ms | — |
| 1 MiB written through | 77.4 ms | — |
| full unclean-mount scan of the boot filesystem | 711 ms | — |

The mount scan is the row to watch: it is **linear in allocated bytes,
not in file count**, and it went 128 ms → 711 ms when the native
toolchain's 160 MB landed on the image. The next multiple of growth puts
it near a second, which is where M105 said a mount stops being instant.

### Desktop and input

Overlapping windows with a real z-order, titlebars, drag/resize/snap,
minimize with motion, taskbar, Spotlight-style launcher, context menus,
toasts, drag and drop, four virtual desktops, keyboard chords, a session
that restores what was open where it was. Terminal, editor, file manager,
task manager, settings, paint, clock. Wallpaper, colours, live resolution
change with a confirm-or-revert countdown, motion and volume all persist.
PC-speaker beep and an AC'97 output stream.

The compositor is **5,189 lines, the largest file in user space, and the
one every visible bug in this project has lived in.** It is still graded
only through a booted machine and a screendump. That is Q14, and it is
row 4 of the queue.

### Terminal, processes, POSIX surface

Line discipline with canonical and raw modes, sessions, process groups,
job control, `^C` and `^Z` with `waitpid(WUNTRACED)`. Pseudo-terminals:
`/dev/ptmx`, `/dev/pts/<n>`, `openpty`, `forkpty`. `fork`, `execve`,
`vfork`, `posix_spawn`, `wait4`, pipes, shared memory, environment
inherited across spawn, a real working directory, signals a program can
**catch** — including faults, since M99 — delivered through a frame on
its own stack, with a real `siginfo_t`. POSIX threads, TLS, futexes.
`<dirent.h>`, `<sys/stat.h>`, `<unistd.h>`, `<signal.h>`, `<pthread.h>`,
`<sys/mman.h>`, `<setjmp.h>`.

### Network

RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, and TCP — the eleven-state
machine, retransmission with a measured RTO, Reno congestion control.
DHCP client, SNTP client, sockets as ordinary file descriptors,
`netconf`. No TLS: that is M100's, and it is what gates `https://`.

### Capabilities

Every process carries a capability set assigned from a manifest at spawn;
it can only ever shrink. An ordinary app cannot paint the screen, read
the clipboard, list processes, open a socket or power the machine off.
See [docs/capabilities.md](docs/capabilities.md). There is one principal
and the machine says so — `chmod` is a truthful failure, not a no-op that
returns 0.

### The toolchain, and what it builds

- `x86_64-lean_os` is a **real target triple** in binutils and GCC, built
  by `tools/build-toolchain.sh` from a nine-edit port.
  `x86_64-lean_os-gcc hello.c -o hello` produces a program this machine
  runs **with no flag supplied by hand** — load address, code model, red
  zone and startup files all come from the target description.
- A dynamic loader (`/lib/ld-lean.so`), `dlopen`/`dlsym`, C++ with
  exceptions that cross a shared object.
- **On the machine:** binutils (`as`, `ld`, `ar`, `nm`, `objdump`,
  `strip`, and two more), GCC (`cc1`, `cc1plus`, the driver), GNU make
  4.4.1, toybox as one static binary behind 143 command names, and
  CPython 3.12.7 with its standard library on disk as `.py` source.
- Built *by* the machine, *for* the machine: bzip2 1.0.8 from its own
  Makefile — eight translation units, an archive, two links, 203 s — and
  its own test suite passes on the result.

| what this machine costs to compile | here | on the host QEMU runs on | ratio |
|---|---|---|---|
| one C translation unit (bzip2's `bzlib.c`, 1,572 lines) | 32.1 s, 56 MiB peak | 0.25 s, 42 MiB | 128x |
| one C++ translation unit (templates, 193 lines) | 86.9 s, 217 MiB peak | 1.50 s, 214 MiB | 58x |
| the assembler on a 29,512-line `.s` | 8.3 s | 0.02 s | ~400x |

**The three-stage GCC bootstrap does not fit, and the number says by how
much:** `all-gcc` is 1,018 objects and a 3.4 GB build tree; stage 2 alone
is ~24 hours of wall clock here, and the image would have to be 8 GiB.
Memory fits with room to spare. It is a wall-clock problem and only a
wall-clock problem, and most of that wall clock is QEMU's TCG interpreter
rather than this OS. What was done instead is stronger than it sounds:
**every one of eight objects this machine compiled is byte-identical with
the cross compiler's**, which is the argument stage 2 == stage 3 makes,
applied to a program small enough to finish.

---

## The instruments

**No instrument here subsumes another**, and that is not a slogan — it is
what four arcs of testing work found. Host tests reach error paths a
booted machine cannot; the boot self-tests prove subsystems from the
inside; the input suite grades real framebuffer pixels because that is
the path a person's hands take (learned the hard way in M40); the
differential tests are the only ones where nobody here decided what the
right answer is.

```sh
./tools/run-tests.sh --fast   # host tier only. No QEMU. ~6 s.
./tools/run-tests.sh          # + graded boot, four-core boot, quick input subset. Minutes.
./tools/run-tests.sh --full   # + whole input suite, crash test, disk faults, I/O APIC boot, bootstrap
```

`make test` / `make test-fast` are the same thing. **There is no CI and
there is not meant to be one** — `.github/` was deleted along with the
workflow that had never run. Every tier is run by hand, on this machine,
before a commit, which is the arrangement all 101 landed milestones were
graded under. Nothing in this tree may depend on a hosted runner.

| instrument | what it grades | how it is run |
|---|---|---|
| **Host unit tests** (`tests/`) | libk, heap, malloc, leanfs, every network parser, the TCP state machine, PTY, symtab, UTF-8, fnmatch, getopt, fwcfg — and the **scheduler** (Q13), 2,157 lines against a fake timer and a fake CPU, with lock-order inversions made errors rather than comments. **247 tests**, ASan+UBSan, under a second | `--fast` |
| **Boot self-tests** | **109 required markers** and 31 performance budgets, graded off the serial log of a real boot. The switch comes from outside the image via fw_cfg, so the image is byte-identical with or without them | `tools/qemu-serial-test.sh` |
| **Input suite** | real clicks and keys through QEMU's monitor, graded on real framebuffer pixels. Most tests check something *did* change; two check that nothing else did, which is the only way to catch a flicker (Q7/Q15). Boots once per image and restores a snapshot per test, keyed on the image hash so a stale one fails closed (Q19) | `tools/qemu-input-test.sh` |
| **Differential tests** | `sh`, the regex engine, `sscanf`, `printf`, libm, `realpath`, and the FILE layer — each compiled for the host from the same source the machine runs, put beside the host's own, and required to agree. Nothing in the fixtures says what the right answer is | `--fast` |
| **Fuzzers** | network parsers and the mount path, ~150k inputs/second | `make fuzz-run` |
| **Mutation harness** | breaks the kernel on purpose and reports whether the tests noticed. The only instrument that grades the *tests* | `make mutate` |
| **Crash test** | SIGKILL mid-write, reboot, verify with an independent reader. 16 cuts | `tools/crash-test.sh` |
| **Disk-fault test** | QEMU `blkdebug` refusing **every** write, through virtio and through ATA, and the machine must still reach PID 1 | `tools/disk-fault-test.sh` |
| **Image-tree test** | a host tool writes a tree into a leanfs image, an independent reader compares it to the source directory, then the machine walks and hashes it back from inside | `tools/image-tree-test.sh` |
| **Exhaustion** (`/bin/exhausttest`) | descriptors, pipes, shm segments and sockets to their ceilings — refuse, recover, work again, twice, with a leak audit across 2,200 rounds | `[q9]` boot marker |
| **SMP test** | four cores boot, each recognises itself, four equal tasks share them | default tier |
| **Bootstrap test** | this machine's own gcc/as/ar/ld building bzip2, and bzip2's own test suite on the result | `--full`, own fw_cfg switch |
| **Python test** | CPython's own regression suite, on the machine, reporting **its own** counts. The only instrument here that neither wrote its own assertions nor chose what to assert | `tools/python-test.sh` |
| **Coverage ratchet** | 11 rows in `tests/coverage-floor.tsv`; coverage may not go **down**. There is no coverage *target* and there will not be one | `make coverage-check` |

**100% line coverage is not a passing grade.** The mutation harness exists
because the first file it examined (`net/ethernet.c`) had full line
coverage and a mutation score of **zero** — every line ran and nothing
asserted what any of them did. Read the coverage floor next to
`make mutate`, never instead of it.

**Budgets are a claim about this machine.** They were measured on QEMU
under macOS on Apple Silicon. Measured elsewhere they will move, and
moving them with a recorded reason is the process working. A budget that
has sat at 2x slack for ten milestones is a bug in the budget.

---

## How work is decided here

These are the rules the archive earned, each with the entry that earned
it. They decide arguments; they are not preferences.

- **Measure before optimizing.** M69 set this. Performance work on an
  unmeasured path does not get done. It has since refused per-CPU run
  queues (M106), readahead by default (M104), a journal (three times),
  swap (M102), and interrupt-driven virtio (M103) — every one of them on
  a number, and every one recorded with the number.
- **Don't build a thing that pretends to enforce something.** M65 refused
  a permission model with no users behind it. A machine with one
  principal reports one principal.
- **Deferrals need a condition, not a mood.** Every entry in *Deferred*
  below says what would have to become true. Check it before proposing
  that work — and note which way the conditions have historically gone:
  the deferrals that survive are the ones whose condition is about *the
  world* (two people sharing a machine, a vendor's GPU); the ones that
  fall are the ones whose condition was about *a measurement this project
  had not taken yet*. The second kind was never really a refusal — it was
  a scheduled question.
- **A test that passes on a configuration nothing exercises is a test of
  nothing.** M106's `[smp]` marker printed "1 CPU(s) online" and passed on
  every boot since M7, while five real bugs sat behind the second core.
  Grading a configuration is not the same thing as running one.
- **The failing build is the specification.** M63's method, used by every
  port since: run it, read the error, add exactly what it named. Every
  predicted list of missing features in this file has been wrong, and
  every one of them says so in its own entry.
- **An error that does not name itself is worse than a crash.** Five
  syscalls in M99 returned -1 over an untouched `errno`, and the visible
  symptom was `print()` writing nothing and exiting 0.
- **Decide and proceed.** Where this file and `CLAUDE.md` do not settle a
  question, take the reading most consistent with them, act, and record
  the assumption in the entry. Do not stop to ask.
- **Never push.** `git push` is not part of this loop, ever. Commits stay
  local; what leaves the machine is the user's call.

---

## The queue

Eleven entries are still open — M28, M92, M99, M100, M103, M107, M108,
M109, M110, Q7 and Q14 — and two that are `[x]` named work they did not
finish (M98's tail and M106's). They fold to the ten rows below: M28's one open box **is** M110's first box, M92's last box
collects inside M107 under the condition already written for it, M103's
three boxes split two ways between M107 and M110, and M98's tail is
mostly row 5's bug with three items recorded rather than scheduled.

**This is the order.** Nothing below is invented and nothing is promoted
from the deferred list; the loop is one milestone at a time, taken to
completion, and an arc that collects a second plan while the first is
unbuilt is the drift *Deferred* exists to catch.

| # | milestone | state | why here |
|---|---|---|---|
| ~~**1**~~ | ~~**M99 (2nd)**~~ | **done 2026-09-04** | all five boxes closed across three increments — see *Landed since this snapshot*. Neither open box was about what its own entry predicted: the loader box was mostly a target-description box, and the build box was a shell box |
| **2** | **M100** — the browser gap, measured | not started | the last milestone of its arc and the one that specifies the arc after it. **TLS lives here**, which is also the fetch M108's three TCP deferrals are conditioned on, and `AF_UNIX`/`socketpair`/`O_NONBLOCK` are absorbed here |
| **3** | **Q7 (2nd)** — the golden-frame baselines | half landed | the invariant half exists and has caught its bug; the baselines, the diff artifact and `make accept-visuals` do not |
| **4** | **Q14** — the compositor, off the machine | not started | the move Q13 made on the scheduler, on a 5,189-line file — taken *before* M107 puts USB input underneath it, so the rewiring is graded in milliseconds rather than only through a screendump |
| **5** | **M106 (tail)** — the battery green on four cores | 3 known failures | not a new milestone: the three failures M106 named and left. It gates CPU affinity, and M98's bootstrap profiler already reproduces one of them on demand — the first reproduction this project has that is not "about one boot in ten hangs" |
| **6** | **M103 (2nd)** — MSI/MSI-X | condition fires at row 7 | its own condition is *"a driver for a part that has no other way to interrupt"*, and M107's NVMe bullet answers it by name. The LAPIC-timer and interrupt-driven-virtio boxes are **not** collected here — they wait for hardware |
| **7** | **M107** — AHCI, NVMe, xHCI + USB HID | not started | all three gradeable under QEMU at this desk, which is what makes M110 a boot rather than a bring-up. Collects M92's last open box on the way |
| **8** | **M108 (driver half)** — a real NIC | not started | the `e1000e` driver is QEMU-gradeable and belongs here. The link half is held — see below |
| **9** | **M109** — lean_os built on lean_os | not started | needs nothing from rows 7–8 and stays after them anyway: the tree it rebuilds should be the whole tree, drivers included, or the generational test grades a subset of the machine it runs on |
| **10** | **M110** — the boot that has never happened | **held** | see the hold |

**Rows 3 and 4 are the one ordering decision this table makes**; the rest
is dependency or standing arc order. The argument: M107's grading bar is
the full battery run four times over, once per storage backend, plus the
input suite with the PS/2 devices deleted from the QEMU command line. Q19
made each of those boots cheap; rows 3–4 are what make each of them
*sharp*, and that is repaid once per backend per run for the rest of the
project's life. The counterweight is on the record too — building an
instrument ahead of its subject cost M101 and M102 each a box they could
not close — but it does not apply here: Q7 and Q14 grade code that
already exists, not measurements M107 has yet to produce.

**Where the eleventh milestone comes from — not from this table.** M100
ends with a gap analysis carrying a number on every line, and M110 ends
with eleven budgets measured on metal for the first time. Whatever
follows row 10 is specified by those two documents, or it is speculation
with a milestone number attached.

### The hold on hardware work

**Added 2026-09-03 by the user's instruction, which outranks the table
above.** Real-hardware work does not start until the user says it does —
even if a machine and a USB drive turn up. That covers:

- **M110** in its entirety,
- **M28's last box** (the same box),
- **M108's link half** — a physical NIC, a real router, a lease that
  expires,
- **M103's two hardware-conditioned boxes** — the LAPIC timer and
  interrupt-driven virtio, both of which wait on M110's measurements.

Row 10's standing permission to jump the queue is suspended: its blocker
is now a decision as well as an object, and the decision is the user's.

**The reading this hold was given, recorded rather than assumed.** The
instruction was "finish the open items, with the exception of the
hardware related work". Two readings were available and the narrower one
was taken: *hardware work* means work that cannot be graded at this desk.
It does **not** cover M107 or M108's driver half, because those are
drivers for parts QEMU implements, and M107's own fourth bullet requires
every one of them to be *"graded under QEMU first ... so the drivers are
debugged here and only their assumptions are debugged on metal"*.

---

## Landed since this snapshot

### M99 (second increment) — a Python that loads its modules `[x]`

*Landed 2026-09-04.* The fourth of M99's five boxes: **extension modules
as shared objects, over M95's loader.** `python3` on this machine is now
an 8 KB position-independent executable that `/lib/ld-lean.so` relocates
against a 6.6 MB `libpython3.12.so.1.0`, with **58 C extension modules in
`/usr/lib/python3.12/lib-dynload`, each `dlopen`ed by full path at the
moment something imports it.** `import _socket` opens a file.

**What it actually turned out to be about.** M99's own entry named the
pieces — "four `case $ac_sys_system` arms in CPython's configure, `-pie`
on the executable, and `MAX_OBJECTS` in `user_space/ld/ld-lean.c`" — and
got the count right, one of the members wrong, and the *subject* wrong.
It reads as work on the loader. Four of the six defects were in the two
places this project tells other people's builds to look: the compiler's
target description and the C library.

**The compiler could link a PIE and could not compile the objects for
one.** M94 wrote `DRIVER_SELF_SPECS` and said what it was for — *every
flag invented by hand is a flag someone else's build system will not
pass* — and M95 grew `LINK_SPEC` and `STARTFILE_SPEC` for dynamic
linking and left that one alone. So `-pie` still got `-mcmodel=large`
and `-fno-pic`. The evidence had been sitting in
`tools/build-dynamic.sh` for four milestones: every link in the file
that demonstrates this feature read `-pie -fPIE -mcmodel=small
-nostdlib -nostartfiles Scrt1.o libc.so ld-lean.so crti.o crtn.o`. Nine
things by hand, in the demonstration. CPython's Makefile has nowhere to
put them, which is what turned a loader box into a target-description
box.

`gcc/config/lean_os.h` now answers four questions from the flags every
other target answers them from:

| given | code model | PIC | else |
|---|---|---|---|
| `-shared` | small | `-fPIC` | `-lc`, no crt1 |
| `-pie` | small | `-fPIE` | `Scrt1.o`, `-l:ld-lean.so`, `-lc` |
| `-fPIC`/`-fpic`/`-fPIE` on a `-c` line | small | as given | — |
| neither | large | `-fno-pic` | `-static --no-relax -T lean_os.ld` |

The third row is the one the first draft got wrong, and it is worth
keeping: **a compile line has no `-shared` on it.** CPython builds a
module's objects with `$(CC) -c $(CCSHARED)` — `-fPIC` and nothing more —
and links them with `$(CC) -shared` afterwards. Keying the code model on
the link flags alone gave those objects the large model, which is a page
of relocation errors at the end of the compile rather than the start.
Position-independent code on this target is small-model code, and the
flag that says which is the one on the line being read.

`-ftls-model=initial-exec` joined them for `-shared`, and that fact had
been written down twice in this tree — once in the Makefile's `libc.so`
rule and once in `build-dynamic.sh` — and not once where a stranger's
build would find it. `libpython3.12.so` uses `__thread`; the link ended
with "undefined reference to `__tls_get_addr`" from a linker that was
right. The cost is the limitation M95 already recorded: **an object
`dlopen`ed after startup cannot have thread-local variables.** Checked
rather than assumed — none of the 58 extension modules has a `PT_TLS`,
only `libpython` does, and it loads at startup.

`--no-relax` on the static branch is the honest consequence of the
code-model change and arrived as a regression in the C++ test. GCC
builds `libgcc_eh.a` — the unwinder every static C++ program links —
with `-fPIC`, which now means small model, correctly. ld then tries to
relax `mov pthread_cancel@GOTPCREL(%rip)` into a `lea`, cannot reach
from 512 GiB, and stops naming the flag it wants. Nothing is lost but
one indirection in code already going through the GOT.

**The C library knew the name of `main`.** `libc.so` carried an
undefined `main`, because `__lean_start` calls it. Correct in a static
`libc.a`; wrong in a shared `libc.so`, and this file is built both ways.
CPython builds with `-fvisibility=hidden` and marks its public API — the
ordinary arrangement — so its `main` is hidden, and the link ended with
`hidden symbol 'main' in Programs/python.o is referenced by DSO`: an
error about the C library, from a program that had done nothing unusual.
`crt0.asm` and `crt0-pie.asm` pass `main` to `__lean_start` in `%rcx`
now, which is what glibc's `crt1.o` does and for this exact reason. **A
C library that knows the name of a program's entry point has an opinion
it has no way to be right about.**

**Three in the loader, two of them found by twelve lines of fixture.**

- `MAX_OBJECTS` 16 → 96, and 16 was never a *tested* number: nothing
  here had ever opened two. `dlclose`'s own note names the condition for
  raising it — "a program that dlopens more than MAX_OBJECTS things over
  its life" — and Python is that program.
- **`open_lib()` never tried the name it was given.** It searched
  `LD_LIBRARY_PATH`, `/lib`, `/usr/lib`, so an absolute path became
  `/lib//usr/lib/python3.12/lib-dynload/_socket.cpython-312.so` and the
  loader reported "cannot find" about a file it had been pointed
  straight at. Every dynamic linker opens a name with a slash in it as a
  pathname; leaving that out was a bug, not a simplification, and M95's
  fixtures asked for `libdyn.so` so nothing noticed.
- **`dlopen` could not fail.** Eighteen call sites of `dl_fail`, which
  prints and `exit(127)`s. Right at startup — a program whose libraries
  are missing has not begun and there is nobody to tell — and wrong for
  `dlopen`, which POSIX says returns NULL. The consequence is not the
  missing file, which a program can stat for itself: **an interpreter
  `dlopen`s a candidate and expects to be told no**, and against a
  loader that exits, one unresolved symbol in one extension module ends
  the process with no traceback. The loader has its own
  sixteen-instruction `_dl_setjmp`/`_dl_longjmp` now (`ld-start.S`),
  because the C library that would have supplied one is a thing this
  file loads.

**And one in the kernel, which presented as the wrong thing entirely.**

```
[oom] out of physical memory filling 0x000000A0001CCF10 for task
      python3 pid 0x00002C0A - killing it, not the machine.
      1036379 frames free.
```

Four gigabytes free and an out-of-memory kill. `FILEMAP_MAX_PAGES` was
**512** — 2 MiB of shared file mapping across the whole machine — under
a comment from M91 that said "more than anything on this machine maps",
which was true when it was written and stopped being true the moment
this machine had a 30 MB shared library. `libpython3.12.so`'s read-only
text is 1,217 pages on its own. 8192 now, 32 MiB, 192 KiB of kernel
`.bss`. **The second time in this project that a fixed-size table's
ceiling has presented as a memory failure somewhere else**, and the
message was accurate about the mechanism and misleading about the cause.

**The measurement, and it went the other way.** `python_fixture_ms`
**4400 ms → 3640 ms**: the same fixture is *faster* dynamic than static,
which is the opposite of what a loader is usually assumed to cost. The
static interpreter demand-paged one 12 MB file whether or not a byte of
it was reached; the dynamic one pages in the part of `libpython` it
touches and the two extension modules it imports. The budget row is
re-measured rather than left at the old number.

**What grades it.** A new boot marker, `[m99ld]`, running
`tests/dynamic/manydyn.c` against 24 generated shared objects: 24 open
at once (past 16, inside 96 — the only interval where a test can tell
the old ceiling from the new one), a symbol resolved out of each *after*
all of them are open, every one named by a full path into `/lib/many`
which is on no search list, and a path that is not there refused **as a
path** rather than starting a search. It failed twice on the way in and
found two of the three loader bugs. `tests/python/m99.py` gained the
other half — `_socket.__file__` is a `.so` that exists, is not the
interpreter, and lives beside `array`'s — and skips rather than fails
under `LEANOS_PYTHON_LINK=static`, because that is a configuration the
build script still offers.

**Cost:** four rebuilds of GCC. Which is also the reason
`tools/build-toolchain.sh`'s port stamp no longer hashes `lean_os.h`:
the stamp exists because an *anchored edit* re-applies and leaves its
old copy behind, and `lean_os.h` is **copied**, whole, over whatever was
there. Hashing it made every change to the target description throw away
binutils and GCC and rebuild both — the tightest loop in this port
paying the price of the loosest one.

**Still open in M99:** `./configure && make` for CPython *on the
machine*. Unchanged, and see *Every open box*.

### M99 (third increment) — a shell somebody else's configure can run `[x]`

*Landed 2026-09-04.* M99's last box asks for `./configure && make` for
CPython **on the machine**, and says the deliverable is the number that
says by how much it does not fit. **The number was not the answer, and
finding that out cost one command:**

```
$ build/sh-host .../Python-3.12.7/configure --help
sh: syntax error near `&&` or `||` with nothing after it
```

Not a wall-clock problem. Not a memory problem. This project's shell
could not run somebody else's configure script at all, and would not
have got as far as being slow.

**The instrument that made that a one-command question is M86's**, and
this is the second time it has paid for itself: `tools/sh-test.sh`
compiles `user_space/shell/sh.c` *for the host*, so the shell the
machine runs can be pointed at a 33,000-line script without booting
anything. A loop that would have been a QEMU boot each time was a
second.

**Eleven bugs, and not one of them was reachable from anything in this
tree.** That is the finding, more than the list:

| what | why nothing here had found it |
|---|---|
| **A one-token peek with a side effect.** `parse_command` peeked ahead to ask "is this a function definition?" and rolled `lx.i` back if not. Lexing a newline *collects here-document bodies*; rolling back a position does not roll back a lex. So `cat <<EOF \|\| fail=1` read the body **and then executed it** | needs a here-document, an operator, and a last command that is a bare assignment. Line 32,443 of configure |
| **Pending here-documents were a stack.** `cat <<A; cat <<B` filled B from A's body | nothing here had ever written two on one line |
| **`"..."` ended at the first `"` inside a `` `...` ``.** So `"#define `printf "%s\n" "HAVE_$h" \| $as_tr_cpp` 1"` became several words | needs a substitution inside a quoted string whose body is itself quoted |
| **Here-document bodies expanded `$` and not `` ` ``** | half of "as if in double quotes", and the half no fixture used |
| **No `$(( ))`** | autoconf's own suitability probe is `test $(( 1 + 1 )) = 2 \|\| exit 1` |
| **No `exec`** | `exec 5>>config.log` is how configure opens the log every later `>&5` writes to |
| **No `trap`** | configure cleans up and gets its exit status right with `trap '...' 0` |
| **`set -e` was a positional parameter.** `set -e` *replaced the arguments* with the word `-e` | `Modules/makesetup` is `#! /bin/sh` then `set -e`, and its own parser then saw `-e` and printed its usage |
| **No `[...]` in patterns.** Every bracket expression fell through to the default arm, silently | `case $val in [\\/$]* )` — how configure asks "is this an absolute path" |
| **`${*-word}` asked the variable table**, where there is no entry called `*` | `for i in ${*-Setup}` is makesetup's main loop |
| **`"$@"` with no parameters was one empty field**, and `${1+"$@"}` collapsed to one; a backslash-newline inside `"..."` was two characters rather than a continuation | POSIX corners that only a generated script writes |

**What it took to find them, in order, is the point.** Each fix moved
the failure forward and the next one was somewhere new: a parse error →
`invalid host type: $@` → every `sizeof` reported as **0** → `makesetup
failed` → a Makefile with no modules in it. The third one is this
project's own recorded lesson arriving from a different direction — *a
configure probe that fails to compile writes a number, not an error* -
and the fifth is worse than a failure, because configure exited 0.

**The result, and it is a differential one.** `./configure` for CPython
3.12.7 now runs to completion under this project's shell: **753 checks,
and `pyconfig.h`, `Modules/config.c` and the `Makefile` byte-identical
with the same configure run by the host's own `/bin/sh`.** 56,084 bytes
of answers about a system, and the two shells agree on all of them.

`tools/configure-test.sh` is that comparison, run twice on demand, in
`--full`. It is a seventh differential test and the first whose
*fixture* is somebody else's program:

> sh-test grades fixtures somebody here wrote against an oracle nobody
> here wrote, which is the right shape and has a ceiling — a fixture can
> only test a construct somebody thought of, and the whole difficulty
> with a shell is the constructs nobody thinks of.

Three fixture files grew to cover the bugs by hand as well (`05`, `07`,
`08`, and new `09-arithmetic.sh`, `10-exec-and-trap.sh`,
`11-set-options.sh`), because the configure test is minutes and the
fixtures are a second, and because a bug that is only caught by a
33,000-line script is a bug nobody will bisect.

**What was implemented rather than accepted.** `set -e` is real
errexit, suspended inside `if`/`while`/`until` conditions, the left of
`&&`/`||`, and under `!` — which is the part everyone gets wrong and the
part the host oracle decides. `set -u`, `-f`, `-x` and `-o` came with
it; an option this shell does not have is an **error**, not a no-op,
which is M65's rule. `trap` runs its action between commands and says so
in a comment, because arbitrary script inside a signal handler is not
something any shell attempts.

**One process note, recorded rather than hidden:** one run of the
default tier's boot stage failed and the two runs either side of it -
one standalone at 110/110, one full tier - passed. Nothing was changed
between them. It is not reproduced and it is not explained.

**Still open in M99:** `./configure && make` for CPython **on the
machine**. The shell is no longer the answer to why not; what is left is
the wall clock, and that is a measurement this increment did not take.

### M99 (fourth increment) — what the build would cost, measured `[x]`

*Landed 2026-09-04.* **M99 is closed.** Its last box asked for
`./configure && make` for CPython on the machine, with M98's rule
attached: if the build does not fit, *the number that says by how much*
is the deliverable. The third increment answered the half nobody
expected — the shell — and this one takes the number.

```
  MEASURED HERE, by tests/pybuild/run.sh:
    one configure probe, first run          4830 ms
    one configure probe, steady (mean 3)    7490 ms
    one C translation unit                 17010 ms   peak RSS 57920 KiB

  COUNTED from real runs of CPython 3.12.7's own build:
    checks its configure ran                753   (tools/configure-test.sh)
    objects its make produced               272   (the cross build)

  MULTIPLIED:
    ./configure   7490 ms x 753 = 5639 s (93 min)
    make         17010 ms x 272 = 4626 s (77 min)
    together                      10265 s (2 h 51 min)
```

**The surprise is which half is bigger.** `./configure` costs *more*
than compiling all 272 objects — 93 minutes against 77. Nobody would
guess that, and it is why the box said measure. A configure script is
753 compile-and-link cycles of a ten-line program, and on this machine
each one costs 7.5 s of which the compiler is a small part; 272
translation units at 17 s each is the part everyone thinks of. **The
build that does not fit does not fit mostly because of the questions,
not the answers.**

**And the second measurement inside the first, which was backwards from
the guess.** The first probe is *cheaper* than the three after it — 4,830
ms against a 7,490 ms mean — and the split says why: user time is flat
at ~80 cs while **system time doubles**, 142 cs to 283. The obvious
story is "the first one pays for demand-paging cc1"; what actually
changes after the first run is that there is now a previous output on
the disk to overwrite and dirty blocks to write back. The compiler is
not what this machine is waiting for. That is M98's own finding
("50% of the compile is idle time... the disk") arriving from a
completely different direction, and it is the first number that would
argue for reopening M104's readahead refusal.

**Method, stated because the method is the deliverable.** Two units
measured *on the machine* by a fixture the kernel spawns under a fifth
fw_cfg switch (`opt/leanos/pybuild=1`, the same argument the third and
fourth make: a measurement, not a self-test). Two counts taken from
**real runs** rather than from the source — the check count from
`tools/configure-test.sh`'s own configure run, the object count from the
cross build — because grepping `configure` for "checking" undercounts by
a third (many checks are inside loops) and counting `*.c` overcounts (a
configured build compiles what it was configured for). The
multiplication is printed with every input beside it, and the script
says in as many words that a product of measurements is an argument
about numbers and not an observation: **this machine has never built
CPython, and the number is what says by how much.**

`tools/python-build-test.sh` is in `--full`, next to M98's bootstrap
harness, and takes about a minute against that one's twenty.

**What it did not do, and the condition.** Putting CPython's 40 MB
source tree on the image and running the real thing is now a wall-clock
decision rather than a capability one — the shell runs configure, the
compiler compiles, the disk holds a source tree (M93). It becomes worth
doing when three hours of QEMU is cheaper than the argument, which is
not today; and the honest note is that the extrapolation ignores
CPython's own generated-code steps and its two links, so it is a floor.

---

*Below this line, the snapshot as written on 2026-09-04.*

**This is where new entries go**, in full and in the archive's own form: a
status line, the boxes with their marks, what it cost, what went wrong and
what that taught, and the measurement if it took one. Strike the row
through in *The queue* the week it lands, and delete its boxes from *Every
open box*. When this section grows past a few entries, or when an arc
closes, append them to `milestones-archive.md` under a dated marker and
rewrite this file as a fresh snapshot.

---

## Every open box, in one place

The queue says what order. This says exactly what is unfinished, in the
words of the entry that left it open, so nothing has to be reconstructed
from a 15,000-line archive.

### M99 — Python, built here `[x]` (closed 2026-09-04)

Landed: the interpreter runs, its standard library is on the disk as
`.py` source, and `python3 -m test` runs CPython's own regression suite
here — **1,332 tests, 57 failures**, 3,225 s of wall clock, 18 modules.
Nineteen bugs in this OS were found on the way, including a thread stack
that had been entered misaligned since M79 and an `fstat` that refused
every descriptor which was not a file (which is why `print()` used to
write nothing and return successfully).

- [x] **Extension modules as shared objects.** Landed 2026-09-04 — see
      *Landed since this snapshot*. 58 of them, `dlopen`ed by full path
      out of `lib-dynload`, against a PIE interpreter and a shared
      `libpython`. Six defects, four of them outside the loader.
- [x] **`./configure && make` for CPython on the machine.** Both halves
      closed 2026-09-04, and neither was where the box expected. The
      *configure* half was the shell, not the clock (third increment).
      The clock is **2 h 51 min**, measured rather than estimated, and
      `./configure` is the bigger half of it (fourth increment). See
      both in *Landed since this snapshot*.

### M100 — the browser gap, measured `[ ]`

- [ ] zlib, libpng, libjpeg, freetype, harfbuzz, expat, sqlite, ICU and a
      TLS library, each unmodified, in dependency order — every one a real
      test of M94–M97
- [ ] TLS end to end over M66's TCP: the first `https://` this machine has
      had
- [ ] `O_NONBLOCK` and `AF_UNIX`/`socketpair`, absorbed here because a
      multi-process browser is the first program that needs both at once
      and for a reason. **`AF_UNIX` is also what stopped CPython's
      `test_stat` from reporting any counts at all**
- [ ] a real but small engine — NetSurf has its own layout engine and a
      framebuffer front end
- [ ] **the measurement, which is the deliverable**: what Chromium's build
      actually asks of this machine, in numbers — disk, RAM, syscalls this
      kernel does not have, what its GPU and sandbox layers assume. Not an
      estimate; a list produced by trying and reading the errors

### M103 — interrupts a real machine delivers `[~]`

Landed: I/O APIC with source overrides, PIC masked rather than deleted,
per-vector per-CPU statistics, full battery green through both
controllers.

- [ ] **MSI and MSI-X** — condition is M107's NVMe (a part with no other
      way to interrupt). **Row 6 of the queue.**
- [ ] **Per-CPU LAPIC timer as the tick source** — an unambiguous
      improvement on real hardware and an unambiguous regression on this
      one. **Held with M110**, which is the boot that decides it.
- [ ] **virtio-blk on its own interrupt, polling retired** — same reason.
      Polling reads 1 MiB in 5.6 ms; on the I/O APIC path this machine
      reads the same megabyte in 10 ms with no driver change at all.
      **Held with M110.**

### M92 — the disk `[~]`

- [~] Interrupt-driven virtio and **AHCI not attempted**, both declined
      with reasons rather than deferred vaguely. This box **collects
      inside M107** under the condition already written for it.

### M106's tail — the battery on four cores

M106 is `[x]` and honest about what it did not finish. `QEMU_CPUS`
defaults to **1** in every harness, stated in each of them rather than
left to QEMU's default, which is how this rotted in the first place.

- [ ] **M66's TCP self-test stalls** on four cores: the bulk transfer
      never finishes arriving and is then reported as corrupt. The stack
      has a proper recursive lock and every net syscall is inside it, so
      this is not an unguarded structure — it is the retransmit/window
      path meeting genuine concurrency for the first time.
- [ ] **One animation frame in six misses its budget** in the interactive
      suite.
- [ ] **An intermittent stall on the exit path**, seen once, where a
      terminated task was still current a second after being reaped.
- [ ] **CPU affinity**, deferred on the condition that the battery is
      green on four cores first. Pinning the compositor to a warm core is
      a latency optimisation of a scheduler that does not yet survive four
      cores, and there is no honest before-and-after until it does.

### M98's tail — the things the bootstrap named and did not fix

- [ ] **`tools/bootstrap-test.sh` defaults to one core because four
      panics**: profiling a four-core build fires M106's own switch-away
      guard — *"cpu 3 is switching away from 'idle' ... while standing
      on"* another task's kernel stack. It reproduces on demand with
      `QEMU_CPUS=4` on that harness, which makes it the best handle
      anyone has ever had on the row-5 work.
- [ ] **50% of the compile is idle time** on one core with nothing else
      runnable — the disk, demand-paging a 43 MB `cc1plus` and reading C++
      headers. M104 measured readahead and refused it by default; this is
      the first workload that would ask again. Recorded rather than acted
      on, because the two numbers M98 changed were the two it had measured.
- [ ] **`make -j4` on one core costs far more than four times `make -j1`**
      — 203 s serially, still running at 600 s four ways. Four concurrent
      `cc1` processes are ~480 MiB of resident set against a block cache
      then serving four files. Stated as an observation, not a
      measurement: the run was stopped rather than finished.
- [ ] **`mkfifo`.** GNU make 4.4 wants a FIFO for its job server, does not
      get one, says so, and falls back to the pipe job server — make's own
      supported path. Recorded rather than fixed; nothing else here has
      asked for a named pipe.

### Q7 — pixels `[~]`

The invariant half landed and caught its bug (the framebuffer scroll that
made the desktop jump 16 pixels whenever any window moved — klog was
painting into the framebuffer the compositor was composing into; the
screen has one owner now and after `SYS_fb_map` it is not the kernel).

- [ ] the golden-frame baselines, the diff artifact, and
      `make accept-visuals`

### Q14 — the compositor, off the machine `[ ]`

5,189 lines, none of it graded except through a booted machine. Most of
it is not graphics: hit-testing, z-order, clipping, damage regions,
snapping, cascade placement and taskbar layout are pure functions of a
window list.

- [ ] the window model compiled for the host against a guard-banded fake
      framebuffer
- [ ] hit-testing at every z-order permutation; z-order invariants as
      properties
- [ ] clipping: no draw call may write outside `clip_x0..clip_x1`, ever
- [ ] placement: the cascade, M45's bottom clamp, and session restore at a
      *different resolution* than the one that saved it
- [ ] the animation clock against a fixed time base

**How we'll know:** the M45 bottom-clamp bug and the M51 occlusion bug
both reproduced as failing host tests against reverted fixes, in
milliseconds rather than by launching seven applications.

### M107, M108, M109, M110 `[ ]`

Not started; their bullets and grading bars are written in full in the
archive (`grep -n '^### M107' milestones-archive.md`). In short: M107 is
AHCI + NVMe + xHCI/USB HID, all graded under QEMU first, with
`QEMU_DISK=ahci` and `QEMU_DISK=nvme` joining `ide` and `virtio` as
supported configurations. M108 is an `e1000e`-class NIC (driver half
gradeable here, link half held). M109 is the whole tree rebuilt on the
machine with the **generational test** — images two and three
byte-identical — as the falsifiable part. M110 is the USB boot, held.

### Smaller things left open, with where they were left

- **Q15 is five invariants, not forty-seven.** Generalising "nothing else
  changed" to every interactive test is real work: each one needs an
  allowed-region list, and each entry in such a list is a place the suite
  has stopped looking.
- **`fs/leanfs.c` is the largest unmeasured file** — 55.68% line coverage
  and no mutation score, because 1,440 mutants at ~1.3 s each is half an
  hour that has not been spent.
- **The coverage ratchet holds ten kernel files and one user-space file**,
  against 64 `.c` files under `kernel/`. There is deliberately no single
  coverage figure: the honest number is the per-file table, not an average
  over a denominator chosen to flatter it. Q13 put the scheduler under
  host tests but it has no floor row yet; Q14 is the other big move.

---

## Known divergences and hazards

Things that are true about this machine right now and will bite the next
person who assumes otherwise. Each is named rather than hidden, and each
says whether it is a bug or a decision.

**Decisions, with the reason:**

- **A thread gets a *copy* of the file-descriptor table, not a share of
  it.** POSIX requires threads to share one table. `<pthread.h>` has said
  this in its own header since M79, and it is the single largest
  divergence CPython's suite found (`test_io`, `EBADF` from a thread). It
  is a fact about `sched_vm_owner`'s scope rather than a bug in a
  function, and fixing it is a real piece of work nobody has scheduled.
- **`fork` is refused from a threaded process** (M83). Making a page
  copy-on-write clears the writable bit in one CPU's page tables and this
  kernel has no TLB shootdown, so a second thread on another core would go
  on writing to a page the child was just promised is its own. A clean
  refusal was chosen over a page that is sometimes shared. It cost
  CPython's test runner its `--timeout` flag, which starts a watchdog
  thread.
- **`SIGSEGV` cannot be blocked**, and that is not an inconsistency with
  catching it. Blocking a signal the MMU is about to raise does not
  postpone it — the faulting instruction is still there and still cannot
  execute. Linux forces the default action on a blocked synchronous
  `SIGSEGV`, which is the same outcome by a longer road.
- **`sin`, `cos` and `tan` refuse above 2^52** rather than answering. The
  quadrant reduction divides by π/2 and rounds to a `long long`; a correct
  answer past that needs Payne–Hanek reduction against a thousand bits of
  π. `tools/math-test.sh` requires a declared divergence to be a
  *refusal*: a "known divergence" that returns a number is a wrong answer
  with a note beside it.
- **`chmod` fails truthfully** rather than returning 0. One principal, and
  the machine says so.
- **The third argument to a `SA_SIGINFO` handler is `NULL`** where every
  other Unix passes a `ucontext_t *`. There is no ucontext here, and a
  pointer to something invented would be worse than a null one a program
  faults on immediately.
- **Readahead is off by default** and the I/O APIC is not the default —
  both because the measurement said so, and both with the row in
  `budgets.tsv` that will say when that stops being true.

**Known gaps, unfixed:**

- **A signal frame carries no FPU state.** A handler that uses SSE
  clobbers the interrupted code's registers. Survivable while signals
  arrived from `kill`; more visible now they arrive from the instruction
  that is executing. The condition that changes it: a program that catches
  a fault and *returns* rather than unwinding.
- **`PTHREAD_KEYS_MAX` is 32**, and CPython's `test_threading` runs out
  past it (`gilstate_tss_set: failed to set current tstate`).
- **`AF_UNIX` does not exist.** Scheduled: M100's third bullet.
- **At least one more libc call fails without setting `errno`** — CPython's
  `test_json` reports `OSError: [Errno 0] Error`. Five were found and
  fixed in M99; the suite says there is a sixth.
- **`EIO` is this libc's "the read failed and this ABI carries no reason
  out" answer.** It is vague honestly, and the fix for the vagueness is a
  return convention rather than a better guess.
- **No `.pyc` cache**, which is why `python_fixture_ms` has a ~5x ceiling
  instead of the usual 2x. Re-measure and tighten it the day one exists.
- **The full self-test battery is not green on four cores** — three named
  failures, above.

---

## Deferred, with the condition each one names

Check this list before proposing any of it. A deferral here is a
*scheduled question*, not a refusal, and the question is answered by the
condition rather than by an opinion.

- **A GPU driver, or real mode-setting.** A driver per vendor per
  generation, and not a thing this project will do. M58's Display pane
  showing only the firmware's mode on real hardware is the honest outcome.
  Unchanged by M100 — software rasterization into this compositor's
  framebuffer is the answer, and it is a slow answer rather than a missing
  one — and unchanged by M107, which is worth saying because it looks
  closer: AHCI, NVMe and xHCI are published specifications with one
  implementation each, a category a GPU has never been in.
- **A browser.** A named goal, not a next step, and the estimate has not
  moved: HTML, CSS, layout, a JS runtime, TLS, GPU compositing, codecs, a
  sandbox, and a Linux-scale syscall surface. **M100 measures the gap
  rather than porting one**, which is the opposite of the request.
- **Multi-user, logins, uids.** Becomes real if and when two people share
  a machine, and not before. Unchanged by a machine that compiles its own
  kernel — which is exactly where the temptation shows up, and why it is
  restated here.
- **A journalling filesystem — refused three times, and the condition has
  changed rather than the number.** M105 took both of M71's clauses
  together and both said no: four concurrent writers left the scan finding
  no orphan, no double-allocation and every free block back, and the cold
  scan does not move with the file count. (It does move with allocated
  *bytes* — 711 ms now, and that row is the closest thing to the second
  condition this file has.) The reason the first clause cost
  nothing is that "multiple writers" was never the dangerous thing —
  **interleaved metadata sequences** are, and M67's one coarse `fs_lock`
  already makes a sequence atomic against another writer, for a reason
  that had nothing to do with crashes. **The restated condition, which is
  now about this project's own future work: a journal becomes worth
  building when a metadata sequence stops being atomic against another
  writer** — when `fs_lock` is split finer for throughput (rows 5 and 9 of
  the queue), or when the inode table stops being resident and the scan
  becomes disk-bound. Either alone is enough. *The first person to make
  this filesystem's locking finer-grained is the person who has to build
  the journal.*
- **Swap.** Decided on a number and refused: the largest compile peaks at
  217 MiB. Reopens when a single compile no longer fits the small machine
  — `build_cxx_tu_peak_rss_kib`'s ceiling is where that fires.
- **`syscall`/`sysret`.** Collected as a measurement: one `int 0x80` round
  trip is **1,300 cycles**, now a budget row. It becomes work when an
  attribution says that cost owns a workload's wall clock. M98's profile
  said 72% of a compile was inside `malloc` and 89% of its syscalls were
  `write` — both were fixed, and neither was the trap path.
- **Window scaling, SACK and Nagle.** Kept, on M108's terms: **if and only
  if M100's TLS fetch asks.** A 64 KiB window on a link with 30 ms of round
  trip is a number, and the number decides.
- **CPU affinity.** Condition: the battery green on four cores.
- **A third-party libc.** The trigger is symbol versioning, not volume.
  M100's gap analysis is where that decision gets handed over, and nothing
  should front-run it.
- **A second architecture.** `x86_64-lean_os` is a triple with one machine
  behind it, and it stays that way until something asks.
- **A property-based testing library.** Becomes worth it when shrinking a
  failing case by hand is the bottleneck, which it is not.
- **A coverage target.** Refused in Q8 and still refused. The ratchet
  constrains direction only.

---

## Lessons that keep being relearned

Compressed from the archive, because these cost real time and every one
of them recurred in a form nobody recognised the second time.

- **A header that has a function and does not declare it where the
  standard says is, to a build, indistinguishable from not having it.**
  Three of the first four things that stopped binutils were functions this
  libc had already had for ten milestones. This rule has now been paid for
  at four separate addresses.
- **A configure probe that fails to compile writes a *number*, not an
  error.** `SIZEOF_TIME_T 0` stopped a build 400 files later at an
  `#error`. And configure caches its answers: a fix applied after a
  configure run is a fix that has not been applied.
- **An anchored-edit port is only idempotent while the edit does not
  change.** Re-applying an edit that could not find its own replacement
  text duplicated a shell `case` arm; the first arm won, nothing warned,
  and the symptom half an hour later was an unrelated missing flag. Both
  build scripts stamp the unpacked tree with a hash of the port now.
- **A range sweep asks whether the answer is right in the middle. The
  edges are where a range reduction, a cast, or a loop bound stops being
  valid.** `tools/math-test.sh` had no edge cases until CPython's own
  suite hung on `acosh(INF)`; adding ±inf, nan, ±0, ±1, the smallest
  subnormal and the largest finite double found three more hangs
  immediately.
- **A test that asserts an absence constrains almost nothing.** Dropping a
  bad packet is one bit of behaviour; the several hundred bits that matter
  are in the reply. That is why `net/ethernet.c` sat at 100% line coverage
  and a 0% mutation score.
- **A fixture that asserts what the implementation happens to do is
  exactly what a differential test exists to outrank.** Two of the M63
  fixture's expectations were the old printf's answers written down as
  law; both moved when the host oracle disagreed.
- **Building an instrument before its subject costs a box.** M101 and M102
  each shipped with a measurement they could not take, waiting on M98.
  That is not a reason never to do it — Q7 and Q14 are the counter-case,
  because they grade code that already exists — but it is a reason to ask
  which kind you are doing.
- **A performance fix in a table that three syscalls do surgery on must be
  tested against all three.** A coalescing merge tested through the
  syscall it was written for hung `mprotect` forever, and the 300-mapping
  test added for it did not catch it because it never called `mprotect`.
- **A comment that justifies a constraint by naming a thing outside the
  tree goes stale the day that thing leaves.** Several justified
  themselves by a CI runner that had never once run.
- **The four instruments each reach something the others cannot**, and the
  proof is on the record: three bugs in M98's second increment were found
  only by the boot marker, one only by the host tier, one only by a boot
  battery hang, and nineteen in M99 only by a test suite nobody here
  wrote.

---

## The loop

Work is **milestone-driven**: one milestone at a time, in the order *The
queue* sets, taken to completion before the next one starts. Don't run
ahead, and don't leave partial work spread across several milestones.

1. **Read the entry first** — here for state, in the archive for what an
   earlier attempt got wrong.
2. **Build it**, respecting the non-negotiables in `CLAUDE.md`. No
   third-party code in the OS. No third-party boot code. Freestanding C
   plus NASM. Plain Makefiles.
3. **Add tests for it.** New code arrives with new tests; a milestone
   whose tests are all pre-existing isn't finished. Pick the instrument
   that actually grades the thing.
4. **Run them.** `--fast` while iterating, the default tier before
   committing, `--full` when the milestone touches input or the slow
   paths. `make mutate` over new tests whose passing could be vacuous.
   Nothing red gets committed.
5. **Update this file** — status, what it cost, what went wrong, what that
   taught. That is part of the milestone, not bookkeeping after it.
6. **Commit** as `M<n>: <what changed>`, on `main`. Never push.

### What to write where

- **This file** carries state, open boxes, conditions, numbers and the
  queue. It should stay something an agent can read in full at the start
  of a session.
- **The archive** carries finished history. Do not edit it except to
  correct a factual error.
- **When this file gets long again** — say, past a few thousand lines, or
  when a whole arc has closed — do what was done on 2026-09-04: append the
  completed entries to `milestones-archive.md` with a dated marker, and
  rewrite this file as a fresh snapshot. The archive is append-only
  history; this file is the working set.
- **New milestone entries go in *Landed since this snapshot***, in full,
  and move to the archive when the arc closes.

---

## Archive index

Every heading in [milestones-archive.md](milestones-archive.md), with the
status it had when it was frozen. Grep for the heading text to find the
entry: `grep -n '^### M85' milestones-archive.md`.

**Foundations (M0–M14)** — all `[x]`: scaffolding & toolchain · boot
sector · protected→long mode · freestanding kernel · CPU fundamentals ·
memory management · timer & core drivers · multitasking · syscall
interface · user mode & process loading · user-space runtime · first user
program · storage & filesystem · init & shell · IPC & process management.

**Path to a desktop environment (M15–M28)** — all `[x]` except M28:
filesystem headroom · linear framebuffer · framebuffer text console ·
PS/2 mouse · user-space heap & shm · **windowing compositor** · UI toolkit
& input routing · desktop shell · desktop icon + GUI terminal · **UEFI
boot path** · third-party build tooling · UEFI-only (BIOS removed) ·
**networking stack + NIC** · `[~]` M28 real hardware (USB boot).

**Path to a usable OS (M29–M33)** — all `[x]`: robustness & cleanup ·
window chrome · dragging & resizing · desktop & input polish · core app
baseline.

**Polished desktop UI (M34–M39)** — all `[x]`: widget primitives · menus ·
dialogs & the file save flow · text selection & scrollbars · visual chrome
· sharper system text.

**Windows/macOS hybrid (M40–M44)** — all `[x]`: **robustness pass + the
interactive-input harness** · top menu bar · bottom taskbar · launcher &
snapping · desktop visual polish.

**Daily-usable desktop (M45–M50)** — all `[x]`: process control & Force
Quit · window chrome details · **session lifecycle & persistent settings**
· notifications and no silent failures · scroll/chords/drag-and-drop ·
robustness & resource hygiene.

**Dependable desktop (M51–M56)** — all `[x]`: **z-order and hit-testing** ·
**kernel hardening, no user-triggerable panic** · **directories in leanfs**
· reclaiming what dies · surviving a compositor crash · depth where people
spend time.

**A desktop someone would choose (M57–M66)** — all `[x]`: type that isn't
8x16 · live resolution change · files without limits & a real date · the
two apps people live in · **motion and a frame budget** · the first sound ·
**M63 somebody else's program (Whetstone)** · a network user space can
reach · **M65 a permission model worth having** · **M66 TCP**.

**A kernel underneath (M67–M74)** — all `[x]`: **a kernel that can be
interrupted** · wait queues · **M69 latency you can feel (where
measure-first was set)** · a machine that says what happened · **M71 files
worth trusting** · one scriptable shell · names not numbers · the session
that remembers.

**Unix-shaped (M75–M80)** — all `[x]` except M80: environment & cwd · **a signal a program can
catch** · POSIX names · `mmap`/`munmap` · **two threads, one address
space** · `[⊘]` M80 somebody else's language (superseded by M99).

**The things underneath the names (M81–M89)** — all `[x]`: a filesystem
that can hold a program · demand paging · **fork** · **execve** · **M85 a
terminal that is a device (PTYs)** · **M86 a shell that is a shell** ·
types, places and hard links · everything else a ported program calls ·
**M89 somebody else's userland (toybox)**.

**Big enough to build on (M90–M100)** — `[x]` unless marked: more than a gigabyte · an address
space that is a set of mappings · `[~]` **M92 the disk and its cache** ·
**M93 a filesystem that can hold a source tree** · **M94 a target this
compiler knows by name** · **M95 code that is loaded, not linked** · TLS,
futexes, real pthreads · **M97 C++** · **M98 a compiler that runs here** ·
`[~]` **M99 Python, built here** · `[ ]` M100 the browser gap.

**The other half of the goal sentence (M101–M110)** — `[x]` unless marked: where the time goes ·
memory that runs out honestly · `[~]` **M103 interrupts a real machine
delivers** · **M104 writeback** · **M105 the journal, refused a third
time** · **M106 cores a build can use** · `[ ]` M107 devices · `[ ]` M108
a real NIC · `[ ]` M109 lean_os on lean_os · `[ ]` M110 the boot that has
never happened.

**Tests that can fail (Q1–Q10)** — all `[x]` except Q7: a test command ·
the first unit test · leanfs against a RAM disk · the stack fed garbage ·
**Q5 every syscall told a lie** · numbers that fail · `[~]` **Q7 pixels** ·
what the tests never touch · **Q9 exhaustion** · Q10 CI (withdrawn — see
*Testing is local* in the archive).

**Tests worth trusting (Q11–Q20)** — all `[x]` except Q14: the leftovers ·
**Q12 mutation testing** · **Q13 the scheduler off the machine** · `[ ]`
**Q14 the compositor off the machine** · **Q15 nothing else changed** ·
**Q16 devices that fail** · **Q17 power cut** · **Q18 latency as a
distribution** · **Q19 boot once, test many** · Q20 the tests as a product.

**Sections in the archive that are not milestones**, and are worth reading
on their own: *Ground rules / assumptions* · *Known gaps after M50* and
*after M56* · *The arcs* · *Cleanup pass* · *Before M94: the five things
this arc named and did not schedule* · *What landing Q1–Q10 actually
found* · *What landing Q11–Q20 actually found* · *Deliberately not next,
and why* · *Testing is local* · *The next ten, and where they actually
start* · *The next ten, asked a second time*.
