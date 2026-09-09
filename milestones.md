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
| **Milestones** | M0–M111 numbered: 102 `[x]`, 4 `[~]` (M28, M92, M99, M103), 1 `[⊘]` (M80), 4 not started (M107–M110) |
| **Testing arc** | Q1–Q20 written, 18 `[x]`; Q7 half landed, Q14 not started |
| **Head of the queue** | **M100** — eight of nine libraries landed; M111 was taken out of order at the user's request and closed 2026-09-09 |
| **Held by instruction** | all real-hardware work: M110, M28's last box, M108's link half, M103's two hardware-conditioned boxes |
| **Host unit tests** | 311/311 passing, 3 slow ones skipped in `--fast` |
| **Boot markers** | 122 required, graded on every self-test boot |
| **Performance budgets** | 39 rows in `tests/budgets.tsv`, all inside their ceilings |
| **Source** | ~50k lines kernel, ~50k user space, ~3.3k system_api, ~9.5k tests |
| **Working tree** | clean; nothing half-landed anywhere |

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
  purpose. Four cores boot and share work: four equal tasks cost
  **92–180%** of what one task costs across eleven runs, where 100% is
  perfect scaling and 400% would be one core doing all of it. **That
  range is a correction.** It was recorded as 114–200% and the old figure
  came from a statistic that was wrong: the kernel took the minimum
  one-task time and the minimum N-task time from *different rounds* and
  divided, which is the ratio of two moments that never happened
  together and is biased upward without limit. It produced readings as
  high as **491%** — more than one core doing all the work, which four
  cores cannot do, and that impossibility is the tell. M100 pairs the two
  measurements per round and takes the best ratio; the honest number is
  **closer to perfect scaling than this file has ever claimed.**
  `tools/smp-test.sh` grades it in the default tier and used to fail
  about one run in six because of the old statistic.

  **Correction, measured 2026-09-09 during M111 and not caused by it:
  `tools/smp-test.sh` fails about two runs in three**, at HEAD
  (`c11d3f3`), with

      *** KERNEL PANIC: sched: this CPU is not on the stack of the task
          it thinks it is running ***

  Six runs on a clean HEAD build: **two passed, four panicked**. Five
  runs of the M111 tree: three passed, two panicked. So it is neither
  new nor M111's, and the sentence this paragraph used to end with -
  "twelve runs of the new one have not failed" - describes a machine
  this one no longer is. The measurement above (92-180%) still stands;
  it comes from the runs that reach it.

  **Two symptoms, one problem.** Most failures are that panic in about
  21 seconds; one run in the final M111 tier instead **hung** and was
  killed at the stage's own 240-second ceiling with `[smp] self-test
  passed` never printed. A panic and a hang from the same stage is
  exactly Q13's description of this class - "about one boot in ten
  hangs" - and it is why the scheduler was moved off the machine in the
  first place.

  This is the M106 tail (queue row 5) presenting from a second
  direction, and it is now the cheapest reproduction of it this project
  has: 21 seconds per attempt, two attempts in three. **Whoever takes
  row 5 should start here** rather than with the full battery. It was
  found by attributing an M111 test failure rather than by looking for
  it, which is the third time in this file an unrelated milestone's
  bisect has been what found a scheduler bug. The full battery is *not* green on four cores;
  see *Known divergences*.
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
  the measurement above: four tasks of equal work came in as low as
  **92%** of one task's time, so that lock is not what stops this machine
  using its cores. M100's correction to the statistic strengthened this
  conclusion rather than weakening it — the refusal was made on a number
  that was too *pessimistic*.
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
| full unclean-mount scan of the boot filesystem | 1,550 ms | — |

The mount scan is the row to watch, and **M100's second increment
falsified what this file used to say about it.** It was recorded as
"linear in allocated bytes, not in file count"; 3.5 MiB of libpng and
libjpeg — 1.4% more bytes — moved it 904 ms → 1,714 ms, which is not
linear in anything. The scan now reports what it does as well as how long
it takes, and what it does is **79,512 block reads of which 311 miss the
cache**: it is memcpy-bound in the block cache, not disk-bound, because
`map_block` re-reads a whole 4 KiB indirect table for every logical block
of every file. Walking 251 MiB of image copies about 320 MiB. That is the
handle if this ever needs to be faster. Two things about it stay open: the
90% jump is reproducible but unaccounted for, and `mount_scan_busy_us`
runs the identical walk — same three counters — for half the time, every
boot. See M100's second increment.

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
`netconf`. **TLS since M100** (mbedtls 3.6.2, ported against this system): the first `https://` this machine has had, over blocking sockets and a real entropy source.

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

### Packages (M111)

`os install grep` puts GNU grep 3.11 on this machine, built here from the
published tarball with no source edit. A package is a `.osp` archive —
manifest, file table, payload — with **no install hooks**: installing is
verify and copy. `tools/os-pkg.c` builds them on the host,
`/bin/os` installs them, and both are `user_space/lib/ospkg.c`.

| | |
|---|---|
| repository | `/pkg/repo/*.osp` + an index carrying each archive's SHA-256 |
| installed | `/pkg/<name>/<version>/`, commands linked from `/pkg/bin`, never `/bin` |
| integrity | three SHA-256s: index→archive, header→body, record→each file |
| capabilities | the shipped grant table **does not apply under `/pkg`**; the mask comes from `/pkg/db/caps` and is intersected with `CAP_PKG_MAX` (`fs-write \| network \| audio`) in the kernel; unlisted means **zero** |
| the gate | `CAP_PKG_ADMIN` on nine write syscalls whose normalized path is under `/pkg`; `/bin/os` is the only holder |
| in the repo | grep 3.11, bzip2 1.0.8, and `impostor` — a fixture that installs binaries called `compositor` and `shutdown` and must get nothing |

No signatures, and the deferred list says under what condition that
changes. Not a sandbox: a package granted `fs-write` can write anywhere
but `/pkg`. See [docs/packages.md](docs/packages.md).

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

**The commit tier is the gate.** M100's second increment ran `--full`
and found four red stages, none of them caused by it and every one
checked rather than assumed. The third increment fixed three of them:
the coverage ratchet's two broken floors were **restored by writing the
tests that were missing** rather than lowered, `tools/bootstrap-test.sh`
now bounds the `make -j4` step it used to hang in and **passes for the
first time**, and `mount_scan_busy_us` was re-ceilinged from the
measurement. What is left red is the noise, and it is named: the
four-core stage fails about one run in six on this host and the
`[libctest]` clock check on the I/O APIC path is flaky, both on this
kernel and on the one before it. Both have boxes. Read them before
spending an evening on a `--full` run, which is what the second
increment cost.

| instrument | what it grades | how it is run |
|---|---|---|
| **Host unit tests** (`tests/`) | libk, heap, malloc, leanfs, every network parser, the TCP state machine, PTY, symtab, UTF-8, fnmatch, getopt, fwcfg — and the **scheduler** (Q13), 2,157 lines against a fake timer and a fake CPU, with lock-order inversions made errors rather than comments. and, since M100, the **record-lock table** (`kernel/fs/flock.c`, mutation score 91.5%). **268 tests**, ASan+UBSan, under a second | `--fast` |
| **Boot self-tests** | **115 required markers** and 36 performance budgets, graded off the serial log of a real boot. The switch comes from outside the image via fw_cfg, so the image is byte-identical with or without them | `tools/qemu-serial-test.sh` |
| **Input suite** | real clicks and keys through QEMU's monitor, graded on real framebuffer pixels. Most tests check something *did* change; two check that nothing else did, which is the only way to catch a flicker (Q7/Q15). Boots once per image and restores a snapshot per test, keyed on the image hash so a stale one fails closed (Q19) | `tools/qemu-input-test.sh` |
| **Differential tests** | `sh`, the regex engine, `sscanf`, `printf`, libm, `realpath`, and the FILE layer — each compiled for the host from the same source the machine runs, put beside the host's own, and required to agree. Nothing in the fixtures says what the right answer is. **Since M100 the same shape grades three ported libraries**: freetype's rasterizer, sqlite's shell and harfbuzz's shaper, each built for the host from the same tarball and required to produce byte-identical output on the machine (`[m100c]`, `[m100d]`, `[m100e]`) | `--fast`; the library halves on the graded boot |
| **Fuzzers** | network parsers and the mount path, ~150k inputs/second | `make fuzz-run` |
| **Mutation harness** | breaks the kernel on purpose and reports whether the tests noticed. The only instrument that grades the *tests* | `make mutate` |
| **Crash test** | SIGKILL mid-write, reboot, verify with an independent reader. 16 cuts | `tools/crash-test.sh` |
| **Disk-fault test** | QEMU `blkdebug` refusing **every** write, through virtio and through ATA, and the machine must still reach PID 1 | `tools/disk-fault-test.sh` |
| **Image-tree test** | a host tool writes a tree into a leanfs image, an independent reader compares it to the source directory, then the machine walks and hashes it back from inside | `tools/image-tree-test.sh` |
| **Exhaustion** (`/bin/exhausttest`) | descriptors, pipes, shm segments and sockets to their ceilings — refuse, recover, work again, twice, with a leak audit across 2,200 rounds | `[q9]` boot marker |
| **SMP test** | four cores boot, each recognises itself, four equal tasks share them | default tier |
| **Bootstrap test** | this machine's own gcc/as/ar/ld building bzip2, and bzip2's own test suite on the result | `--full`, own fw_cfg switch |
| **Python test** | CPython's own regression suite, on the machine, reporting **its own** counts. The only instrument here that neither wrote its own assertions nor chose what to assert | `tools/python-test.sh` |
| **Coverage ratchet** | **20 rows** in `tests/coverage-floor.tsv`; coverage may not go **down**. There is no coverage *target* and there will not be one. Since M100 the floors are the measured number rather than a round number under it, and a file in the report with **no row is an error** — both because fifteen lines of standing advice on every run are what hid two floors that had actually fallen | `make coverage-check` |

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
| **2** | **M100** — the browser gap, measured | in progress: eight of nine libraries landed; TLS done | the last milestone of its arc and the one that specifies the arc after it. **TLS lives here**, which is also the fetch M108's three TCP deferrals are conditioned on, and `AF_UNIX`/`socketpair`/`O_NONBLOCK` are absorbed here |
| **3** | **Q7 (2nd)** — the golden-frame baselines | half landed | the invariant half exists and has caught its bug; the baselines, the diff artifact and `make accept-visuals` do not |
| **4** | **Q14** — the compositor, off the machine | not started | the move Q13 made on the scheduler, on a 5,189-line file — taken *before* M107 puts USB input underneath it, so the rewiring is graded in milliseconds rather than only through a screendump |
| **5** | **M106 (tail)** — the battery green on four cores | 3 known failures, **plus `smp-test.sh` itself failing 2 runs in 3** (measured 2026-09-09 at HEAD — see *The machine as it stands*) | not a new milestone: the three failures M106 named and left. It gates CPU affinity, and there are now two reproductions on demand — M98's bootstrap profiler, and a 21-second `tools/smp-test.sh` that panics with "this CPU is not on the stack of the task it thinks it is running" about two attempts in three. **Start with the second one** |
| **6** | **M103 (2nd)** — MSI/MSI-X | condition fires at row 7 | its own condition is *"a driver for a part that has no other way to interrupt"*, and M107's NVMe bullet answers it by name. The LAPIC-timer and interrupt-driven-virtio boxes are **not** collected here — they wait for hardware |
| **7** | **M107** — AHCI, NVMe, xHCI + USB HID | not started | all three gradeable under QEMU at this desk, which is what makes M110 a boot rather than a bring-up. Collects M92's last open box on the way |
| **8** | **M108 (driver half)** — a real NIC | not started | the `e1000e` driver is QEMU-gradeable and belongs here. The link half is held — see below |
| **9** | **M109** — lean_os built on lean_os | not started | needs nothing from rows 7–8 and stays after them anyway: the tree it rebuilds should be the whole tree, drivers included, or the generational test grades a subset of the machine it runs on |
| **10** | **M110** — the boot that has never happened | **held** | see the hold |

**M111 is not in this table and that is recorded rather than hidden.**
`os`, the package manager, was asked for directly on 2026-09-09 and built
to completion in one pass — an instruction outranks the queue, the same
way the hardware hold does. It jumped nothing: no row above was started
and abandoned, and every row below is where it was. What it changed for
the rows that remain is one thing worth knowing: **M100's TLS now has a
second customer waiting**, because `os install` fetching from a machine
this one did not build is the milestone that makes package signing a real
check rather than decoration — see the deferred list.

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

### M100 (first increment) — the first library of the stack `[~]`

*Landed 2026-09-04.* M100's first bullet is nine libraries in dependency
order, each unmodified, "every one a real test of M94 through M97".
**zlib 1.3.1 is the first**, built by this project's own compiler with no
edit to anything — the first port in this tree that needed no edit at
all, `config.sub` included, because zlib does not have one — and graded
by the `[m100]` boot marker running **zlib's own `example` program** on
the machine: compress/uncompress, deflate/inflate whole and in chunks, a
preset dictionary, and the gz file layer opening, seeking and reading a
real file, plus a `minigzip` round trip through this filesystem.

Nothing here chose what that program checks. It is the argument M99 made
for CPython's regression suite, applied one layer down to a library.

**It is a library port, and that is a different shape from every port
before it.** bzip2, GNU hello, toybox and CPython are programs: they are
built and they are run. zlib has to be *installed into the sysroot* -
`libz.a`, `libz.so`, `zlib.h`, `zconf.h` - because libpng, freetype and
the rest of the list are each one `-lz` away from it, and a header a
build cannot find is indistinguishable from a library that does not
exist.

**Two facts about this target, found by the first library and recorded
rather than fixed** — which is what M100 is *for*:

- **`fseeko` is missing.** zlib's configure probes for it, does not find
  it, defines `NO_FSEEKO` and carries on with a 32-bit offset in its
  `gz*` layer. A libc gap with a name. Left standing on M63's rule: the
  next library that actually needs it is the one that should pay for it,
  and every gap this project has closed was named by a build rather than
  by a checklist.
- **`attempted static link of dynamic object`.** zlib builds a second
  copy of its two test programs against `libz.so` with no `-pie`,
  because **on every other ELF system the default link is dynamic and
  here it is static.** Those two targets cannot link. Left standing
  deliberately: a static default is the right one for a machine whose
  kernel loads `ET_EXEC` directly, and this is precisely the kind of
  assumption M100 exists to find and count rather than to absorb. The
  day a port needs the other answer, it will say so with a link error
  naming it.

**Next in the order:** libpng and libjpeg, both of which link `-lz` and
neither of which has been tried.

### M100 (second increment) — two libraries, and a budget row that was measuring the wrong thing `[~]`

*Landed 2026-09-05.* The second and third entries in M100's dependency
order: **libpng 1.6.44 and libjpeg 9f**, both built by this project's own
compiler with no edit to either source, both installed into the sysroot,
and both graded by **their own test suites running on this machine**.
libpng is the first library in the stack that links the one before it —
`./configure` prints `checking for zlibVersion in -lz... yes` and will not
proceed without it, which is what makes "dependency order" a fact rather
than a way of listing things.

**Neither port needed anything.** One line in each bundled `config.sub`,
through the same anchored edit GNU hello takes, and then
`./configure --host=x86_64-lean_os && make` ran to completion on both.
Three libraries in and the count of source edits is still zero.

**What the grading is, and why it is sharper than [m100]'s.** zlib's own
`example` checks its answers against itself: compress, uncompress, assert
you got back what you put in. That cannot catch a codec which is
self-consistently wrong. **libjpeg ships reference output.** `testimg.ppm`
is what the IJG's decoder produced from `testorig.jpg` in 1995;
`testimg.jpg` is what their encoder produced from that PPM. Their
`check-local` target runs seven commands and requires seven byte-exact
comparisons, and the new `[m100b]` boot marker runs exactly those seven
commands here, with toybox's `cmp` deciding:

| | |
|---|---|
| `testorig.jpg` → PPM, GIF, 256-colour BMP | three files, byte-identical |
| `testimg.ppm` → baseline JPEG | byte-identical with theirs |
| progressive JPEG → PPM, and PPM → progressive JPEG | byte-identical both ways |
| `jpegtran` progressive → baseline | **the 1995 original back, byte for byte** |

Byte equality out of a DCT is much harder to pass than it sounds: it
grades this compiler's integer arithmetic and this libc's memory over
100 KB of pixels against an answer computed on somebody else's machine
thirty years ago. A decoder one bit wrong anywhere fails it; "the picture
looks right" cannot. libpng's half is its own `pngtest --strict`, which
writes a PNG onto this filesystem, reads it back and compares chunk by
chunk.

**It passed on the first boot, and the falsification is on the record
rather than assumed:** a corrupted `testimg.jpg` on the image makes the
marker fail naming the exact comparison
(`missing: their PPM re-encoded to the byte-identical baseline JPEG`)
and panics the boot. A self-test that has never been seen to fail is a
self-test nobody has graded.

**And the library stack now survives `make sysroot`.** zlib landed by
copying four files into the sysroot after `make sysroot`'s own
`rm -rf`, which worked because nothing came after it. `tools/gcc-test.sh`
runs `make sysroot` on every invocation, so the second library in the
order would have deleted the first between two runs of the same script.
Each library is now installed by **its own `make install`** into
`build/thirdparty-sysroot`, and the `sysroot` target copies that tree
over itself at the end. The generated half stays generated; the ported
half is a record of what was installed; neither is a file list kept by
hand. `libpng-config` and the `.pc` files come along for free, which the
next two libraries will look for.

#### What the two of them found about this target

Recorded rather than fixed, which is what M100 is for.

- **libtool does not know this OS**, so every autotools library here is
  **static-only** — silently. Both configures print `checking if libtool
  supports shared libraries... no`, `--enable-shared` is accepted and
  ignored, and nothing anywhere is an error. M97 taught the *toolchain's
  own bundled* libtool about this target with an anchored edit; a project
  that ships its own generated `configure` brings its own copy and knows
  nothing. This is the largest single assumption the stack has made so
  far. Left standing because what makes it worth fixing is a library that
  must be shared, and none of the first three is.
- **`feenableexcept` is missing** — libpng probes for it and does without.
- `fseeko` is still missing (zlib found it first; libpng did not ask).

#### The finding that cost the most: `mount_scan_us` was not measuring the scan

3.5 MiB of libpng and libjpeg went onto the image — **1.4% more allocated
bytes, 247.5 MiB to 251.0 MiB** — and `mount_scan_us` went **904 ms to
1,714 ms**. Ninety per cent more time for one and a half per cent more
data, against a row whose own note says the cost is *"linear in allocated
bytes"*. Both numbers are from freshly built images, same procedure, and
both reproduce.

So the scan grew a way of saying what it did, and that is the part worth
keeping. It now reports, beside the microseconds, the block reads it
issued, how many missed the cache, and how many sectors reached the
device:

```
[m105] the quiet scan: 79512 block reads, 311 of them missed the cache,
       2936 sector reads issued to the device.
       The busy scan: 79512 / 311 / 2936
```

**This scan is not disk-bound and never was.** 311 of 79,512 reads touch
the disk. The other 79,201 are 4 KiB memcpys out of the block cache, and
at the ~18 us a cached `blk_read` costs on this machine they *are* the
measurement — 79,512 × 18 us is 1.46 s, which is `mount_scan_us` to
within 3%. `map_block` re-reads a whole indirect table for every logical
block of every file, so walking 251 MiB of image copies about 320 MiB.
That is the thing to fix if anyone ever wants this faster, and it is now
a number rather than a suspicion.

**A second scan, immediately, cold both times** is a new budget row
(`mount_scan_repeat_us`) and it earned its place by *falsifying* the
obvious explanation. If the first scan of a boot were paying for the host
serving the image file cold, the second would be fast. It is not: five
back-to-back repeats all land within 5% of the first, and an image
freshly `cp`'d to a new file on the host measures the same. Also ruled
out: a process having run (a scan after spawning `/bin/hello` is still
slow).

**Ceiling re-set** from six measurements spanning 1,416–1,716 ms, with the
usual ~2x headroom, and the reason written into the row.

#### Reproduced, and not explained — but narrowed by one boot

**`mount_scan_busy_us` runs the same walk with the same three counters and
costs half** — 780 ms against 1,550 — on every boot through the 8259. A
synthetic fixed-work probe (20,000 cached `blk_read`s) measures **368 ms
before the four writers run and 367 ms after**, so the machine is not what
changes.

The `--full` tier's **I/O APIC boot narrows it, and inverts which of the
two looks suspicious.** Through the I/O APIC the pair is *equal* — 1.65 s
quiet against 1.52 s busy — while through the 8259 it is 1.53 s against
0.78 s:

| | quiet | busy |
|---|---|---|
| 8259 (the default path) | 1.53 s | **0.78 s** |
| I/O APIC | 1.65 s | 1.52 s |

So the **quiet scan is the stable number across both interrupt
controllers, and the busy one is not.** Whatever the effect is, it makes
the second scan *faster* under one controller and does nothing under the
other — which is the opposite of what the pair was set up to show, and is
a much better-shaped question than the one this increment started with.
Still not explained; handed to whoever picks up M103 or M105 next, with
the numbers rather than the adjective.

`mount_scan_busy_us` was raised to the same 3 s ceiling, and the control
run says why it had to be: through the I/O APIC it measures **1,517,430 us
on `cdbca6e`'s kernel with this image** — already over its old ceiling
before this increment touched anything.

#### What the `--full` tier is red on, and none of it is this increment

The commit tier is green — 112/112 markers, every budget inside its
ceiling. The full tier has **four red stages, and none of them is this
work** — each was checked against `cdbca6e`'s tree or excluded by
construction rather than assumed, because a milestone that reports
somebody else's failures as its own is worse than one that reports none.
Two of them are worth more than that: **the full tier has not been green
for some time, and the snapshot below still says it is.** That sentence
is now corrected.

| stage | what it says | checked how |
|---|---|---|
| **coverage, and the ratchet** | `kernel/dev/fwcfg.c` 89.55% against a 97% floor, `kernel/mm/heap.c` 97.37% against 100% — both floors *set at `cdbca6e`* | stashed this work and ran `make coverage-check`: **byte-for-byte the same two files at the same two percentages.** Pre-existing, and it means the previous commit set two floors it does not meet |
| **the same battery, through the I/O APIC** | `[libctest] the clock is answering but not advancing`, then a panic at `[m63]` | `[libctest]` runs at log line 868 and the first line of code this increment adds runs at 1060, so it **cannot** be this. Three further I/O APIC boots on this tree passed it. Flaky, on a path M103 already records as 2x slower |
| **`mount_scan_busy_us` over budget** | on the I/O APIC path only | `cdbca6e`'s kernel measures **1,517,430 us** with the same image — over the old ceiling already. Raised, with the reason in the row |
| **the toolchain, building somebody else's program here** | hit its 3,600 s ceiling, twice | **this stage has never passed**, and the evidence is in two files nobody had put side by side. The archive's own M98 table says `make -j4` *"did not finish inside a 40-minute ceiling"*; `tools/bootstrap-test.sh` **requires** the `[measure] bzip2-j4` line and the three `[perf]` rows the kernel prints only after that step returns; and `tests/budgets.tsv` has **no row for any of them** — `build_wall_s`, `build_peak_live_tasks` and `build_peak_fds_one_task` have never once been recorded. A harness that demands four outputs no run has ever produced is a harness that has always been red |

#### And the flake that took the longest to clear

One run of the default tier failed at `tools/smp-test.sh`. Twelve
standalone runs, six on this kernel and six on `cdbca6e`'s with the same
image, say what it is:

| kernel | `smp_parallel_cost_pct` observed | failures |
|---|---|---|
| this one | 244, —, 247, 207, 211, 339 | 2 of 6 |
| `cdbca6e` | 118, 313, 137, 153, 243, 188 | 1 of 6 |

**Pre-existing, and the first time it has been quantified.** Twenty-one
runs across two kernels: the harness ceiling is 300% and the spread on
this host is **103–491%**, so the ceiling sits inside the noise rather
than above it. Not changed here — that is row 5's decision and it should
be made by whoever explains the outlier, not by whoever tripped over it.

It is the one stage of the commit tier this increment could not get green
on demand, and it is committed anyway for one reason: **`cdbca6e` fails
it at the same rate on the same image.** Everything else in that tier is
green, including 112/112 boot markers and every budget.

*Fixed in the third increment, and it was the statistic rather than the
machine — see "The four-core flake, and the statistic behind it" below.*

**Cost:** about forty QEMU boots. The libraries took **one**, and passed
on it. Everything else went on finding out that three of the four things
that then went red were already red, and on the ninth boot's worth of
work that turned "the mount scan got slower" into a counter.

**Next in the order:** freetype, which links `-lz` and wants `libpng` for
one optional feature, and is the first one that will ask this libc for
something neither of these did.

---

### M100 (third increment) — the four red stages, fixed rather than filed `[~]`

*Landed 2026-09-05.* The second increment found four red stages in
`--full`, proved none of them was its own, and wrote them down. Writing
them down is not fixing them. Three are fixed here and the fourth is
better understood than it has ever been.

#### The coverage ratchet: two floors had been broken since M98

`kernel/dev/fwcfg.c` at 89.55% against a 97% floor and `kernel/mm/heap.c`
at 97.37% against 100%. Neither was lowered. Both were **restored by
writing the tests that were missing**, which is what `CLAUDE.md` says
happens when new code arrives — the code had arrived without them.

- **`fwcfg.c`.** M98 added the `bootstrap` switch, M99 added `pytest`,
  and M99's fourth increment added `pybuild`. The test that exists to
  catch a *shared cache* between these accessors was grown for the first
  two and not the third, so `boot_pybuild_enabled` had zero coverage. Its
  own comment, three lines above the table it needed a row in, said:
  *"a switch that is only checked against the three that existed when it
  was written is a switch nobody has checked."* **A warning in a comment
  is not a test.** The table is now one array that the loop derives the
  directory, the selectors, the expectations and the count from, every
  switch is asked on every case, and each is asked twice. 89.55% → **100%**.
- **`heap.c`.** `grow_heap` asks two things per page and both can say no:
  pmm can have no frame, or the *mapping* can fail — which the real
  `vmm_try_map_page_in` does when it needs a frame for a **page table**.
  The two unwind differently, because on a mapping failure the frame for
  the page has already been handed out and the loop has to free that one
  itself. Those three lines had never run since M102 wrote them, because
  `tests/fakes/fake_vmm.c` could not fail a mapping — under a comment
  saying that making it fail *"would be inventing a failure mode the fake
  cannot honestly model"*. Half right: the fake having no page tables is a
  reason it cannot decide **when** to fail, not a reason it cannot be
  **told**. `fake_vmm_fail_map_after(n)` models exactly the documented
  return and nothing more, and the new test asserts the frame comes back.
  Both new tests were checked by breaking the code they cover: removing
  the `pmm_free_frame` gives `expected 0, got 1`, and pointing
  `boot_pybuild_enabled` at the wrong fw_cfg name fails four assertions.

**And the ratchet's own two defects, which are why nobody noticed.**

- **It lied about where a floor came from.** `parse_floor` dropped the
  commit column, so the `FELL` line printed `commit()` — the *current*
  HEAD — under the words "set at". Both broken floors were set at
  `b7bb520` and were being reported as `cdbca6e`'s. That is the one fact
  that separates *"you just broke this"* from *"this has been broken for
  a while and nobody looked"*, and it was showing the wrong one.
- **Its report was mostly advice.** Seven files printed
  `raised ... raise it, and say so` on every run because their floors
  were rounded down to whole numbers, and eight more printed `new`
  because they had no row at all — including `kernel/sched/sched.c`,
  2,157 lines in the host tier since Q13 with nothing constraining its
  direction. **Fifteen lines of standing advice, and the two real
  regressions underneath them.** The floors are the measured number now,
  every file in the report has one, and a file without a row is an
  **error** rather than a sixteenth line. `make coverage-check` is silent
  when nothing changed, which is the only state in which anybody reads
  it.

#### `waitpid(-pgid)` was refused by a comment that had come true

```c
if (want < -1) {
    return -1; /* process groups arrive with M85 */
}
```

M85 arrived five milestones ago and brought sessions, process groups, job
control and `kill(-pgid)` with it. This line stayed. **A TODO that names
a milestone stops being a TODO the day that milestone lands; after that
it is a false statement about the system** — and this one was false on
the path where being wrong is silent, because `waitpid` returning -1 for
a group that exists is indistinguishable from having no such children.

All four POSIX shapes work now (`> 0`, `-1`, `< -1`, and `0` for the
caller's own group). The group filter is applied *before* the
"any children at all" flag, deliberately: no matching children is
`ECHILD`, because the alternative is a wait that blocks forever on a
group that will never produce anybody. `user_space/bin/forktest.c` grew
four exit codes for it, and the interesting ones are not "it does not
work" — they are the two ways the filter can be subtly wrong: returning a
child from the *caller's* group (which looks like success), and blocking
rather than failing once the group is empty. Falsified by restoring the
old line: `forktest exited 10`, and the boot panics.

Nothing in this tree passed either form, which is exactly why nobody
noticed. `wait()` is `waitpid(-1)` and every toybox caller passes -1.

#### A bug this increment introduced, and what it cost to find

`measure -t` needs a process group to kill, so the first version called
`setpgid(0, 0)` in every child — including the ones with no deadline, on
the theory that a process group costs nothing. **It is not free.** The
unbounded `make -j1` step, 188 s on the run before, stopped producing
output entirely and was still in it ten minutes later; the two
single-process steps either side of it were fine, which is what a
job-control stop looks like — it takes the process with children to make
it visible. Scoping the call to `-t` restored it to 200.9 s.

The lesson is the general one and it is worth the space: **a capability
nothing uses is not free.** This one cost the step it was added next to,
and it was added for a deadline that step does not have.

#### The four-core flake, and the statistic behind it

The second increment quantified this and left it: `tools/smp-test.sh`
failing about one run in six, on this kernel and on `cdbca6e`'s, with
`smp_parallel_cost_pct` spanning **103–491%**. The third increment found
why, and it was not the machine.

**491% is impossible.** Four cores cannot cost more than four times one
core's work; the units do not allow it. A number outside the range its
own units permit is the tell that the *statistic* is wrong rather than
the thing being measured — and it had been sitting in the log, passing,
for as long as it stayed under 300.

The kernel took the minimum one-task time and the minimum N-task time
**independently, across different rounds**, and divided:

```c
for (round = 0; round < 3; round++) {
    a = smp_bench_round(1);     one_us  = min(one_us,  a);
    b = smp_bench_round(cpus);  many_us = min(many_us, b);
}
cost_pct = many_us * 100 / one_us;
```

That is not the best of three ratios. It is the ratio of two unrelated
best cases: a lucky one-task round shrinks the denominator, the unlucky
N-task rounds are all that is left in the numerator, and the quotient
compares two moments that never happened together. The comment above it
argued correctly that *"interference can only make a round slower, so the
fastest round is closest to the machine"* — and then applied that
argument to the two halves separately instead of to the quantity being
budgeted. It also meant the three `[perf]` rows disagreed with each
other: the printed ratio did not equal the printed times divided,
because they came from different rounds.

**Fixed by pairing.** The ratio is computed per round, from two
measurements taken seconds apart under the same host conditions, and the
best of *those* is the answer. Five pairs rather than three, which is
80 ms of a boot.

| | spread over the runs measured | failures |
|---|---|---|
| unpaired, 3 rounds | 103–491% (12 runs, two kernels) | 3 of 12 |
| paired, 3 rounds | 83–260% (12 runs) | 0 of 12 |
| paired, 5 rounds | **92–180%** (11 runs) | 0 of 11 |

**The ceiling stays at 300.** The spread does not justify tightening it,
and choosing a tighter one from eleven samples of a noisy host would be
the same mistake one level down.

**And this stage turns out to have had two failure modes, not one.** The
one above fails in about 20 s, because the boot reaches `[m106]` and the
ratio is over the ceiling. The other fails at exactly **240 s**, which is
`smp-test.sh`'s own ceiling: the four-core boot never reaches the marker
at all. Twenty-seven standalone runs of the fixed harness have not
reproduced it — including three taken immediately after a full graded
boot, which is the position it occupies in the tier — but it appeared
once in a commit-tier run after the fix, so it is real and it is not the
statistic. **That one is M106's tail**, which already names *"an
intermittent stall on the exit path"* and whose history in the archive is
*"about one boot in ten hangs"*. It is row 5's, it is not M100's, and the
useful thing this increment can leave behind is that the two modes are
now **distinguishable by their duration**: ~20 s is the measurement, 240 s
is the machine.

**And it changes what this file claims about the machine, in the good
direction.** Four cores were recorded as costing 114–200% of one core's
work. The honest number is 92–180%, and M106's refusal of per-CPU run
queues was made on a figure that was too *pessimistic* — the coarse
`sched_lock` is even less of a bottleneck than the milestone that kept it
believed.

#### One process note, recorded rather than hidden

Two consecutive runs of the fast tier failed `test_fwcfg`'s new
switch-independence test with four assertions, and then eleven
consecutive runs passed it with no source change of any kind. Both
failures happened while a `make -s all` was running concurrently in the
background — a mistake in how the command was backgrounded, not a
property of the tier. `tests/runner.c` does not shuffle, fork or thread,
and `$(TEST_BIN)` does depend on every kernel source it compiles, so
neither ordering nor a stale build explains it. It is not reproduced and
it is not explained; the concurrent build is the suspect and it is
written down as a suspect rather than as a cause.

#### `docs/third-party-programs.md`, which had been wrong for five milestones

Four bullets on its capability list were false: files "up to 8 MiB"
(4 GiB since M93, and the *Limits* section forty lines above said so —
two numbers for one fact on one page), "there is no `<sys/socket.h>`"
(there is), "`scanf`: no. Still absent because nothing has asked" (it is
one of the seven differential tests), and "`fork`, `exec`, `dlopen`: no,
and not coming soon" — five milestones after M83, M84 and M95 landed all
three. The page also never mentioned `x86_64-lean_os-gcc`, which M94
built precisely so that nobody would have to type flags by hand, so its
title question was answered with the long way round.

Fixed, with the two routes stated side by side, every claim checked
against a file in this tree, and a table of what has actually been ported
since. **A capability list is a thing that goes wrong silently**, and the
rule now written on the page is that a claim which cannot be checked
against a file in this tree does not belong on it.

---

### M100 (fourth increment) — freetype and expat, and the first library graded against itself `[~]`

*Landed 2026-09-08.* The fourth and fifth entries in M100's dependency
order: **freetype 2.13.3 and expat 2.6.4**, both built by this project's
own compiler with no edit to either source, both installed into the
sysroot by their own `make install`, and both graded on the machine by
the new `[m100c]` marker. Five of nine, and the count of source edits is
still zero.

**freetype is the first library in the stack that links two of the ones
before it** — `-lz` for compressed tables and `-lpng16` for colour
bitmap fonts — and its configure would not proceed with `--with-png=yes`
until it found libpng. It looked for it the way every modern autotools
project does, through `pkg-config`, which this host did not have and
which knows nothing about a sysroot when it does. The cross convention
is a `$host-pkg-config` on PATH, so `tools/build-thirdparty.sh` now
writes `x86_64-lean_os-pkg-config`: `pkgconf` pointed at the `.pc` files
the earlier libraries installed, with `PKG_CONFIG_SYSROOT_DIR` so that a
`-I/usr/include/libpng16` in a `.pc` file becomes a path into the sysroot
rather than into the host's `/usr`. `pkgconf` joins `gsed` in
`docs/toolchain.md` as a host tool somebody else's build asked for.

**And it is the first library with no test suite this machine can run**,
which forced the question of what grades it. The answer is the shape
`tools/sh-test.sh` and `tools/math-test.sh` already have — a differential
test — applied one layer down: `tests/freetype/ftrender.c` is compiled
twice from the same source, once for the host against a host build of the
same freetype tarball, once for the machine against the one the cross
compiler built, and the two outputs must be **byte-identical**. It renders
every printable ASCII glyph of DejaVu Sans at four sizes through the
TrueType bytecode interpreter, the smooth rasterizer, the monochrome one
and the autohinter, hashes every bitmap row by row, and prints the
kerning of six pairs — 580 lines, and nothing in the fixture says what a
glyph looks like. What that grades is tens of thousands of lines of
somebody else's fixed-point integer arithmetic, compiled by this
project's compiler, against clang's compilation of the same code on
another machine: one wrong shift or signed division anywhere in the
rasterizer changes a hash. **It agreed on the first boot.**

expat is the opposite case: it ships 4,392 checks of its own
(`tests/runtests`), cross-built here and run on the machine, and the
sentence the host prints — `100%: Checks: 4392, Failed: 0` — is the one
the machine has to print. It does. `xmlwf`, the well-formedness checker,
reads a document off this disk and names the line and column of a tag
that does not match, which is the same library as a program.

#### What the two of them found about this target

- **`checking for working mmap... no`**, from freetype. `AC_FUNC_MMAP` is
  a *run* test and autoconf answers it "no" for every cross build, so
  freetype uses its ANSI stdio stream rather than the mmap one. This OS
  has had file-backed mmap since M91; what it does not have is a way for
  a configure script to run a program on it. A fact about
  cross-compiling, not about the kernel — and the stdio path is a better
  test of this libc anyway, since a 750 KB font read through
  `fseek`/`ftell`/`fread` is exactly where M98's `ungetc` bug lived.
- **No `-lpthread`.** `ax_pthread.m4` tries `-pthread`, `-lpthread` and
  friends and finds none, because this libc's threads live in `libc.a`.
  A warning only (`FT_DEBUG_LOGGING`), and the same shape as `libm.a`'s
  argument in the Makefile: an empty `libpthread.a` would answer it, and
  nothing has needed the answer.
- **No entropy source.** expat's configure asks for `arc4random_buf`,
  `arc4random`, `getrandom` and the raw `SYS_getrandom` in turn, finds
  none, and settles for `/dev/urandom` — which devfs provides and which
  its own header says is a xorshift over the TSC. For expat's hash salt
  that is enough. **For a TLS key it is not**, and 9/9 is where this
  stops being a note.
- **config.sub moved.** freetype's bundled copy is the 2024-05 vintage,
  which lists one OS per line, and the six-names-on-a-line anchor
  `tools/toolchain-port/apply.py` has used since M94 is not in it.
  It learned the second form; the edit is the same one line.

#### The numbers

| | |
|---|---|
| `freetype_render_ms` | **1,300 ms** for 570 glyphs, the font read through stdio |
| `expat_suite_ms` | **50,240 ms** for 4,392 checks — about 100x the host, the usual TCG ratio, and now the single most expensive self-test in the boot |

Both are budget rows at ~2x. The expat row's note says out loud that it
is why the battery's ceiling will be raised if it ever is.

**Cost:** one graded boot, which passed. The port itself was two
`./configure` runs and the pkg-config wrapper; the fixture and its
oracle build were the work.

---

### M100 (fifth increment) — sqlite, and the two things it would not run without `[~]`

*Landed 2026-09-08.* The sixth library: **sqlite 3.47.2**, the
amalgamation, `./configure --host=x86_64-lean_os && make` with one
`config.sub` line and one of sqlite's own switches
(`--disable-dynamic-extensions`), graded by the new `[m100d]` marker
differentially, like freetype: `tests/sqlite/cases.sql` through the
`sqlite3` shell built for the machine and through the same shell built
for the host from the same tarball, and the two transcripts must be
byte-identical. 5,000 rows through a B-tree and an index, a transaction
rolled back and one committed through a journal file on this
filesystem, joins, window functions, a recursive CTE, `EXPLAIN QUERY
PLAN`, `VACUUM`, `integrity_check` twice, and a page of arithmetic and
string functions where a miscompile would show first. Sixty-seven lines
of transcript, and the host decides every one.

**It is the library that cost the most, and none of the cost was in
sqlite.** Four things this target did not have, each named by the
build or the run rather than by a checklist — M63's rule, and it held
for the fourth port in a row:

- **`dlopen` in a static program.** The first link ended in four
  undefined symbols — `dlopen`, `dlsym`, `dlclose`, `dlerror` — because
  on this target `dlopen` lives in `/lib/ld-lean.so` and reaches a `-pie`
  program through it, and `libdl.a` is empty on purpose (the Makefile's
  sysroot rule says why). sqlite's own documented answer for a system
  without `dlopen` is `--disable-dynamic-extensions`, and that is what
  was used. Recorded rather than changed: a static default is what a
  machine whose kernel loads `ET_EXEC` directly should have, and this is
  the second port to say so with a link error.
- **`popen` and `pclose`**, which this libc did not have and sqlite's
  shell uses for `.import '|command'` and `.output |command`. Written
  in `user_space/libc/src/popen.c` over M83's fork and M84's exec, with
  the one decision worth writing down: the child puts its pipe end on
  fd 0 or 1 with `dup2`, which on this kernel *releases* the slot it
  overwrites (M59), so the child holds exactly one reference and the
  parent's read sees EOF when it exits. **`system()` came with them.**
  M97 declared it and made it refuse — *"the version that would get
  written without a caller to check it against would be wrong in some
  way nobody would find"* — and sqlite's `.shell` is the caller. All
  three share one mechanism and are graded twice: `libctest` checks the
  output arrives, the input arrives (which means the child saw EOF), and
  the exit status comes back including the shell's 127; and the sqlite
  transcript runs `.system` and a piped `.import` where the host's
  `/bin/sh` is the oracle.
- **fcntl record locks.** sqlite's unix VFS takes an `F_SETLK` before
  every read transaction and another before every write, and treats any
  answer other than "granted" or "held by somebody else" as a **disk I/O
  error**. This libc had declared the three lock commands and refused
  them with `EOPNOTSUPP` since M89, on M65's rule — an advisory lock that
  always succeeds protects nothing while telling every caller it did —
  so every `INSERT` on this machine would have failed. That rule is why
  the answer is a real lock table rather than a stub.

- **Recursive mutexes**, and this is the one worth the most space.
  With `popen` and the locks in place, sqlite linked, was put on the
  image, and **hung** — no output, no error, no exit, for 600 seconds.
  The transcript went to a file so the log showed nothing; a temporary
  per-syscall trace on the task named `sqlite3` showed everything: its
  **eighth syscall of its life** was `futex(WAIT)` on a mutex at a
  static address, and it never returned. A single-threaded program
  blocking on a futex is a program waiting for itself. sqlite is built
  `THREADSAFE=1`, its database mutex is `PTHREAD_MUTEX_RECURSIVE` by
  design, and it takes that mutex inside itself on every API call. This
  libc's `pthread_mutexattr_settype` **refused** `RECURSIVE` — on M65's
  rule, deliberately, and its own header said why: *"a recursive mutex
  this library treated as normal deadlocks the first time a program
  relies on the recursion, somewhere far from here."* Right about the
  deadlock; wrong that refusing prevented it. **sqlite does not check
  what `settype` returns.** Nor does libstdc++'s `std::recursive_mutex`,
  which was one program away from the same hang. A refusal only refuses
  when the caller looks, and the honest thing left was to make the type
  real: `pthread_mutex_t` grew an owner and a count (four bytes to
  sixteen; glibc's is forty), RECURSIVE and ERRORCHECK do what POSIX
  says, NORMAL's fast paths are untouched, and the two static
  initialisers every port spells (`PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP`
  and its ERRORCHECK twin) exist. **That is an ABI change**, and the
  bill was paid where it fell: libstdc++ rebuilt from the GCC build tree
  against the new header (and it had to be a *clean* rebuild — the first
  attempt re-archived Sep 3 objects because automake's dependency files
  do not name the sysroot's `pthread.h`), and CPython rebuilt because
  `libpython3.12.so` embeds a `pthread_mutex_t` in every lock it
  allocates and is dynamically linked against this libc. Every static
  binary carries its own libc and was untouched. `libctest` checks the
  type contract from one thread — three holds need three unlocks, an
  ERRORCHECK holder gets `EDEADLK` rather than a hang, an unlock by a
  non-holder is `EPERM`, a type that does not exist is still `EINVAL`,
  and a NORMAL mutex behaves exactly as before — and `threadtest` checks
  the half that needs two threads: the same recursive mutex taken three
  deep by both with a counter inside, and a foreign unlock refused.

#### The record locks

`kernel/fs/flock.c`, and it is POSIX's model kept to what it says:
locks belong to a **process** on an **inode**, cover a byte range with an
open end for "to EOF", are shared or exclusive, never conflict with the
holder's own, and are released when the process closes **any**
descriptor for the file — the clause everybody finds surprising, which
sqlite's own source comments on for two pages before working around it,
and which is honoured exactly: from `close`, from `dup2` (which closes
`newfd`), from exec's `FD_CLOEXEC` sweep, and by pid at exit. The
kernel resolves `l_whence`, because only the fd table knows the offset
`SEEK_CUR` is relative to. `F_SETLKW` parks on a channel every release
wakes, using the seq form of `sched_block_on` so a release between "the
answer was CONFLICT" and "I am asleep" is not lost. `F_GETLK` reports
the holder. A held lock is `EAGAIN`, a full table is `ENOLCK`.

**The table is pure logic with no scheduler in it, and that is a test
decision.** The blocking lives in `syscall.c`; `flock.c` answers
"conflict" and the caller decides whether to wait. So the range
arithmetic — where a lock table's bugs actually are — is graded on the
host in `tests/test_flock.c`: two readers coexist and a writer excludes;
a process upgrading its own read lock to a write lock over the middle
leaves three entries; unlocking the middle splits, unlocking an edge
trims, unlocking what is not locked is not an error (sqlite's unlock
path relies on that); `len 0` reaches every later byte, including
sqlite's own one-byte-shared-lock-then-write-lock-to-EOF pattern at
offset 2^30; a refused request leaves the table unchanged down to the
byte; touching locks of one type merge and of two types do not; the
close rule releases one process's locks on one file and nothing else;
and a full table refuses a split that would need an entry while still
allowing a trim that does not. Eight tests, mutation score **91.5%**.
The boot marker adds the half the host cannot: `flock_count()` must be
zero after sqlite exits, so a lock the close rule missed is a failure
and not a leak.

What is **not** there: deadlock detection (`EDEADLK`). Two processes
each waiting for the other's lock will wait. sqlite never calls
`F_SETLKW`, nothing else here does either, and a detector nothing
exercises is the thing M65 forbids.

#### The numbers

| | |
|---|---|
| `sqlite_fixture_ms` | **2680 ms** — the whole transcript, journal and VACUUM included |
| `kernel/fs/flock.c` | 99.30% line coverage, first floor; mutation score 91.5% |
| `libc.a` | 528 KB, up from 524 |

#### What the ABI change dug up, in order

Growing a struct every port embeds is the kind of change that finds out
what else was quietly wrong, and it found two things.

- **libstdc++ had been stale since M99, and the rule it was stale
  against broke static C++.** The first "rebuild" of libstdc++ against
  the new header re-archived its Sep 3 objects: automake's dependency
  files do not name the sysroot's headers, so `make` saw nothing to do.
  A clean rebuild then failed to link `tests/cxx/library.cpp` — the
  M97 fixture the tier had passed on every run — with `relocation
  truncated to fit: R_X86_64_PLT32 against undefined symbol _ITM_RU1`.
  libstdc++ is compiled PIC throughout (libtool's `-prefer-pic`, for
  the `.a` as much as the `.so`), M99's second increment made `-fPIC`
  mean the small code model, and a small-model object calls an
  external function through the PLT with a 32-bit PC-relative
  relocation: fine for every function that exists, and impossible for
  a **weak undefined** one, which ld resolves to address 0 — not within
  2 GiB of a program at 512 GiB. `cow-stdexcept.o` calls the
  transactional-memory hooks weakly, so every static C++ program that
  reached `std::stoi` or `std::runtime_error` stopped linking, and had
  in truth been unable to link against a *current* libstdc++ since
  2026-09-04. Nobody knew because the archive predated the rule. The
  fix is in the target description, where every flag of this kind
  lives: **`-fno-plt` wherever the small model is chosen**. The call
  becomes a GOT load (`R_X86_64_GOTPCRELX`), a GOT slot can hold 0
  anywhere, and it costs nothing the PLT was buying here — `ld-lean.so`
  binds eagerly, so a GOT entry filled at load is what the PLT stub
  would have reached on the first call anyway. Ten-line experiment
  first, then the spec, then GCC's driver rebuilt (58,000 lines of
  make log) and libstdc++ rebuilt behind it. `library.cpp`, a
  `std::recursive_mutex` program and a `<stdexcept>` program all link.
- **CPython's "rebuild" was the same non-event.** `tools/build-python.sh`
  starts clean only when its *port* changes; its Makefile does not track
  the sysroot's headers either, so the rebuild produced a `libpython`
  whose objects were dated Sep 4, and the first graded boot after it
  panicked at `[m99]`: `Fatal Python error: take_gil:
  PyMUTEX_UNLOCK(gil->mutex) failed` — a four-byte mutex handed to a
  libc that reads sixteen, the `type` field being whatever lay beside
  it. **Both build scripts now keep an ABI stamp** — a hash of every
  header the sysroot hands a program — and throw a build tree away when
  it was made under a different one. The port stamp catches a changed
  port; this catches a changed libc, which is the more common event and
  the one neither script had a word for.

#### Two instruments that misbehaved, recorded rather than hidden

- **The mutation harness left its timed-out mutants running.** A
  `killed-timeout` verdict came from `subprocess.run(timeout=)`, which
  kills the `make` it started and nothing underneath it — and a mutant
  that inverts a loop condition is by definition one that never
  finishes. Seven orphaned `leanos-tests` processes spun at full CPU for
  forty minutes, unnoticed, while the graded boot ran beside them: that
  boot's every disk number came in **3–10x** its recorded value
  (`disk_1mib_write_through_us` 413 ms against 77) and it hit the
  capture ceiling with thirty markers to go. An instrument that quietly
  degrades the instrument next to it is exactly the kind this project
  is supposed to see. `tools/mutate.py` now runs each mutant in its own
  session and `SIGKILL`s the whole group on timeout, and
  `tests/test_flock.c`'s fill loops are bounded at one past the table,
  so a mutant that never refuses is an assertion naming a line rather
  than a ninety-second timeout that counts as a kill.
- **The boot capture ceiling was already spent.** `SECONDS_TO_RUN` was
  400; the battery measured 360 s with freetype and expat in it, and
  the first boot with sqlite hit 400 exactly. Raised to 600 (and the
  I/O APIC pass to 900), with the history written where the last three
  raises are.

**Cost:** six graded boots. One that timed out under the orphaned
mutants, one that found the hang, one diagnostic boot whose transcript
went to a pipe and so showed nothing (a lesson about `tee` and stdio
buffering, learned in six minutes), one with the syscall trace that
found it in fifteen lines, one that panicked at `[m99]` on the stale
CPython, and the one that passed — after two libstdc++ rebuilds, a GCC
driver rebuild and two CPython rebuilds. Most of the increment was the lock table and the
mutex; most of both was the tests.

**Next in the order:** harfbuzz, which is C++ and links freetype, and
then the two that decide the arc — a TLS library, which is where the
entropy note above becomes a kernel item, and ICU.

---

### M100 (sixth increment) — harfbuzz, the first C++ library in the stack `[~]`

*Landed 2026-09-08.* The seventh library: **harfbuzz 8.5.0**, ~120,000
lines of C++ templates over OpenType tables, `./configure
--host=x86_64-lean_os && make` with one `config.sub` line, linking the
freetype beside it in the sysroot through the `pkg-config` wrapper the
fourth increment wrote, and compiled by this project's `g++` against the
libstdc++ the fifth increment rebuilt. Graded by the new `[m100e]`
marker, differentially, like freetype and sqlite: `tests/harfbuzz/hbshape.c`
shapes six strings in five scripts — Latin with ligatures, Greek,
Cyrillic, Arabic right-to-left with every letter substituted for its
joining form, Hebrew with combining marks positioned by GPOS, and a line
of digits and superscripts — first through harfbuzz's own font loader in
font units and again through `hb-ft` with freetype answering for the
advances at 24 px, and prints every glyph id, cluster, advance and
offset. The host's build of the same tarball produces the reference, and
the machine's output must be byte-identical. **It was, on the first
boot.** Seven of nine, and still zero source edits.

**What it asked for was ten functions this libc's `<math.h>` did not
declare** — the float variants C99 has had since 1999, which nothing
ported here had named before. The build named them the way a build
does, one translation unit at a time: `floorf`, `ceilf`, `fabsf`,
`sinf`, `cosf`, `tanf` from the first compile (thirteen errors from one
header, all the same shape); `hypotf` from the second, once those six
let it get that far; `sqrtf` and `atanf` from the third. Three compiles
to learn nine names is the honest cost of M63's method, and the
correction is the one-line grep over harfbuzz's sources that lists every
`*f(` it calls — which is how the tenth, `roundf`, was found: named
fifty times, behind a fallback macro of harfbuzz's own, so no compile
would ever have said it. Added, and only those ten (M63's rule), each
the double function rounded once to float;
`tools/math-test.sh` learned to grade `float f(float)` and `float
f(float, float)` declarations the way it grades the doubles, against the
host's own floats, with the nine special values asked in float too —
where "huge" is infinity and "tiny" is zero, which is the point. Its
first run found what a claim written before the measurement usually
finds: the three trigonometric ones differ from the host by **exactly
one float ulp** at some points — the ordinary consequence of rounding a
correct double once — and the claimed tolerance of `1e-7` was one ulp at
the wrong end of a binade (2^-24 at the top, 2^-23 = 1.19e-7 at the
bottom). The claim was corrected to one ulp with the arithmetic in the
row; nothing was widened to hide anything. And then `atanf(+inf)`: the
host's answer is the float just *below* π/2 and this libc's the one
just above — which is the correctly rounded one, as it happens — and
the special-value check was judging floats at double precision. It
judges them at one float ulp now, which is what the rows claim.

**And then `std::isnan`, which did not exist here, and had never
existed.** With the floats in place the next error was `'__builtin_isnan'
is not a member of 'std'`: harfbuzz writes `std::isnan(x)`, this libc's
`<math.h>` defines `isnan` as a macro over the builtin, and `<cmath>` is
supposed to `#undef` that macro and provide the function — *if*
libstdc++ was configured believing `<math.h>` is C99. It was not. Its
configure compiles one probe naming all twelve classification and
comparison macros plus `double_t` and `float_t`; this header had the
six classifications and none of the six comparisons (`isgreater`
through `isunordered`) and neither typedef, so `_GLIBCXX_USE_C99_MATH`
came out 0 on 2026-09-03 and every `std::isnan` in every C++ program
compiled here since has been a compile error waiting for somebody to
write one. Six macros and two typedefs added (`libctest` checks the
property that makes the macros different from the operators — a NaN
compares false to everything, quietly), and libstdc++ **reconfigured**,
not just rebuilt: the answer lives in its `config.cache`, and the
fifth increment's ABI stamp, which `make clean`ed on a header change,
now throws the directory away so the top-level make configures it
again. That is the third time in two increments that "rebuilt" turned
out to mean less than it said.

**Then libstdc++ objected to the floats it had been providing itself.**
Every C++ program that used one of the new functions ended in `multiple
definition of 'fabsf'` from `libstdc++.a(math_stubs_float.o)`: libstdc++
*defines* every float function its configure believes the libc lacks,
and for a cross target that belief is not a link test but the port's
own case in the generated `configure` — the one M97 wrote "modelled on
fuchsia, which asks for nothing", with a note that a hosted libstdc++
would one day turn it into "a list of things measured rather than
assumed". That day: the arm now declares the nine of the ten this libc
has that libstdc++ would otherwise stub (`roundf` has no stub), each of
them a function `tools/math-test.sh` grades, and libstdc++ was
reconfigured a second time. The port changed, so the toolchain was
rebuilt from the tarballs by `tools/build-toolchain.sh` before this
landed — the stamp mechanism's own rule, and the only proof that the
recipe in `apply.py` is the toolchain on disk.

**And the last stop was libtool's.** With everything compiling, the
link of `libharfbuzz.a` ended with `'/usr/lib/libpng16.la' is not a
valid libtool archive`. libtool writes a `.la` beside every library it
installs and records that library's dependencies in it by their
*final* paths — `/usr/lib/libpng16.la` inside `libfreetype.la` — so a
sysroot staged under `DESTDIR` on a host that is not this machine is
full of files naming files that do not exist, and the first library to
link another through one stops. Every distribution that stages a
sysroot deletes `.la` files for exactly this reason, and
`build-thirdparty.sh` does now; the same facts live in the `.pc`
files, which pkg-config resolves relative to the sysroot. That moved
the dependency question to pkg-config, and the answer there is
`--static`, always: every link on this target is static unless it says
`-pie`, and a `.pc` file's `Requires.private` is exactly the list a
static link needs — without the flag, `--libs freetype2` says
`-lfreetype` and the link ends in every png and zlib symbol freetype
uses.

Two things recorded rather than fixed:

- **`--enable-static` has to be spelled out.** harfbuzz's configure
  defaults static libraries *off*, and libtool cannot build shared ones
  on this target (the second increment's note), so the default would
  build nothing and say so only at install time. Every autotools library
  in this stack so far had `--enable-static` on by default; this is the
  first that does not, and it is worth knowing for the two that follow.
- **No `-lpthread`, for the second library in a row.** harfbuzz then
  uses its own atomics and needs nothing, so nothing is lost; but the
  pattern is now two libraries long and an empty `libpthread.a`, on
  `libm.a`'s argument in the Makefile, is looking less like a
  speculation and more like a scheduled question.

#### The numbers

| | |
|---|---|
| `harfbuzz_shape_ms` | **1900 ms** — twelve shaping runs across five scripts, both font loaders |
| `libharfbuzz.a` | 115.6 MB, with debug information; the shaping fixture statically linked against it is 45 MB |

**Cost:** one graded boot, which passed; the rest was the four
compiles it took to get there — floats, `std::isnan`, the `.la` files —
none of which was harfbuzz's fault and every one of which the next C++
library would have hit instead — plus a toolchain rebuilt from scratch,
because the port changed. ICU is that library, and it is a
hundred times the size.

**Next in the order:** a TLS library (mbedtls 3.6.2, which ships plain
Makefiles beside its CMake), where `/dev/urandom`'s xorshift stops being
enough and `getrandom` becomes a kernel item — and then `https://` end
to end, which is the bullet the whole arc has been pointed at.

**One process note, recorded rather than hidden.** The default tier's
boot stage failed once on the way to this commit, on `[wm] animation
missed its frame budget: 1 of 4 frames` — every marker present, every
budget row inside its ceiling, and the one failure the item M106's tail
already names (*"one animation frame in six misses its budget"*). The
boot re-run alone passed. It is not this increment's, and it is not
new; it is written here because a tier that went red on the way to a
commit should say so in the commit's own entry.

---

### M100 (seventh increment) — sockets a POSIX program can use, and `O_NONBLOCK` `[~]`

*Landed 2026-09-08.* Not a library: the thing the eighth library asked
for before it would link. mbedtls's socket layer is `read(fd)` and
`write(fd)` on a socket and nothing else, and on this machine `read` on
a socket returned -1, `write` on one returned -1, `recv` returned 0 for
"nothing yet" — which every program written against POSIX reads as the
end of the stream — and `O_NONBLOCK` was `#define`d to 0 under a
comment saying *"every descriptor here is what it is"*. Nothing here
had noticed because nothing here called `recv` without polling first.
M88 deferred `O_NONBLOCK` to M100 for *"the first program that needs
it"*, and this is that program.

**What changed, and what deliberately did not.**

- `SYS_recv` and `SYS_send` are untouched. Their contract — never
  block, one segment at most, 0 for "not now" — is the one every caller
  written here polls against, and a call whose meaning changed under
  them would be the worst kind of ABI change. `MSG_DONTWAIT` is now the
  spelling that reaches them from libc, with their 0 turned into the
  `EAGAIN` it always meant.
- `SYS_read` on a stream socket **blocks**: for a byte, or for the end
  of the stream (0, as `read(2)` has it, not `SYS_recv`'s -1), or — with
  the non-blocking bit — returns `-OS_ERR_AGAIN`, a new code beside
  `OS_ERR_INTR` with the same shape and the same rule (only when nothing
  was transferred). It parks on the poll channel, which every arrival
  and the scheduler's tick wake, and re-asks the connection each time.
- `SYS_write` on a stream socket **takes it all**: one segment at a
  time through the stack, parking when the send buffer is full. There
  is no write-readiness wake anywhere in this kernel (`<poll.h>` has
  said so since M88), so the park has a deadline of one tick — a
  bounded wait re-asked at the tick rate, which is not a spin and is
  written down as what it is.
- **`O_NONBLOCK` is a byte in the fd slot** beside `cloexec`, for the
  same 16 KiB-of-BSS argument, set and cleared by `F_SETFL` reaching
  the kernel (`F_SETFL_CMD`, the seventh `fcntl` command) and reported
  by `F_GETFL` next to the access mode M98 put there. Honoured by
  sockets and by **pipes** — `pipe_read` and `pipe_write` grew the flag,
  an empty read is `EAGAIN` and a full write is the short count or
  `EAGAIN` — and accepted for a file, which never waits and so already
  has it. One divergence, stated: the bit travels with the slot, so
  `dup2` and `fork` copy it, where POSIX keeps it on the open file
  description two descriptors share. Nothing has asked for the shared
  form.
- libc: `recv`/`send` are `read`/`write` unless `MSG_DONTWAIT`;
  `read`/`write` spell `-OS_ERR_AGAIN` as `EAGAIN`; `O_NONBLOCK` is
  `0x800`, the kernel's `OS_NONBLOCK_BIT` and Linux's value, so nothing
  translates it.

**What the first boot found, and the instrument that found the rest.**
The first blocking `read` never returned. Nothing in `tcp.c` woke the
poll channel: the wakes were the keyboard's, the mouse's, the pipes',
the ptys' and UDP's, and every TCP wait in the tree happened to carry a
deadline, which is why `SYS_waitfds` on a socket had always worked. So
every inbound segment now wakes the channel — data, an ACK that frees
the send buffer, a FIN, a reset — once per segment after whatever it
did, and the timer's own events (a connect that gave up) do the same.
The second boot got past the read and stopped inside the 64 KiB write.
A boot is six minutes; the instrument that was missing is
`tests/test_tcp_loopback.c`, **both ends of a connection on the host,
joined by the real loopback queue**, driven with the syscalls' exact
chunking — and its first run found something the machine had been
paying for since M66: **the handshake rejected its own SYN-ACK.** On
loopback `ip_send_from` delivers synchronously, so the SYN-ACK reaches
the client *inside* `tcp_connect`'s `emit()`, one line before
`snd_nxt = iss + 1` — and the SYN_SENT branch checks the ACK against
`snd_nxt`. Every loopback connect sat in SYN_SENT and completed on the
SYN's first *retransmission* a second later; it worked, at one RTO per
connection, for four milestones, and the machine's own comment said
"on loopback this is already established by the time `sys_connect`
returns". The listener's SYN-ACK had the same after-the-emit advance.
Both moved; the rule is now written where the first one was: the state
a reply is judged against must be true before the segment that
provokes the reply is sent. The fake NIC also learned the loopback
clause `net_is_local_ip` has had since M66 — without it a connection to
`127.0.0.1` left through the fake, and a fake narrower than the
definition it stands in for grades a machine that does not exist.
And every reset and every retransmission the stack sends is a log line
now, because both present elsewhere as "the transfer stopped" and had
been invisible.

**And a bug in the test, which the machine's own divergence exposed.**
With the transfer fixed, the boot still hung after the 64 KiB write
returned - main blocked forever on the read that expects end-of-stream.
The cause is the documented divergence in `<pthread.h>`: **a thread here
gets a copy of the fd table, not a share of it.** The peer thread's
`close(srv)` dropped only its own copy; the main thread still held the
server socket, so no FIN went out, and the client's read for EOF waited
on a close that never happened. The fix is the test's, not the kernel's
- the peer confirms without closing, and main joins (releasing the
peer's copy) and then closes its own reference - and it is the right
shape of finding for a machine-level test to make: a real OS divergence
caught the moment a program depended on the POSIX behaviour it does not
have. The transfer was also sized to 16 KiB, the size the native
section already streams reliably, so the graded boot does not spend
minutes in RTO recovery from loop-queue-overflowing 64 KiB bursts on
TCG.

**Graded where a person's program would meet it.** `tcptest` (M66's
self-test) grew a POSIX section with a thread as the peer, because a
blocking read needs somebody else doing the writing, and the checks are
on the clock as well as the bytes: a `read` that returns the message
the peer wrote 200 ms later must have taken at least 100 ms, or it did
not block; `O_NONBLOCK` and `MSG_DONTWAIT` with nothing pending are
`EAGAIN` and not 0; one `write` of 64 KiB — forty-five segments through
a 4 KiB send buffer — returns 64 KiB while the peer thread drains and
checks every byte; and `read` after the peer closes is 0. `libctest`
grades the pipe half from one thread: the bit reads back, an empty read
is `EAGAIN`, a non-blocking write fills the pipe and then says `EAGAIN`
with the count it took, and the reader drains exactly that count.

**Cost:** one graded boot{BOOT_NOTE}. The change is ~150 lines in the
kernel and ~60 in libc, and every one of them is a semantic a ported
program assumes without checking.

---

### M100 (eighth increment) — TLS, and the random device it would not take a key from `[~]`

*Landed 2026-09-08.* The eighth library and the bullet the arc was
pointed at: **mbedtls 3.6.2**, the long-term-support line, built with
its own plain GNU Makefiles (it ships them beside its CMake, which is
what makes it buildable here at all) by this project's compiler, with
no edit to any of its source. **The first `https://` this machine has
had**, graded three ways by two new markers:

- `[m100f]`: mbedtls's own `ssl_server2` listens on loopback with its
  own test certificate; mbedtls's own `ssl_client2` completes a verified
  session with it and reads the server's built-in page; then
  `tests/tls/httpsget.c` — written here, linked against the sysroot's
  mbedtls, through the same API a browser uses — fetches
  `https://localhost/` from that server with the chain verified against
  the test CA as a PEM file on this disk, and prints the page; and then
  the refusal: the same server asked for under the name `127.0.0.1`,
  which its certificate is not for, is refused with mbedtls's own
  `CN mismatch`, because an https that accepts any certificate is the
  lie M73's `fetch` was written to avoid. Every byte of it goes through
  M66's TCP, the seventh increment's blocking socket read and write, and
  `kernel/dev/random.c` for both ends' randomness.
- `[m100g]`: **three of mbedtls's own test suites**, each reading its
  own vectors off this filesystem and printing its own `PASSED` to the
  last one — ChaCha20-Poly1305 (the exact AEAD `[m100f]`'s session
  negotiated), SHA-2, and ECDSA: the cipher, the hash and the signature
  a TLS 1.3 handshake here uses, graded by the library's own answers.
  **Three, not the eighteen first tried, and the reason is a measurement
  M100 exists to take.** Run on the machine, the fuller set reported:
  chachapoly 8/8, shax 606/606, ecdsa 106/106 — the crypto is exact —
  but x509parse 501/872, ssl 839/851, ctr_drbg 293/294. The x509parse
  and ssl failures are overwhelmingly the suites reaching for mbedtls's
  `tests/data_files` tree, which is not on the image (they load
  certificates and keys by relative path), rather than a crypto fault;
  the one ctr_drbg vector is an entropy-source edge. A real gap with a
  shape — a data-file tree and two edges — recorded rather than hidden,
  and the marker requires the three whose vectors are self-contained and
  which pass to the last one. Adding the rest back is a data-file port
  and two investigations, which is the next person's.

**Why the client is a test program and not `fetch`.** M100's bullet
says TLS *"gives fetch something to do that is not a plaintext port 80
demo"*. It does not, and cannot: the non-negotiable is that no
third-party code ships in the OS, and `fetch` is the OS. mbedtls is
ported *against* this system the way zlib and freetype are, and the
program that links it lives in `tests/`, beside `ftrender` and
`hbshape`, as the thing that proves the port. `fetch` still refuses
`https://` by name, and still says why.

#### What mbedtls asked for, in the order the build named it

- **`fd_set` from `<sys/time.h>`.** `net_sockets.c` includes
  `<sys/time.h>` and `<sys/types.h>` and calls `select()` with an
  `fd_set`, because POSIX says `<sys/time.h>` makes everything in
  `<sys/select.h>` visible and glibc does. One include at the end of
  the header, and the *fifth* address at which this project has paid for
  the rule it keeps relearning: a header that has a thing and does not
  provide it where the standard says is, to a build, indistinguishable
  from not having it.
- **A socket a POSIX program can read.** `net_sockets.c` is `read(fd)`
  and `write(fd)` on a socket, expecting both to block. That is the
  seventh increment, and it is the larger half of this one.
- **`getentropy`.** Not from mbedtls — from toybox, whose portability
  layer sees `<sys/random.h>` exist and assumes the BSD function beside
  `getrandom`. The header rule from the other side: a header that
  exists promises everything its namesake has. Added.
- **Entropy that is not a counter**, which is the kernel item this
  arc's third note said would become one here.

#### The random device

`/dev/urandom` was a xorshift over the TSC — its own header said so,
honestly, and for expat's hash salt and Python's temporary-file names
that was enough. A TLS library draws a **key** from it. `kernel/dev/
random.c` is what it reads now, and the design is written so a host
test can say whether it does what it claims:

- **ChaCha20**, RFC 7539, sixty lines written from the RFC and graded
  against the RFC's own block-function vector — fetched from the RFC
  editor for the test rather than typed from memory, which turned out
  to matter: the vector as remembered had one byte wrong in the second
  row. The one cryptographic primitive here, and the one place in the
  file where the right answer was decided by somebody not writing it.
- **A pool that is a key.** Every input is XORed into 32 bytes and the
  key is run through the block function to mix, so the pool is never
  "the last thing that came in" but everything that ever did, permuted.
- **Fast key erasure on extraction.** To produce N bytes the generator
  makes N + 32 bytes of keystream, hands out N, and *replaces the key
  with the other 32* — so the key that produced any output is gone the
  instant the output exists, and a machine whose memory is read
  afterwards gives up nothing already handed out. The construction
  BSD's `arc4random` and Linux's `random.c` both settled on.
- **Fed by every interrupt of the boot** — the TSC at each one, from
  `irq_handler`, which is the timing of the world outside the CPU and
  the *primary* source on a machine with no hardware generator. That
  is this machine: QEMU's default CPU has neither RDRAND nor RDSEED,
  and every graded boot runs on it, so the code that uses them is
  exercised only on metal and the boot log says which it found. The
  RTC and the MAC go in too — not entropy, but distinct per machine.
- **No entropy estimate**, on purpose and said so in the header: Linux's
  credit-counting has been wrong in both directions for twenty years,
  and a blocking `/dev/random` that blocked on an invented number would
  be the lie of the other shape. What is claimed instead is narrower and
  checkable, and `random_events()` says how many inputs the pool has
  taken so the `[rng]` self-test can require the number to be large
  before anything asks for a key. `SYS_getrandom` sits beside the device
  file, under Linux's name, for the program that has run out of
  descriptors or lives in a chroot with no `/dev`.

`tests/test_random.c` grades the RFC vector, determinism under a held
clock, erasure (the state after an extraction cannot reproduce it),
that feeding changes everything after, the cheap sanity checks that
catch the bugs a stream generator actually has — a byte histogram flat
to six sigma over a megabyte and no repeated block — and a **golden
sequence**: the exact bytes the whole pipeline produces for a fixed
seed, which pins the pool's mixing and the fast-key-erasure extraction
that no external oracle can (a pool has design freedom), so a mutation
that still looks random is caught. Mutation score 68.2%; the residual is
the RDRAND/RDSEED path, which is `#if`'d out under the host test and so
cannot be killed there (it is graded on the machine by `[rng]` reporting
which the CPU has), plus the last of the pool's genuine freedom. `libctest` grades
the syscall and the device from user space; `[rng]` grades that the
pool was fed (20,139 interrupts by the time it ran) and both paths
are live.

#### The numbers

| | |
|---|---|
| `tls_fixture_ms` | **11,440 ms** — a server started, two verified TLS 1.3 sessions, one refusal |
| `mbedtls_suites_ms` | **26,820 ms** — three suites, ~720 of the library's own vectors |
| `[rng]` | 20,139 interrupts mixed into the pool by the time it ran; RDRAND and RDSEED both absent, as expected of QEMU's default CPU |
| `boot_to_desktop_s` | 362 → **393**, and the capture ceiling 600 → 900, for TLS and the suites |

**Cost:** many — TLS over TCG is the slowest thing in this arc, and the socket glue took most of them graded boots. The mbedtls build itself was three
commands and one header; the increment was the socket semantics before
it and the random device under it, which is what "TLS end to end" was
always going to mean here.

### M111 — `os`, a package manager, and the boundary a package lives behind `[x]`

**Landed 2026-09-09.** Asked for directly rather than taken off the
queue: "a package manager for the whole os, called `os` ... it will
install packages via `os install grep` ... and it will port linux
software and install it onto the os securely in an isolated fashion."

- [x] **`os install grep` installs GNU grep 3.11**, built for this
      machine by this project's own `x86_64-lean_os-gcc` from the
      published tarball with **no edit to its source** — the one change
      is a line added to its bundled `config.sub`, which is what every
      distribution does and what M94 already writes down as
      upstream-shaped. 1.2 MB binary, `provides: grep egrep fgrep`.
- [x] **A package format with no install hooks.** `user_space/lib/ospkg.c`
      — header, manifest, file table, payload. Installing is verify and
      copy; nothing runs. Built by `tools/os-pkg.c` on the host and read
      by `/bin/os` on the machine from the same source file.
- [x] **SHA-256 written here** (`user_space/lib/sha256.c`), three hashes
      per package for three different questions: the index's over the
      archive (did the right one arrive), the header's over the body (is
      it intact), each record's over one file (is what is on disk still
      what was installed — the one `os verify` asks months later).
- [x] **The kernel decides what a package may do.** `CAP_PKG_ADMIN`,
      `CAP_PKG_MAX`, `CAP_PKG_UNLISTED`, `kernel/proc/pkgcaps.c`, and a
      write gate on `/pkg` at nine syscalls.
- [x] Graded three ways: 30 host tests (`tests/test_ospkg.c`,
      `tests/test_sha256.c`), a differential script (`tools/pkg-test.sh`,
      in `--fast`), and four `[m111]` boot markers driven by
      `user_space/bin/pkgtest.c`.

#### The hole a package manager opens, which is the whole of the design

`caps_for_program()` has matched on the **basename** since M65, and its
own comment says why: `/bin/settings` and `settings` are the same
program, and a table keyed on the spelling would be a table with a way
around it. That reasoning was correct for forty-six milestones because
every executable on the disk came out of this repository.

`os install` ends it. If an installed program is *called* `compositor`,
that table hands it `CAP_ALL` — not through a bug, but by doing exactly
what it was written to do, to a file it was never written about. The
impersonation needs no exploit. It is a filename.

So the rule became one about a **place**:

- A program under `/pkg` never matches the shipped grant table.
- What it gets is what `/pkg/db/caps` records for its path, intersected
  with `CAP_PKG_MAX` **in the kernel** — `fs-write | network | audio`,
  and nothing else, ever.
- A program under `/pkg` the registry does not name gets **zero**. Not
  the default. Nothing.

And the registry is only worth reading because **`CAP_PKG_ADMIN` gates
every write whose normalized path resolves under `/pkg`** — create,
truncate, mkdir, rmdir, unlink, rename (both ends), symlink, and hard
link (both ends). `/bin/os` is the only shipped program that holds it.

**Both ends of `link`, and the source end is the one that matters.**
`link("/pkg/grep/3.11/bin/grep", "/tmp/x")` makes a second name for an
installed binary's inode, and a write through `/tmp/x` is a write to the
package. A gate on the destination alone misses it completely. That was
found by writing the test before the ninth gate, which is the argument
for `pkgtest.c` checking all nine separately rather than checking "can I
write under /pkg".

#### The `#!` over-grant, and the fix that was too big

Until this milestone a script's capabilities came from its
**interpreter**, full stop. Nothing had noticed, because every script on
the machine was this project's. A package shipping one line beginning
`#!/bin/sh` would have been launched with the shell's `CAP_ALL` —
`CAP_PKG_ADMIN` included, which is authority over every installed
package on the machine, for one line of attacker effort.

**The first fix was `caps_for_spawn_path(interp) &
caps_for_spawn_path(script)`, unconditionally, and it was wrong — for a
reason worth more than the fix.** The argument for it is sound: the
script is the program a person meant to run and the interpreter is
machinery, so machinery should not raise the ceiling. But scripts are
not in `CAP_GRANTS`, so intersecting with the table took *every* script
on the machine to `CAP_APP_DEFAULT` — and this kernel's own self-tests
write **twenty** `#!/bin/sh` fixtures into `/tmp`, several of which exist
to launch programs that need the network.

The boot failed with `mbedtls_net_connect returned -0x42` — `socket()`
refused — inside M100's TLS self-test, four hundred lines and one
subsystem away from the edit. Making the sound argument work would have
meant listing twenty temporary filenames in a table caps.h says should
stay one screen long, which is the tell that the argument was being
applied at the wrong scope.

**What landed is the rule M111 already applies to binaries, applied to
scripts:** a script **under `/pkg`** is intersected with what its package
asked for; a script anywhere else keeps the interpreter's grant. One
sentence covers both halves of the milestone — *nothing about a file
under `/pkg` may raise its capabilities above what its package
declared*. The impostor package ships a `#!/bin/sh` script whose only
line tries to write into the package database, and `[m111]` requires the
file not to exist afterwards.

**Left open, named rather than implied:** a script *outside* `/pkg` still
runs with its interpreter's grant, so a downloaded `.sh` launched from
the terminal — whose parent is `gui_terminal`, which holds `CAP_ALL` —
gets more than a downloaded *binary* in the same directory would. That
asymmetry predates this milestone and is not closed by it. **Condition:**
it closes when a script has somewhere to be declared, which is the same
condition a manifest for anything not installed by `os` names.

#### What GNU grep cost — four C library fixes, none of them predictable

Every one was named by grep's build rather than by a checklist, and not
one of them is the bug the error message describes.

1. **`<assert.h>` had `#pragma once`.** C11 7.2: that header is designed
   to be included more than once, re-reading `NDEBUG` each time. gnulib's
   `config.h` does `#include <assert.h>` then `#undef assert`, taking for
   granted that the next include puts it back. With the guard, nothing
   did, and `dfa.c` stopped **3,200 lines later** on "implicit
   declaration of function 'assert'" — naming a header it includes twice.
2. **`mbsinit` was missing.** Nothing here calls it and *grep does not
   either* — it probes for it. gnulib's rule on a failed probe is not "do
   without": it decides this platform's `mbstate_t` cannot be trusted,
   typedefs its own as an `int`, substitutes its own `mbrtowc` and **not**
   its own `wcrtomb`. The error was a type mismatch in a file nobody here
   wrote, about a state object nobody here asked for. Three lines to fix.
3. **`creat` was missing** — `open` with three flags, under the name code
   older than those flags still uses (gnulib's `creat-safer.c`).
4. **Every function in `<ctype.h>` was `static inline`,** and therefore
   in no object file at all. A configure script does not include a
   header — it *links*. `checking for isblank... no`, and then gnulib
   compiled its own `isblank`, which collided with the one in the header
   it could not see. The same trap was set for fifteen other names.

All four are **M94's lesson, arriving four more times**: a missing symbol
is not a missing feature, it is a *configure answer*, and the
substitution it triggers lands somewhere with no relation to the thing
that was absent.

**And the fix for (4) that did not work, which cost the most time.** The
obvious answer is C99's `extern inline` idiom — plain `inline` in the
header, one `extern inline int isblank(int);` in a `.c` file to emit the
out-of-line copy. It produced "multiple definition of `isascii'" from
every pair of objects. The reason is that GCC knows these names as
**built-in library functions**, so every translation unit already carries
an implicit `extern` declaration of them — and C99 says an inline
definition plus an external declaration in one unit is an *external*
definition. The identical header with a name GCC does not know behaves
perfectly, which is what makes it so hard to look at. The answer was an
X-macro: one list of sixteen expressions, expanded `static inline` in the
header and plain in `ctype.c`, so the fast path and the symbol are the
same arithmetic and there is one source of truth.

#### What the tests found, and what they are

- **UBSan, on the first run of the first test.** The reader used the file
  records where they lay, and the manifest between them is
  variable-length, so the table was landing unaligned: *"member access
  within misaligned address ... requires 8 byte alignment"*. x86-64 would
  have executed it forever. The fix is in the **format** — the manifest
  is padded with newlines to a multiple of 8, so with a 96-byte header
  and a 280-byte record the table is always 8-aligned, and the reader
  refuses a package or a buffer that is not. A reader that must not
  allocate and records that must not be copied leave alignment as the
  thing to fix, and a format is the right place to fix it.
- **`tools/pkg-test.sh` is the sixth differential instrument** and the
  same argument as the other five: this project's SHA-256 against the
  host's `shasum -a 256` over 250 real files from this tree, a package
  round trip decided by `cmp` and `diff -r`, determinism (the same tree
  twice is the same archive byte for byte), and one flipped byte in a
  real archive refused. Nothing in it says what the right answer is.
- **`tests/test_ospkg.c` builds every archive byte by byte** rather than
  by calling the writer — a reader tested only against its own writer
  agrees with the writer's mistakes.
- **`make mutate FILE=user_space/lib/ospkg.c` graded the tests**, which
  is what that harness is for, and it earned its keep on a file whose
  every line was already executed. Six of its survivors were real gaps:
  `ospkg_file_data` was never asked for an index it does not have (so
  its bounds check could be off by one, which is a read past the file
  table); no path in any test contained a **space**, so the control-byte
  comparison could have been widened to refuse every filename with one;
  `requires:` was parsed and never read back, which would have made `os`
  install a package and quietly not install what it needs; and
  `ospkg_caps_to_names` was never given a buffer too small for what it
  had to say — which is exactly the message about a package asking for
  too much. Eleven tests killed them, and every one of the six is a behaviour worth having a test for on its own.
- **And the harness found a design hole, not just a test gap.** The
  header's `reserved` field could be broken with nothing noticing,
  because nothing checked it — which is what a reserved field looks like
  from the outside. It is now refused unless zero, because a field that
  is ignored is a field a later format version can never use: packages
  would already be in the world with rubbish in it.

#### What it does not claim, written down rather than implied

- **No signatures.** Integrity, yes; authenticity, no. A signature needs
  a key, a key needs distribution, and distribution needs somebody other
  than the machine you are standing at. A key shipped in the same image
  as the thing it signs is a check that cannot fail, which is the exact
  fake check M65 refused to build. **Condition:** when a package can
  arrive from a machine this one did not build — `os install` fetching
  over M100's TLS. That milestone is the one that has somebody to trust,
  and it is the one that should pay for a trust root.
- **Not a defence against editing the disk.** `CAP_PKG_ADMIN` is a rule
  this kernel enforces while it is running.
- **Not a sandbox.** A package granted `fs-write` can write anywhere
  except `/pkg`. There are no file owners here, so "may write only its
  own directory" is not a sentence this OS can currently make true, and
  a check for it would be decoration.

#### Three failures the tests found that reading did not

- **The stale-archive index, which failed in the worst possible way.**
  Padding the manifest for alignment changed the format, so every archive
  built before it stopped verifying — correctly. `os-pkg index` opened
  the real index file, wrote its header, and died on the first bad
  archive, leaving a valid-looking index that listed nothing. The machine
  then reported `os: no package called 'grep'`, which is a true sentence
  about a broken repository and sends you to entirely the wrong place.
  Two fixes, both about honesty rather than about indexing: the index is
  written to a temporary and renamed, so a half-true summary never
  reaches the disk; and `os` now distinguishes *"the index lists no
  packages at all"* from *"the index does not have that one"*.
- **A static buffer in a recursive function.** `os`'s `remove_tree`
  collected a directory's names into a `static` array and then recursed,
  which overwrites the list the caller is still walking. A package one
  directory deep would have removed cleanly and one two deep would have
  left files behind, silently. It is a stack array now, and a directory
  with more than 64 entries is a loud refusal rather than a partial
  removal.
- **A registry that truncated instead of failing.** `rewrite_registry`
  dropped lines that did not fit its buffer. A missing line means a
  program launched with nothing when its manifest asked for something,
  and that symptom appears nowhere near the cause — so it is now an
  error, and an install whose registry cannot be written is undone.

#### One thing this milestone found that is not this milestone's

The default tier's four-core stage failed on the M111 tree. Attributing
it rather than assuming it — `git stash`, rebuild HEAD, run it six times
— gave **two passes and four panics at HEAD**, against three passes and
two failures with M111 applied (one of those a hang rather than a panic
- see *The machine as it stands* for why that is the same problem). So `tools/smp-test.sh` fails about two
runs in three and has nothing to do with packages; the panic is
`sched: this CPU is not on the stack of the task it thinks it is
running`.

That is recorded in *The machine as it stands* as a correction, because
this file said "twelve runs of the new one have not failed" and that is
no longer what this machine does. It also hands queue row 5 the cheapest
reproduction it has ever had: 21 seconds, two failures in three.

**The lesson, which this project keeps paying for and this time did
not:** the twenty minutes spent on `git stash` was the whole
investigation. A red stage in a tier run is not evidence about the
change in front of you until you have run the same stage without it.

#### Cost

`user_space/lib/{sha256,ospkg}.{c,h}` 730 lines, `user_space/bin/os.c`
900, `tools/os-pkg.c` 560, `kernel/proc/pkgcaps.{c,h}` 250, nine gated
syscalls in `syscall.c`, `user_space/bin/pkgtest.c` 300,
`tests/test_{ospkg,sha256}.c` 830, `tools/{build-packages,pkg-test}.sh`
420, `docs/packages.md`. Host tests 268 → 311, and `make mutate FILE=user_space/lib/ospkg.c` kills 22 of 36 buildable mutants. Boot markers 118 → 122;
`[m111]` costs **1.26 s** of the battery, and the graded boot is
122/122 with no panic at 393 s to desktop (ceiling 600).
Repository: grep 3.11 (1.2 MB), bzip2 1.0.8, and `impostor` — a fixture
that installs binaries called `compositor` and `shutdown` and must get
nothing, because a boundary with no adversary in the image is a boundary
nothing checks.

---

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

- [~] zlib, libpng, libjpeg, freetype, harfbuzz, expat, sqlite, ICU and a
      TLS library, each unmodified, in dependency order — every one a real
      test of M94–M97. **Eight landed: zlib 1.3.1 (2026-09-04), libpng
      1.6.44 and libjpeg 9f (2026-09-05), freetype 2.13.3, expat 2.6.4,
      sqlite 3.47.2, harfbuzz 8.5.0 and mbedtls 3.6.2 (2026-09-08)** —
      the last of them the TLS library, and **the first `https://` this
      machine has had**. Graded by `[m100]` through `[m100g]`: their own
      suites where they ship one, a differential fixture against the
      host's build where they do not, and for mbedtls all three (its own
      crypto suites, its own client and server completing a verified
      TLS 1.3 session on loopback, and an https GET written here). Zero
      source edits across all eight. **ICU is the ninth and last**, and
      it is a wall-clock question (a hundred times harfbuzz's build), not
      a capability one.
- [x] **TLS end to end over M66's TCP — done 2026-09-08.** mbedtls 3.6.2,
      `[m100f]`/`[m100g]`; a verified TLS 1.3 session (ChaCha20-Poly1305)
      and an https GET, over `kernel/dev/random.c`'s entropy and M100's
      blocking sockets. See the eighth increment.
- [~] `O_NONBLOCK` **done** (seventh increment: real on sockets and
      pipes, `F_SETFL`, `EAGAIN`), and blocking `read`/`write`/`accept`
      on a socket with it. **`AF_UNIX`/`socketpair` not done** — mbedtls
      did not need them, and nothing has yet; the note that `AF_UNIX` is
      what stopped CPython's `test_stat` from reporting counts stands.
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
- [x] **The four-core harness itself failed about one run in six** —
      M100's second increment quantified it and the third found the cause,
      which was the statistic and not the machine. It took the minimum
      one-task time and the minimum N-task time from *different rounds*
      and divided, which is biased upward without limit and produced
      readings as high as **491%** - more than one core doing all the
      work, which four cores cannot do. Paired per round and best-of-five
      now: 103-491% became **92-180%**, and 3 failures in 12 became 0 in
      23. The recorded scaling figure moved with it, in this project's
      favour - see the third increment's entry.
- [ ] **An intermittent stall on the exit path**, seen once, where a
      terminated task was still current a second after being reaped.
      **M100 gave this a reproduction signature**, which it did not have
      before: `tools/smp-test.sh` failing at exactly **240 s** - its own
      ceiling, with the `[m106]` marker never appearing - as opposed to
      the ~20 s failure that means the boot finished and the ratio was
      over budget. The second kind was the harness's own statistic and is
      fixed; this kind is the machine. Twenty-seven standalone runs did
      not reproduce it, one commit-tier run did, so whoever picks this up
      should expect to run the tier rather than the stage.
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
- [~] **`make -j4` does not compile at all — it spins.** This was
      recorded as *"costs far more than four times `make -j1`* — 203 s
      serially, still running at 600 s four ways. Four concurrent `cc1`
      processes are ~480 MiB of resident set against a block cache then
      serving four files. Stated as an observation, not a measurement:
      the run was stopped rather than finished."* **M100 bounded the step
      and took the measurement, and the guess was wrong in the most
      useful way.** With `measure -t 900`:

      ```
      [measure] bzip2-j1: wall 201880 ms  user 4197 cs  sys  5816 cs  peak-rss 119952 KiB  exit 0
      [measure] bzip2-j4: wall 900070 ms  user  510 cs  sys 43734 cs  peak-rss   2720 KiB  exit 124
      ```

      Two numbers say it is not slowness. **Peak RSS 2,720 KiB against
      `-j1`'s 119,952**: `getrusage(RUSAGE_CHILDREN)` reports the largest
      single child, and the largest child this build ever made is about
      the size of `make` itself — **`cc1` never ran**. And **437 s of
      system time against 5 s of user**, where `-j1` spends 42 s user to
      58 s system. It is not compiling and losing to memory pressure; it
      is in the kernel, in a loop, having launched nothing.

      98.8% system time with no forward progress is the signature of a
      poll that never completes, and the first thing to look at is the
      box directly below this one: make 4.4 cannot get a FIFO here, falls
      back to the **pipe job server**, and a job server that never hands
      out a token is a `make` that waits for one forever. That is a
      hypothesis with a named suspect, which is more than this box had
      before; it is not yet a diagnosis, because nobody has watched which
      syscall the 437 seconds are in.
      **This is now the sharpest handle row 5 has** — it reproduces on
      demand, in 900 bounded seconds, on one core, with no flakiness at
      all. Two independent runs: sys **43,734** and **43,695** cs, peak
      RSS **2,720 KiB** both times. A bug that repeats to four
      significant figures is a bug somebody can bisect.
- [x] **`tools/bootstrap-test.sh` had never passed, and M100's second
      increment is what noticed — fixed.** Its step 8 is `make -j4`, which the
      table in this very entry records as *"did not finish inside a
      40-minute ceiling"* — and the harness **requires** that step's
      `[measure] bzip2-j4` line plus the three `[perf]` rows the kernel
      prints only after it returns. `tests/budgets.tsv` has no row for
      any of the three, which is the proof: `build_wall_s`,
      `build_peak_live_tasks` and `build_peak_fds_one_task` have never
      been produced by a run. Two more ceilings were tried at 3,600 s
      and both timed out in the same place, with every other step
      *faster* than its recorded number (75.9 s against 86.9 for the C++
      unit, 188 s against 203 for `bzip2-j1`), so the machine is not what
      is slow.

      **Fixed by bounding the step rather than by lowering the bar.**
      `user_space/bin/measure.c` grew `-t SECONDS`: the command gets a
      deadline, is reported as `exit 124  UNFINISHED at the -t limit`
      when it hits one, and its wall-clock is deliberately *not* emitted
      as a `[perf]` row, because that number would be the limit rather
      than the cost. The script bounds step 8 at 900 s, and everything
      after it now runs — so `build_wall_s`, `build_peak_live_tasks` and
      `build_peak_fds_one_task` were produced **for the first time**, and
      the last two now have budget rows answering M98's third box. The
      harness grades the step on which of three things happened
      (finished, hit the deadline, or failed for some other reason), and
      only the third is an error.

      Two smaller things fell out of it. `boot_to_desktop_s` is no longer
      graded on this boot — the build runs before PID 1 on purpose, so
      that row was the build's wall clock wearing the ordinary boot's
      name, and it was failing against a 600 s ceiling with 1,237 s.
      And the bounded measurement is what turned the box above from
      "slow" into "spinning".
- [ ] **`mkfifo`.** GNU make 4.4 wants a FIFO for its job server, does not
      get one, says so, and falls back to the pipe job server — make's own
      supported path. Recorded rather than fixed; nothing else here has
      asked for a named pipe. **M100 promoted this from a curiosity to a
      suspect**: the fallback path is the one `make -j4` spins in, and
      "make's own supported path" is a claim about make rather than about
      this kernel's pipes. The box above is where that gets settled.

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
- **`AF_UNIX`/`socketpair` do not exist.** M100 absorbed `O_NONBLOCK` and blocking sockets but not these - nothing ported has needed them (mbedtls did not). Still what stopped CPython's `test_stat` from reporting counts.
- **The random device is real as of M100** (`kernel/dev/random.c`):
  ChaCha20 under a pool every interrupt feeds, fast key erasure on
  extraction, `getrandom` beside `/dev/urandom`. The xorshift-over-the-TSC
  it replaced is gone. RDRAND/RDSEED are used where the CPU has them and
  QEMU's default CPU has neither, so on this machine the interrupt timing
  is the whole source - stated in the header, graded by `[rng]`.
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
  scan does not move with the file count. (It moves with allocated
  *bytes* — 1,550 ms now, and that row is the closest thing to the second
  condition this file has. M100's second increment **confirmed the half of
  that condition that matters here and falsified the other half**: the
  scan really is not disk-bound — 311 of its 79,512 block reads reach the
  device — so the second clause still does not fire; but it is not linear
  in bytes either, and the number that grows is memcpy in the block cache,
  not I/O.) The reason the first clause cost
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
- **Package signing (M111).** Refused now for the reason M65 refuses every
  check with nothing behind it: a signature needs a key, a key needs
  distribution, and distribution needs somebody other than the machine
  you are standing at. A key shipped in the same image as the thing it
  signs verifies nothing. The three SHA-256s a package already carries
  are an *integrity* claim and are stated as one. **Condition: `os
  install` fetching a package over the network** — the first moment a
  package can come from a machine this one did not build, and therefore
  the first moment there is anybody to trust. M100's TLS client is
  already here, so this is a near thing; the milestone that adds the
  fetch is the one that pays for the trust root.
- **A sandbox for an installed package (M111).** A package granted
  `fs-write` can write anywhere except `/pkg`. "May write only its own
  data directory" needs a per-file notion of ownership this OS does not
  have, and a check for it would be decoration. **Condition: the same one
  multi-user names** — a second principal, or a program on this machine
  that holds something another program must not read.

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
- **A refusal only refuses when the caller looks.** This libc refused
  `PTHREAD_MUTEX_RECURSIVE` for two milestones, on M65's rule and with a
  comment predicting the exact deadlock a silent acceptance would cause.
  sqlite does not check what `settype` returns, and got the deadlock
  anyway — as its eighth syscall. A refused capability that a real
  program assumes is a capability that has to be built (M100).
- **A cross configure does not measure the libc; it is told.** libstdc++'s
  port case said "asks for nothing" for three milestones, so libstdc++
  defined its own `sinf` (and every C++ program that used the real one,
  once it existed, hit a duplicate symbol), and concluded `<math.h>` was
  not C99 (so `std::isnan` never existed here). Both were answers the
  port gave, not facts the configure found. When the libc grows, the
  port's case is a second place that has to be told (M100).
- **A rule about compile flags is only as current as the last artefact
  built under it.** M99 made `-fPIC` mean the small code model; the
  installed libstdc++ was a day older than that rule, and the M97 test
  kept passing against stale objects until a mutex grew and forced a
  rebuild — at which point every static C++ program that reached
  `std::runtime_error` stopped linking. The fix (`-fno-plt` in the
  target description) took an hour; the finding out took the rebuild.
- **An instrument can degrade the instrument beside it.** The mutation
  harness's timed-out mutants were orphaned processes, seven of them at
  full CPU for forty minutes, and the graded boot running alongside
  reported every disk number 3–10x its recorded value and timed out.
  Look at `uptime` before believing a slow boot (M100).
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
