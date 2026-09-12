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
| **Milestones** | M0–M117 numbered: 108 `[x]`, 4 `[~]` (M28, M92, M99, M103), 1 `[⊘]` (M80), 4 not started (M107–M110) |
| **Testing arc** | Q1–Q20 written, 18 `[x]`; Q7 half landed, Q14 not started |
| **Head of the queue** | **M100** — eight of nine libraries landed; M111 and M112 were both taken out of order at the user's request and closed 2026-09-09 |
| **Held by instruction** | all real-hardware work: M110, M28's last box, M108's link half, M103's two hardware-conditioned boxes |
| **Host unit tests** | 395/395 passing, 3 slow ones skipped in `--fast` |
| **Boot markers** | 128 required, graded on every self-test boot - M117's `[m117]` is the first to time a click through a client and to charge an idle desktop for its CPU |
| **Performance budgets** | 44 rows in `tests/budgets.tsv`, all inside their ceilings |
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

  **Re-measured 2026-09-10 during M114, and the rate is not stable:**
  one failure in five attempts at `632a4a8`+M114 — a timeout after
  `[smp] 00000004 CPU(s) online.` with the self-test never reaching its
  measurement, then three consecutive passes at 108%, 80% and 98%. So
  the failure is the same one and "two runs in three" is not a constant;
  whatever it depends on is not in this tree. It is still row 5's
  problem and it is still a reproduction on demand.

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
| **Host unit tests** (`tests/`) | libk, heap, malloc, leanfs, every network parser, the TCP state machine, PTY, symtab, UTF-8, fnmatch, getopt, fwcfg — and the **scheduler** (Q13), 2,157 lines against a fake timer and a fake CPU, with lock-order inversions made errors rather than comments. and, since M100, the **record-lock table** (`kernel/fs/flock.c`, mutation score 91.5%). and, since M112, `user_space/lib/fsutil.c` — the Files app's size formatting, name rule and recursive tree walks, the last of those against a real directory tree through the host's own filesystem. **338 tests**, ASan+UBSan, under a second | `--fast` |
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
| ~~**2**~~ | ~~**M100** — the browser gap, measured~~ | **engine and measurement done 2026-09-10** (ninth increment); ICU is the one library still open, and it is a wall-clock question rather than a capability one. The measurement's own conclusion opens the next row of this arc: `AF_UNIX` | the last milestone of its arc and the one that specifies the arc after it. **TLS lives here**, which is also the fetch M108's three TCP deferrals are conditioned on |
| **3** | **Q7 (2nd)** — the golden-frame baselines | half landed | the invariant half exists and has caught its bug; the baselines, the diff artifact and `make accept-visuals` do not |
| **4** | **Q14** — the compositor, off the machine | not started | the move Q13 made on the scheduler, on a 5,189-line file — taken *before* M107 puts USB input underneath it, so the rewiring is graded in milliseconds rather than only through a screendump |
| **5** | **M106 (tail)** — the battery green on four cores | 3 known failures, **plus `smp-test.sh` itself failing 2 runs in 3** (measured 2026-09-09 at HEAD — see *The machine as it stands*) | not a new milestone: the three failures M106 named and left. It gates CPU affinity, and there are now two reproductions on demand — M98's bootstrap profiler, and a 21-second `tools/smp-test.sh` that panics with "this CPU is not on the stack of the task it thinks it is running" about two attempts in three. **Start with the second one**. M112 adds a third, and it needs no second CPU: two graded boots in three stalled inside a `spawn`-then-`SYS_wait` self-test (`[m100d]` once, `[m100f]` once) on a single-CPU battery, eight to nine minutes with no serial output, while a third run of the same image was 123/123 in 410 s |
| **6** | **M103 (2nd)** — MSI/MSI-X | condition fires at row 7 | its own condition is *"a driver for a part that has no other way to interrupt"*, and M107's NVMe bullet answers it by name. The LAPIC-timer and interrupt-driven-virtio boxes are **not** collected here — they wait for hardware |
| **7** | **M107** — AHCI, NVMe, xHCI + USB HID | not started | all three gradeable under QEMU at this desk, which is what makes M110 a boot rather than a bring-up. Collects M92's last open box on the way |
| **8** | **M108 (driver half)** — a real NIC | not started | the `e1000e` driver is QEMU-gradeable and belongs here. The link half is held — see below |
| **9** | **M109** — lean_os built on lean_os | not started | needs nothing from rows 7–8 and stays after them anyway: the tree it rebuilds should be the whole tree, drivers included, or the generational test grades a subset of the machine it runs on |
| **10** | **M110** — the boot that has never happened | **held** | see the hold |

**M112 is not in this table either, and for the same reason.** The
Files app was asked for directly on 2026-09-09 — *"complete features to
create files, sort files, view file sizes, delete files and such"* —
and closed the same day. Three of those four already existed (M56, M59);
the missing one was that this window could not create anything at all.
It jumped nothing, and it changed nothing for any row below. What it
*did* change is worth one line: the recursive delete it needed lives in
`user_space/lib/fsutil.c` rather than in `SYS_rmdir`, so the kernel's
"empty directories only" rule is intact and now has a boot self-test
asserting that it stays that way.

**M113 is not in this table either.** *"Pre-install an open source
browser"* was asked for directly on 2026-09-10, and what it turned out
to need was not a browser — M100 had already built one — but the
discovery that nothing in this build put that browser on the image the
tests grade. It jumped nothing and changed nothing for any row below.
What it changed for the rows that remain is one sentence: **the input
suite was grading whatever image happened to be lying around**, and any
milestone that adds a payload to the disk now has a worked example of
how that goes wrong and one open box saying which four payloads still
do.

**M114, M115 and M116 are not in this table either.** All three were
asked for directly on 2026-09-10, about the same browser, and M116 is
the one worth a line here: the complaints had come back after two
milestones "fixed" them, because every instrument in the tree graded the
guest and none graded the path to the person - the NIC's receive ring
under a real stream, the host window, and the terminal a test run leaves
behind. It jumped nothing. What it changes for the rows that remain is
the rule for the next one: **a fix is not finished until something that
looks where the person looks can fail on it.**

**M117 is not in this table either.** *"The browser crashed on
apple.com... it is also extremely slow... everything should be
snappy"* was asked for on 2026-09-11, the morning after M116. It
jumped nothing. What it changed for the rows that remain: the desktop
under every row is one that sleeps now (every wmclient loop blocks in
`SYS_waitfds`, which M68 built for it and reverted), which makes Q14's
compositor-off-the-machine work smaller rather than larger; and the
browser has a **twin** (`tools/build-netsurf-host.sh`) and a
**backtrace**, so the next layout crash is a bisection and not a
guess. The bug itself was sixteen milestones old and one line:
`malloc(0)` returned NULL.

**M118 is not in this table either, and it is the first of these that
was not a complaint.** *"Systematically build the needed pieces so I can
install chromium later"* was asked for on 2026-09-11, and unlike M112-M117
it asked for work this file had already specified: `docs/browser.md`'s
five conditions, in its own order, the first of which its own measurement
called the smallest change with the largest effect. So M118 jumped
nothing and invented nothing - it closed an open box M100 had left
(`AF_UNIX`/`socketpair`) and took the arc that document opens one step.
What it changed for the rows that remain: nothing, except that the
deferred browser entry's condition has moved from `AF_UNIX` to **an
epoll-shaped readiness interface plus `eventfd` and `timerfd`**, and that
the measurement itself was corrected in one direction - the condition was
never Chromium's alone. WebKit, Gecko and Ladybird all pass descriptors
over a Unix-domain socket and none of the four has a single-process mode.

**M119 is not in this table either, and it is M118's own successor.**
Built 2026-09-12 under the same instruction, and the same reasoning
applies: `docs/browser.md` names five conditions in order, M118 closed the
first and promoted the second, and this is the second. It jumped nothing.
What it changed for the rows that remain is one thing worth knowing beyond
the browser arc: **this kernel has write-readiness now**, which it has
never had - `EPOLLOUT` means a pipe with room rather than `<poll.h>`'s
"anything open" - and the condition for making `poll` agree is a measured
case of a program spinning on the old answer. Row 4 (Q14, the compositor
off the machine) should read [docs/readiness.md](docs/readiness.md) before
it starts: the four ways to wait on this machine are now listed in one
place, and `SYS_waitfds` is still the right one for a window client.

**M120 is not in this table either, and it closes the arc M118 opened.**
Built 2026-09-12, third of three under one instruction. `memfd_create` was
one of the six absent syscalls M100's measurement singled out, and with it
the three pieces a multi-process program is made of - a channel, a wait and
a shared buffer - are all here. It jumped nothing. What it changed for the
rows that remain: `mmap_region_t` has a tag in it now, so **any future work
on mmap regions has a reference to hand over** (six call sites, three
helpers in sched.c, and a test that counts them); and there is a second way
to share memory on this machine, which Q14 should know about before it moves
the compositor - `kernel/ipc/shm.h` stays the right answer for a rendezvous
by name, and a memfd is the right answer when the authority should be the
descriptor.

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

### M112 — the Files app, finished: making things, and unmaking them `[x]`

**Landed 2026-09-09.** Asked for directly rather than taken off the
queue, the same way M111 was: *"I want you to work on the Files app on
the Desktop, I want it to have complete features to create files, sort
files, view file sizes, delete files and such."*

Three of those four already existed. Sorting has been in this window
since M59 (click a column heading, click it again to reverse), the size
and date columns landed in the same milestone, and rename, copy and
delete arrived in M56. What was missing was the first verb in the
sentence, and it was missing completely: **this window could not create
anything.** A file arrived on this disk from a text editor's Save box,
from a shell redirect, or from the boot self-tests; a folder arrived
from `mkdir` typed into a terminal. The Files app was a viewer with an
edit menu, and the first thing anyone tries in a file manager was the
one thing it refused.

- [x] **New File and New Folder** — `N` and `F`, or the menu. `OPEN_EXCL`
      rather than a stat-then-create, which is M87's own argument for why
      that flag exists.
- [x] **A right-click context menu** with all six operations and the key
      each one answers to. Three of them used to be reachable only by
      knowing that `R`, `C` and Backspace did something.
- [x] **Exact sizes.** The status strip describes the selected row —
      name, exact byte count, modified time — and `Get Info` shows the
      same for a folder as a recursive count of what is under it. The
      size *column* still rounds, by design; two files that both read
      "8.4K" are not the same file, and until now nothing in this window
      could tell you which.
- [x] **How much is left.** With nothing selected the strip reports the
      folder's item count and size, and both of this filesystem's
      ceilings from `SYS_statvfs` — free bytes *and* free inodes,
      because `os_fs.h` says in as many words that a tree of small files
      exhausts the second with most of the first still available.
- [x] **A delete that takes a folder with things in it**, behind a
      confirm that counted the tree first and names the number.
- [x] **Left and Right arrows** navigate. Leaving a directory used to
      take selecting row 0 and pressing Enter.
- [x] Graded three ways: **27 host tests** (`tests/test_fsutil.c`,
      against a real directory tree through
      `tests/fakes/fake_user_fs.c`), a **`[m112]` boot marker** driven by
      `user_space/bin/dirtest.c`, and **two input tests** that drive the
      whole create-fill-delete chain through real clicks and keys.

#### The one place this argues with an earlier decision

`SYS_rmdir` takes empty directories only, and its comment in
`system_api/include/syscall.h` says why: *"recursive delete is one
keystroke away from losing everything under a path, and this OS has no
trash to take it back out of."*

That is still right and the syscall is unchanged. What changed is the
other half of the sentence. A window that will **make** a folder but not
remove one it filled is half a feature, and the half it keeps is the
dangerous one — you can fill a disk from a GUI that offers no way to
empty it.

So the recursion lives in user space (`user_space/lib/fsutil.c`), and
what makes it acceptable is the thing the kernel could never have: a
confirm that ran `fsutil_count_tree` first and asks **"Delete folder and
41 items inside?"** rather than "Delete this folder?". The second
question is a button somebody presses. The first is one they can answer.

`/bin/dirtest` checks that `SYS_rmdir` still refuses a full directory,
so if the kernel ever relaxes that rule the dialog stops being wrong
quietly.

#### The bug that was already there, found by writing the tests

`selected` was an **index into a list that is rebuilt every second**.

The window re-lists on a timer so that a file saved from another window
shows up. Each rebuild overwrote `names[]` in place and then clamped
`selected` to the new count — so a file appearing or vanishing anywhere
*above* the selection silently moved it onto a different file. A second
Files window copying something, an editor saving, the sort order
changing: any of them. The next Delete then showed its confirm about the
row you were looking at and removed the one the index had drifted onto.
Nothing on screen said the moment it happened, which is the whole shape
of it.

The fix is four lines — carry the *name* across the rebuild and find it
again — and it is what makes "create a folder and it is already
selected" work at all, which is how it was found. A milestone about
creating files found a bug about deleting them.

Two smaller ones beside it:

- **A prompt owned the keyboard and not the mouse.** The comment has
  said "an open prompt owns every keystroke" since M56. Clicking another
  row while a Rename box was open moved the selection, and Enter then
  renamed the row that had been clicked using the name prefilled from
  the row that had not. Prompts now swallow mouse events too.
- **`Copy to:` truncated.** It opened with the source's own name
  prefilled, and `OPEN_TRUNCATE` on Enter — one keystroke from a
  whole-file truncation of whatever already had that name. `OPEN_EXCL`
  now, like the create path.
- **The hint line named a key that does not exist.** It said "Del
  delete" for fifty-six milestones; `kernel/drivers/keyboard.c` decodes
  `0x0E` to `'\b'` and has no entry for the Delete scancode at all. The
  menu says `Bksp`.

#### The instrument, and why it is a fake filesystem made of real files

`tests/fakes/fake_user_fs.c` backs `SYS_getdents`, `stat`, `unlink`,
`rmdir` and `mkdir` with the **host's own filesystem** under a per-test
`mkdtemp`. It follows `fake_user_syscalls.c` (M98), and the reason for
the shape is the assertion: the only useful thing to say about a
recursive delete is *"is it gone"*, asked of something that is not the
code that deleted it. A model filesystem written for this test would
answer with the same assumptions the walk was written under — which is
exactly the failure `make mutate` exists to catch. The host's `stat` has
no such assumptions.

It hands back at most three entries per call even when more would fit,
because the batching *is* the part of the contract the walk has to
survive: a fake that always returned a whole directory would never once
exercise the loop that re-reads.

That still leaves questions only the machine can answer — whether leanfs
marks a directory the way the walk expects, whether a cookie survives
the entries under it being unlinked, whether a path assembled one
component at a time is one this resolver still accepts. `/bin/dirtest`
asks those, on leanfs, in 70 ms.

#### What was deliberately not built

- **Multi-select.** Every operation here acts on one row. Bulk delete is
  the obvious next thing and it needs a selection model, a confirm that
  can describe a set, and three tests; nothing has asked for it.
- **Recursive copy.** M59's note stands unchanged: a directory copy is a
  different operation with its own failure modes.
- **A trash.** Refused for M56's reason, which the counted confirm makes
  less pressing rather than more.
- **Remembering the sort order across launches.** `settings_file.c` is
  the desktop's theme file and a per-app preference does not belong in
  it. It would need a place for one, which is a milestone about
  preferences and not about files.

#### Cost

`user_space/lib/fsutil.{c,h}` 407 lines, `user_space/bin/dirtest.c` 154,
`tests/test_fsutil.c` 353, `tests/fakes/fake_user_fs.c` 205,
`user_space/bin/file_manager.c` 1,035 → 1,709, plus the kernel's `[m112]`
block and two input tests. Host tests **311 → 338**; boot markers
**122 → 123**, `[m112]` costing **70 ms** of the battery. The graded boot
is **123/123 with no panic**, every one of the 39 budgets inside its
ceiling, at 396 s to desktop (ceiling 600). The two new interactive tests
pass in 15 s and 18 s, and one of them is in the pre-commit `--quick`
subset, where it runs **10/10** beside the nine tests that were already
there — including `file_manager_navigates_directories`, which is the one
this milestone's change to how the selection is tracked could have
broken. The **whole input suite is 52/52** in 344 s, and the host tier
with the slow tests in is **341/341**.

Hand-mutated rather than left to `make mutate`, which does not target
`user_space/lib`: dropping the `'/'` from the name rule, making
`remove_at` not recurse, moving the decimal threshold from 10 to 100, and
making `count_at` not descend. All four are killed, three of them by
exactly one test each.

#### Two graded boots in three stalled, and it is row 5 again

Worth recording precisely, because the first reading was wrong and the
correction is the useful part.

Three graded boots were run on a fully populated image. **Two stalled**:
the serial log stopped dead for eight and nine minutes with QEMU pinned
at 100%, once inside `[m100d]` and once inside `[m100f]`, and both were
killed by `qemu-serial-test.sh`'s 900-second ceiling with thirty-five
markers still to come. The **third is 123/123 with no panic**, every one
of the 39 budgets inside its ceiling, and it took **410 seconds of wall
clock** — 396 s of that to the desktop.

The tempting explanation was that 900 s had simply stopped being enough
for an image carrying toybox, the toolchain, Python and nine libraries.
It is wrong, and the third run is what disproves it: 410 s is less than
half the ceiling. The two that stalled were not slow. They stopped.

**The second and third runs used the same image and the same kernel.**
Nothing was rebuilt between them. So this is intermittent, and where it
stops says which class it is: both `[m100d]` and `[m100f]` spawn a child
and `SYS_wait` on it, which is the shape of queue row 5 — Q13's "about
one boot in ten hangs", the reason the scheduler was moved off the
machine in the first place.

**It is not this milestone's.** The stalls are in stages M112 does not
touch, the same kernel completed a full battery earlier the same day on
an image without the third-party programs, and the third run is green.
But two in three is a much worse rate than the one in ten row 5 was
written about, so it is recorded here rather than dismissed: **whoever
takes row 5 now has a second reproduction beside `tools/smp-test.sh`,
and this one needs no second CPU.** Both stalls followed a period of
heavy host load, which is the variable to try first.

**The harness was right and was not believed.** Its own failure line
says "log capture ended too early, or a real regression - try a longer
SECONDS first". Read it as the first suggestion rather than as a
diagnosis: a stall and a slow boot produce the identical message, and
telling them apart costs one more run and a look at whether
`boot_to_desktop_s` ever printed at all.

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

### M100 (ninth increment) — a browser, and the gap the rest of one leaves `[x]`

*Landed 2026-09-10.* M100's fifth and sixth bullets together, which are
the last two that were not about a library: **a real but small engine**,
and **the measurement, which is the deliverable**. Both are written up
in [docs/browser.md](docs/browser.md); this entry is what it cost and
what went wrong.

**Asked for directly** — *"I want include a google chrome web browser
with this OS pre-installed"* — on 2026-09-10, which is why it jumped the
queue's row 3 and 4. It jumped nothing else: no row below was started
and abandoned. Chrome itself is not a porting problem (it is
proprietary, ships only glibc/GTK/X11 binaries, and shipping somebody
else's binary is what CLAUDE.md's first non-negotiable forbids), so what
was built is what M100 already had scheduled for exactly this request.

**What runs.** NetSurf 3.11 as `/bin/netsurf` and as the **Browser**
icon on the desktop: libhubbub parsing HTML5, libdom, libcss doing the
cascade, Duktape running JavaScript with bindings generated from WebIDL,
freetype rasterising, libpng/libjpeg/libnsgif/libnsbmp/libsvgtiny
decoding, and **libcurl 8.11.1 over mbedtls 3.6.2 over M66's TCP** for
http and https. Fifteen third-party projects, **zero edits to any of
their source**. Graded by `browser_renders_a_page` in the input suite —
real clicks, real framebuffer pixels — because a layout engine's output
*is* pixels and a browser that painted nothing would still exit 0.

**The thing worth taking from this milestone is the seam, not the
browser.** The port needed no NetSurf front end, which would have been a
permanent fork. libnsfb registers surfaces *at runtime*, NetSurf selects
one *by name* at runtime, and it links libnsfb with `--whole-archive` -
three decisions upstream made for its own reasons, which together mean
one file compiled here (`user_space/bin/nsfb_leanos.c`) and added to
`libnsfb.a` is the entire display port. And the pixels are zero copy:
libnsfb's `NSFB_FMT_XRGB8888` is byte-for-byte this compositor's own
`0x00RRGGBB`, so the layout engine renders straight into the shared
segment.

**Five gaps in this system, each named by a build rather than a
checklist** — M63's rule at M100's scale, and worth more than the
browser:

- **`pread`/`pwrite` did not exist** (libnsutils, for NetSurf's disc
  cache). Now `SYS_pread`/`SYS_pwrite`, 111 and 112, and *honest* rather
  than an lseek sandwich - which matters because the program that asked
  has two threads behind one descriptor. It cost two dispatch entries
  and no new machinery, because `vfs_handle_read` had always taken the
  offset as an argument: the open-file entry's `offset` field was the
  only thing in the way.
- **The `lround` family did not exist** (libsvgtiny calls `lroundf`).
  The usual shape of a libm gap: `round()` had been here since M99 and
  its four integer spellings had not. Grading them needed a **new kind
  of row** in `tests/math/cases.tsv` - the first functions in
  `<math.h>` that do not return a floating-point type - and the whole
  table in `tests/math/main.c` converted to designated initialisers,
  because `-Wmissing-field-initializers` is right to refuse the shorter
  lie.
- **`scandir`/`alphasort` did not exist** (NetSurf's `file:` fetcher —
  it is how a browser shows you a folder).
- **`STDIN_FILENO` did not exist** (curl's `terminal.c`). fd 0 has been
  stdin since M14; the three names POSIX gives them had never been
  written down. Same shape as M94's `wcwidth`.
- **`<iconv.h>` did not exist at all**, and NetSurf includes it
  unconditionally. So this libc has an iconv now: 26 single-byte
  charsets from a generated table plus the UTF family, 3,328 entries.

**And a sixth, which is not a missing function and is the sharpest thing
this milestone found.** `system_api/include/signal.h` and
`user_space/libc/include/signal.h` **have the same name**; the libc one
is found first and reaches the other with `#include_next`, which works
only because `usr/local/include` precedes `usr/include` in the default
search order. curl's `configure` adds `-isystem <sysroot>/usr/include`
when told where mbedtls is, the kernel ABI's `signal.h` moves in front,
`sigset_t` vanishes, and `<setjmp.h>` stops compiling - in a file that
mentions neither signals nor mbedtls. **Any third-party build that names
the sysroot's include directory hits this.** It is worked around in
`tools/build-netsurf.sh` and it is *not fixed*; the fix is to stop
having two headers with one name, and it is now an open box below.

**What the iconv test found, which is a fact about differential testing
rather than about iconv.** `tools/iconv-test.sh` compares this project's
iconv against the host's over every byte of every charset and every code
point in the BMP. Its first run reported **144,589 disagreements** and
almost none of them were bugs: **macOS's iconv transliterates by
default**, so asked for U+0100 in ISO-8859-1 it returns `A` where GNU
libiconv returns EILSEQ - and returns 0 rather than POSIX's count of
non-reversible conversions, so a caller cannot even detect it. The host
is not one oracle. Rebuilt to derive each charset's true repertoire from
the host's *decode* direction (which is exact and non-transliterating)
and then require this libc to encode every character in it and refuse
every character outside it, the count went to **13** - and all thirteen
were one real bug: **U+FFFF shared a value with the table's "unassigned"
sentinel**, so encoding it produced the first hole in each charset, in
thirteen charsets at once. Fixed, and the fast tier now runs 2,580,532
conversions with zero disagreements.

**Three bugs this milestone's own code had, and how each was found -
which is more useful than the list of what worked.**

- **The mouse buttons were numbered wrong, and no test would have said
  so.** lean_os reports bit0 left, bit1 right, bit2 middle; libnsfb
  inherited X11's numbering, where 2 is MIDDLE and 3 is RIGHT - and
  NetSurf acts on MOUSE_1 and MOUSE_3 and ignores MOUSE_2 entirely. The
  obvious `NSFB_KEY_MOUSE_1 + bit` therefore dropped every right-click
  and turned middle-click into a right-click. Both symptoms read as "a
  new port doesn't do right-click yet", which is exactly the kind of
  thing nobody goes looking for a numbering bug about. Found by reading
  NetSurf's own switch statement rather than by any instrument.

- **The BOM handling was wrong and untested.** `iconv_open("UTF-8",
  "UTF-32")` on a big-endian stream: the leading BOM read with the wrong
  endianness is the bytes `00 00 FE FF`, which decodes to 0xFFFE0000 -
  larger than U+10FFFF, so the decoder correctly refused it and the
  stream failed with EILSEQ instead of simply being the other byte
  order. **Deciding endianness is not a question a Unicode decoder can
  answer, because the bytes are not Unicode yet**; it now happens on raw
  bytes before decode() sees them. The reset path was wrong too - it
  could not put the endianness back, because nothing recorded what it
  had been. Found by re-reading the code, then given eight test rows so
  it stays found.

- **Two of the three assertions in the new input test were vacuous.**
  The first version counted "blue pixels" and "dark pixels beside white
  ones" over the whole screen. This desktop's icons are *already* blue -
  3,950 such pixels on a bare desktop, against a threshold of 500 - and
  its icon labels are *already* white text with dark around them (254,
  against 200). **Two of three assertions passed with no browser
  running at all.** Found by measuring a bare desktop and an open
  Editor against the test's own thresholds, which is the only way that
  hole is ever found and is the same thing Q12's mutation harness
  exists to do for the tests that grade the kernel. Rebuilt to anchor on
  the columns where white is *dense* - a region that does not exist
  unless a page is on the screen - the three numbers became 1,618 /
  1,618 / 292,519 white, and 0 / 0 / 111,754 blue.

  And the rebuilt version bought something the first one could not: it
  compares **blue against red inside the page** (111,754 to 465). A
  surface with red and blue swapped is the single most likely way
  `nsfb_leanos.c` could be wrong *and still look plausible* - a browser
  full of orange lays out and scrolls perfectly - and now those two
  numbers trading places is a failure.

**The self-test was mutation-checked rather than assumed, because its
whole subject is a property that is easy to test vacuously.** Two
deliberate breaks of `sys_pread`, each rebuilt and booted:

- *reads from the descriptor's own position instead of the offset* →
  `[m100h] browsertest exited 5`, kernel panic
- *reads from the right place but ALSO advances the descriptor* →
  `[m100h] browsertest exited 7`, kernel panic

The second is the one that matters: it is the exact behaviour an
`lseek`/`read`/`lseek` implementation would have, it produces the right
bytes every time, and it is wrong. Each was caught by the specific
assertion written for it rather than by something incidental.

**And one thing the ninth icon found about this desktop, which is not a
bug.** `test_double_click_launches_every_icon` failed at 8 of 9 windows
and the browser was **on the screen, fully rendered, at the front** in
the failure screendump. The taskbar lays its running-app buttons out
left to right and stops when the next one would reach the tray - at
1024x768 that is **eight** - so `count_app_windows`, which reads taskbar
slots, saturates. The Browser icon is the first thing on this desktop to
reach that ceiling, and it took a screenshot of the failure to see that
the assertion was wrong rather than the machine. The test now knows the
capacity (computed from desktop_shell.c's own geometry rather than
typed) and, past it, asserts that the screen changed instead.

**Two host-portability edits to NetSurf, neither about lean_os**
(`tools/netsurf-port/apply.py`): `/bin/which`, which macOS does not
have, and `echo -n`, which this desk's `/bin/sh` prints instead of
honouring. The second bites only on a *rebuild* - the first build writes
a malformed `link.d` and links fine, the second reads it and stops at
`missing separator` - which is why it is an anchored edit rather than a
note.

**An upstream bug this build now watches rather than patches.** NetSurf
picks its default surface with `if (type < fetype)` where `fetype` is
never assigned, so the *last* surface to register wins - meaning link
order decides, silently. On the wrong side of that the browser comes up
on the `ram` surface, renders a whole page into memory nobody displays,
and shows an empty window with **no error anywhere**.
`tools/build-netsurf.sh` now reads `.init_array` out of the linked
binary and fails the build if this project's surface is not last. Its
first version failed for a reason worth recording: `readelf -x` dumps
**raw bytes** grouped in fours, not 32-bit words, so reconstructing the
pointers the obvious way produced plausible 64-bit numbers that matched
no symbol - and the check reported "no surface constructors found"
rather than a parse error. A check that cannot fail correctly is worse
than the thing it checks.

**Where the tiers stand at this commit.** `--fast` 10/10 (the new
`iconv` stage included), the graded boot **124/124 markers with no
panic** and every budget row inside its ceiling, and the **full
interactive suite 53/53** including the new `browser_renders_a_page`.
The four-core stage is red, and it is red for the reason row 5 of the
queue already says it is: three runs here were **fail, fail, pass**, at
exactly the 240 s ceiling-timeout signature with `[m106]` never
appearing, which is the rate and the signature recorded at HEAD on
2026-09-09. Not this milestone's, and recorded here because a tier that
went red on the way to a commit should say so in the commit's own entry.

**What it cost.** Two host tools this project had not needed
(`bison` 3.x - macOS ships 2.3, from 2006, which cannot parse
libnslog's grammar - and the host's libpng, for NetSurf's own
PNG-to-C-array build tool). Both dev-time only. `/bin/netsurf` is
**7.0 MB stripped** (20.1 MB unstripped) and the resources another 19
files; the whole image went from 361 MB with room to spare.

**And the measurement, which is the deliverable.** Numbers rather than
an estimate, all of them produced on 2026-09-10 by fetching Chromium's
own source and docs and comparing against this machine. The full write-up
is in [docs/browser.md](docs/browser.md); the four that decide it:

- **≥ 8 GB RAM, ≥ 32 GB swap, ≥ 100 GB disk**, from Chromium's own
  `build_instructions.md`. This machine passes its battery on 128 MiB,
  has **no swap at all** (M102 refused it on a number), and its whole
  disk image is **2 GiB**. The minimum is thirty-seven times the image.
- **535 sub-repositories** in `DEPS` (5,197 lines). The entire NetSurf
  stack that runs here is fifteen, and lean_os itself is 147,389 lines.
- **`clang` and `libc++` are the only supported compiler and STL**, in
  its own documentation. This project's toolchain is GCC 14.2 with
  libstdc++, so M94's nine-edit port would have to be done again.
- **Chromium's seccomp sandbox names 427 syscalls. This kernel has
  113.** 61 match by name and 18 more under a different spelling
  (`wait4`/`SYS_waitpid`, `rt_sigaction`/`SYS_sigaction`,
  `getrusage`/`SYS_rusage`, …), so the honest overlap is **79** and the
  gap is **348**.

348 is a large number and it is not the point. **`AF_UNIX` with
`SCM_RIGHTS` is.** Chromium's entire IPC layer is Mojo over a UNIX
socket passing file descriptors between processes, and this kernel has
no `AF_UNIX` at all - so without a few hundred lines there is no
Chromium, not a slow one and not a limited one. It is the smallest
change on that list with the largest effect, M100's own entry already
had `AF_UNIX` open for a *different* reason (CPython's `test_stat`), and
**it is what the next milestone in this arc should be.** That is a
condition, which is what this project means by a deferral, and it is the
first time the browser row has had one.

**M118 built it (2026-09-11), and corrected the paragraph above in one
direction.** The numbers are left as they were measured because a
measurement with a date on it should be added to rather than edited. What
was wrong was the framing: this was never Chromium's condition. WebKit's
`IPC::Connection`, Gecko's IPDL and Ladybird's LibIPC pass descriptors
over a Unix-domain socket too, and none of the four engines has a
supported single-process mode any more, so one kernel feature stood in
front of all of them.

**And M119 built the second (2026-09-12)** - `epoll`, `eventfd`,
`timerfd`, which is `base`'s whole message pump. **M120 then built
`memfd_create`**, one of the six absent calls the table singles out, so the
three pieces a multi-process program needs - a channel, a wait and a shared
buffer - are all here. Between them this kernel has **128 syscalls**; twelve of the 348 have moved, leaving an overlap of
91 and a gap of 336. What has not moved is the part that decides the rest:
`signalfd4`, `seccomp`, `prctl(PR_SET_SECCOMP)` and `clone` with a
namespace flag - the first a feature nothing in `base` calls, the other
three the sandbox. **The condition for this row is now the
third item on that list: clang and libc++ for `x86_64-lean_os`**, which is
a compiler rather than a few hundred lines of kernel - and that is the
honest shape of the remaining three.

### M113 — the browser, actually installed `[x]`

*Landed 2026-09-10.* **Asked for directly** — *"I previously ask you to
pre install google chrome, you told me it was impossible, now I want you
to pre-install an open source browser instead"* — the day after M100's
ninth increment answered the first half of that sentence. It jumped
nothing: no row of the queue was started and abandoned, and every row
below is where it was. An instruction outranks the table, the same way
M111 and M112 did.

**The browser already existed. It was not installed.** That distinction
is the entire milestone, and the first thing this milestone did was
measure it rather than assume it: an independent reader
(`tools/leanfs-fsck.py`) walked `build/os-image.bin` as it stood at
HEAD and reported `/bin/netsurf` **missing**, along with `/bin/toybox`,
`/bin/python3` and `/pkg`. The image a person would have booted that
morning had no browser on it.

**Why it had none, which is a fact about the build rather than about
NetSurf.** `$(IMAGE)`'s recipe is `cat $(MBR_BIN) $(KERNEL_BIN) >
$(IMAGE)` followed by `truncate`. It runs whenever the kernel changes,
and it **wipes leanfs completely** — every ported payload with it. M100
installed the browser onto whichever image happened to be sitting in
`build/` at the time, and nothing put it back afterwards. The
demonstration arrived unprompted during this milestone's own work:
editing the `Makefile` rebuilt the kernel, recreated the image, and the
very next `leanfs-put` printed `created directory /bin` — a filesystem
with nothing in it.

**What that did to the instrument, which is worse than what it did to
the desktop.** `browser_renders_a_page` in the input suite is the only
thing in this project that can grade a layout engine at all — M100's own
entry says so, and it is right: a browser that started, fetched, parsed,
laid out and then painted nothing would exit 0. **Nothing between
`make all` and that test ever put a browser on the image it runs
against.** `tools/run-tests.sh` has a careful, commented population
sequence — toybox, the cross-compiled fixture, the dynamic loader, C++,
the native binutils, CPython, the package repository, each one a stage
of its own with a paragraph saying why — and NetSurf was not in it. The
test passed because a browser had been installed by hand and the kernel
had not been touched since. **The first person to edit the kernel and
run the suite would have got a browser that painted nothing, and that
failure reads exactly like a rendering bug in `nsfb_leanos.c`.** A
milestone-long debugging session was sitting in the tree waiting for
somebody to start it.

**What landed.**

- **`make browser`** — build the port if it has never been built, and
  install it either way. One command, because two script names nobody
  remembers is operationally the same thing as no browser.
- **`make browser-if-built`** — the install half alone, which says so
  and *succeeds* when the port has never been cross-built. This is the
  one the harnesses call, so a test run never starts a twenty-minute
  cross-compile inside itself.
- Both depend on **`preseed`**, which is the same answer `toybox` and
  `packages` already give to the inode question CLAUDE.md names: writing
  a third-party file into a never-booted image claims the first free
  inode, and if that is slot 0 it shifts `hello` out of the launcher and
  fails M22's pixel self-test. Neither is part of `all`, for that reason.
- **`tools/run-qemu.sh` and `tools/run-tests.sh` both run
  `browser-if-built` after `make all`**, which is what makes the browser
  survive a kernel rebuild. The reinstall is **0.13 s** measured, against
  the twenty minutes a rebuild of the port costs — so doing it on every
  boot is cheaper than deciding whether to.
- **The `[m113]` boot self-test**, and it is a *presence* check on
  purpose. Two instruments already grade what NetSurf does; none graded
  whether it was there. It asks the machine four questions:
  `/bin/netsurf` at a plausible size, `default.css` where the compiled-in
  `NETSURF_FB_RESPATH` looks, `DejaVuSans.ttf` where
  `NETSURF_FB_FONTPATH` looks, and `caps_for_program("netsurf")` still
  `CAP_FS_WRITE | CAP_NETWORK` with `CAP_FRAMEBUFFER` **absent**.
- Docs: `docs/browser.md` gains the install section, and CLAUDE.md and
  README.md now say `make browser` instead of naming two scripts.

**The capability line in that self-test is the one worth defending.** A
browser that had quietly acquired `CAP_FRAMEBUFFER` would render
perfectly, and `browser_renders_a_page` would still pass — pixels cannot
tell you *whose* authority drew them. docs/browser.md's claim is that
twenty megabytes of somebody else's C and C++ running a JavaScript
engine holds one bit more than the text editor does. That sentence now
has a test under it.

**Both branches were run rather than reasoned about**, which is the only
reason this entry can claim either.

- **The skip.** A fresh image, no payloads: `[m113] /bin/netsurf is not
  on this image - skipped. \`make browser\` builds and installs it` —
  and no panic. This matters more than it looks: the marker is
  *required* by `tools/qemu-serial-test.sh` on `[m98]`'s and `[m99]`'s
  reasoning (run-tests.sh installs it before the boot), so the skip line
  and the pass line must never be confusable, and they are not.
- **The catch.** `/bin/netsurf` replaced with a 21 KiB `hello`, which is
  what a truncated `leanfs-put` or a strip that ate the file looks like
  to `stat()`: `[m113] /bin/netsurf is there but is not a plausible
  browser: kind 0, 21 KiB`, then the panic. The test is not vacuous, and
  it says the number it found rather than only that it was unhappy.

**What this cost:** one Makefile target pair, two harness call sites, one
kernel self-test, and four boots — one to prove the marker, one to prove
the skip, one to prove the catch, and the graded battery. No new
subsystem, no third-party source touched, and not one line of NetSurf.

**And what it is really about.** M100's entry says *the thing worth
taking from this milestone is the seam, not the browser*. M113's is
smaller and duller and has probably cost this project more time in
aggregate than any seam: **a build artifact that is not reproduced by
the build is not part of the system, however carefully it was made.**
The browser was the case that made it visible because it had a test that
could quietly grade the wrong image. Toybox, the native toolchain,
CPython and the package repository are all reinstalled by
`run-tests.sh` and *none of them* is reinstalled by `tools/run-qemu.sh`
— so the machine a person boots to *use* still has only a browser put
back. That is now the open box below, and it is one line of the same
shape repeated four times, deliberately left for a milestone that can
measure what it costs a boot rather than folded in here on the
assumption that it is free.

- [ ] **The other four payloads, on the same terms.**
      `tools/run-qemu.sh` reinstalls the browser and nothing else, so a
      `make run` after a kernel edit still boots a desktop with no
      toybox, no `python3`, no native toolchain and no `/pkg/repo`.
      **The condition**: a measurement of what each reinstall costs a
      boot. The browser's is 0.13 s and that is why it went in without
      one; CPython's is 596 files and the native toolchain's is 105 MB,
      and neither should be added to every `make run` on the assumption
      that it is as cheap.

### M114 — a resolver that does not depend on one server `[x]`

*Landed 2026-09-10.* **Asked for directly**, the same day M113 landed —
*"the qemu screen are very small now. and the browser doesn't work, and
it looks like it's from 1990s. can't we port a working open source
browser somewhere?"* Three complaints. Two were the same bug wearing
different clothes, one was a bug in this OS that had nothing to do with
browsers, and the third is answered at the end of this entry with
numbers rather than an opinion.

**"The browser doesn't work" was not about the browser.** It was
diagnosed by measurement rather than by reading code, and the sequence is
the useful part of this milestone:

| what was asked | what it said |
|---|---|
| open Browser, load `file:///usr/share/netsurf/welcome.html` | rendered perfectly — CSS, PNG, freetype text |
| load `http://example.com/` | status bar stuck on `Loading`, forever |
| load `https://example.com/` | **`Could not resolve hostname`** |
| `netconf` on the machine | `10.0.2.15/24 via 10.0.2.2, DNS 10.0.2.3 (DHCP lease)` |
| `nslookup example.com` | `no reply from the server (timed out)` |
| the guest's own packets, through QEMU's `filter-dump` | ARP resolved 10.0.2.3, then a **well-formed** `A? example.com.` left twice, one second apart, and nothing came back |
| `nettime 216.239.35.0` | `says 2026-09-10 23:07:38 UTC` — **UDP to the public internet was working** |
| the host's first nameserver, from the host | answered nothing. Its second one answered instantly |

So the link worked, ARP worked, UDP in and out of the guest worked, the
query was well-formed — and every name on the machine was unresolvable,
because **the one server it was allowed to ask was a router that accepted
queries and dropped them.** The browser was reporting, accurately, a
failure that belonged to this OS's resolver: `dns_resolve` asked
`conf.dns` and nothing else.

**A resolver with one server cannot tell "the network is down" from
"this server is broken", and cannot recover from the second.** That
sentence is the milestone.

**What landed.**

- **`/etc/resolv.conf`**, parsed for `nameserver` lines and nothing else
  — `search`, `domain` and `options` are a feature each, and a parser
  that silently ignored a directive it did not implement would be lying
  about what the machine will do.
- **Every configured server asked at once**, first correct answer wins,
  file before DHCP. Parallel rather than in turn because a sequential
  resolver pays a dead server's *whole* timeout before reaching a live
  one — three seconds and then a failure, on this machine, where the
  other server would have answered in milliseconds.
- **The file is seeded by the kernel on first boot**, not written by the
  build. That is M113's lesson applied one milestone later: anything the
  build writes into leanfs is destroyed the next time the kernel changes.
  A first-boot file comes back on its own.
- **`nslookup` prints the servers it actually asked.** It printed one
  address before this milestone and that address was the problem, which
  is why the bug needed a packet capture to find.
- **`tests/test_dns.c`** — 21 cases against
  `tests/fakes/fake_user_net.c`, a network with no network in it whose
  servers can be told to *accept every query and answer none*. That
  behaviour cannot be arranged against a real server, and pointing at an
  address nothing listens on grades ICMP-unreachable instead of silence.
  `user_space/lib/dns.c` had no host tests at all before this.

**Three bugs this milestone's own code had, all found by running it.**

- **An out-of-bounds read, in the fix.** `sys_readfile` returns the
  *file's* size and copies at most `maxlen`; the first version handed
  that number straight to the parser over a 512-byte buffer, and the
  seeded resolv.conf is 675 bytes. The parse ran off the end, found no
  nameserver, and the machine silently went back to asking the one
  server this milestone exists to stop trusting — *the symptom of the
  fix failing was identical to the symptom of the bug.* There is now a
  test with an 8 KB resolv.conf whose only job is to be larger than the
  buffer, and ASan is what makes it a failure rather than a plausible
  answer.
- **The tests were grading the cache.** Four of them passed while
  reporting "queries seen: expected 1, got 0" — a resolver that had not
  sent a packet and still returned the right address, because
  `dns_resolve`'s cache is one static array shared by every case in the
  process. `dns_cache_clear()` exists because of this.
- **One test's premise was wrong.** It expected a DHCP server's
  NXDOMAIN to beat another server's address; resolv.conf is asked
  *first*, so the address arrived first and won. That is a real semantic
  choice rather than an accident, so it is now two tests — the negative
  answer, and the mixed case with the ordering written down.

**And a fourth, which is the most useful thing here and is not about
DNS.** The fix worked on the machine's own programs and the browser kept
reporting `Could not resolve hostname`. **NetSurf is a 20 MB static
binary: this project's libc is inside it.** `build/libc.a` was built
only as a prerequisite of `make sysroot` — never by `make all` — so it
was three hours stale, and every ported program was linking a C library
from before the fix. Nothing said so; it looked exactly like the fix not
working. Now `all` builds `$(LIBC_A)` and `$(NETSURF_BIN)` depends on
it, so a libc change relinks the browser.

Writing that prerequisite also produced a small, perfect example of the
same class of mistake: `all: $(IMAGE) $(LIBC_A)` at line 217 expands
`$(LIBC_A)` to **nothing**, because it is a `:=` variable defined at
line 756. The line silently said `all: $(IMAGE)` and the archive stayed
stale for one more build. The prerequisite now lives next to the rule.

**`make sysroot` is not the way to refresh it, and that is worth
knowing before somebody tries:** its recipe begins `rm -rf $(SYSROOT)`,
which would delete the fifteen third-party libraries installed there by
their own `make install` and cost a twenty-minute rebuild of the whole
browser stack.

**The screen, which was the same wipe again.** *"The qemu screen are very
small now"* — 1024x768, the firmware's mode. The desktop has changed
resolution live since M58 and remembers it in `/etc/settings.conf`, and
`make` had deleted the filesystem that file lived in. `QEMU_RES=1440x900
./tools/run-qemu.sh` now re-applies it from outside the image on every
run (`tools/set-resolution.sh`), which is the same answer M113 gave for
the browser. **The compiled-in default stays 1024x768 on purpose**:
every coordinate in `tools/qemu_input_suite.py` is measured against that
framebuffer, so raising the default would silently invalidate fifty
interactive tests.

**What "it looks like it's from the 1990s" is actually measuring**, and
the honest answer to *"can't we port a working open source browser"*:

- NetSurf now **loads real pages off the public internet** — verified,
  `http://example.com/` rendered with its stylesheet. What it still
  cannot do is `https`, and that is not a bug: no certificate
  authorities ship here (M65's rule, `docs/browser.md`), so mbedtls
  refuses. **The recorded condition for reopening that was "a way to
  update the bundle without rebuilding the image", and M111 built it** —
  a `ca-certificates` package is the shape it takes. That is the next
  thing this browser needs and it is an open box below.
- The *look* is NetSurf's framebuffer front end and its engine's age:
  libcss is CSS 2.1 with pieces of CSS 3, and Duktape is ES5. A site
  built on flexbox, grid and modern JavaScript will render, and will
  render wrongly.
- **There is no smaller-effort modern engine.** Chromium is measured in
  `docs/browser.md` and blocked on `AF_UNIX` + `SCM_RIGHTS` before any
  of the disk or toolchain numbers matter. Firefox needs a Rust target
  for `x86_64-lean_os` that does not exist, on top of a full Gecko port.
  WebKit's embeddable port needs EGL/GLES and a compositor protocol this
  machine has no driver for. Ladybird needs C++23 (which this toolchain
  has), and also Skia, ICU and Qt. **NetSurf is not the compromise
  choice here — it is the only engine whose dependency set this machine
  can actually satisfy**, and the honest next steps are CAs, then ICU,
  then measuring what libcss actually fails at on real pages rather than
  guessing.

- [ ] **`ca-certificates`, as a package.** Without it `https://` fails
      with `Problem with the SSL CA cert`, which is most of the web.
      **The condition is already met** — `docs/browser.md` named "a way
      to update the bundle without rebuilding the image" and M111's
      `os install` is that. Packages live under `/pkg`, so NetSurf's
      `Choices` has to point `ca_bundle` there, and *shipping no CAs
      until somebody installs them* stays true.

### M115 — the browser on the real web: certificates, and a `movaps` `[x]`

*Landed 2026-09-10.* The second half of the same request M114 answered.
M114 made names resolve; this is what was still between that and a
browser somebody can use — and it found a bug in this project's C
library that had been there since M19 and had never once been noticed.

**1. `ca-certificates`, as a package.** With the resolver fixed,
`https://` failed with exactly one message — `Problem with the SSL CA
cert (path? access rights?)` — because this OS ships no certificate
authorities. That was M100's decision on M65's rule, and M100 also wrote
down the condition for reopening it: *a way to update the bundle without
rebuilding the image.* **M111's `os install` is that condition, so the
work was a package rather than a decision.**

```
os install ca-certificates
os: installed ca-certificates-1.0 (1 files) in /pkg/ca-certificates/1.0
os: it may: nothing but read files and use the descriptors it is given
```

and `https://example.com/` loads, verified, in 8.2 s. Nothing changed
about what a *fresh* image trusts, which is still nobody. The package
holds one file of text and **no executable at all**, which makes it the
smallest possible demonstration that a package here is data plus a
manifest rather than a program that runs — `os` says so on the way in.
`Choices` points `ca_bundle` at `/pkg/ca-certificates/1.0/...`, so the
browser looks where a package can actually write (M111's rule) rather
than at `/etc/ssl`.

**2. The bug, which is the milestone.** With certificates in place, a
large real page — `https://en.wikipedia.org/wiki/Unix`, 82.72 KiB of
HTML — killed the browser partway through:

```
[isr] ring-3 fault: General protection fault in task netsurf pid 0x208
  rip=0x00000080001FCA56   rsi=0x0000009000B5AE68
  code@rip = 0F 29 06 ...
```

`0F 29 06` is **`movaps %xmm0,(%rsi)`**, a sixteen-byte *aligned* SSE
store, and `rsi` ends in `68` — eight-aligned, not sixteen. That is the
entire diagnosis, and it is a bug in `user_space/lib/malloc.c`:

```c
#define HEAP_ALIGN 8UL   /* word alignment - matches kernel/mm/heap.c */
```

**C requires `malloc` to return memory aligned for any type; on x86-64
that is 16**, because the ABI's largest scalar alignment is SSE's and
the compiler is entitled to act on it. It does — GCC vectorises a struct
copy into a malloc'd buffer with `movaps`, which *faults* rather than
running slowly on a misaligned address.

**Every program on this machine had been getting 8 since M19** and not
one of them noticed, for a reason worth keeping: none of them was built
by a compiler that had vectorised a store into the heap. It took twenty
megabytes of somebody else's C, on a page big enough to make the layout
engine copy structures around, to execute the one instruction that can
tell. This is M63's rule at its sharpest — *the bug was not in the
ported software; the ported software was the instrument.*

The kernel's part of this was already right and is why the diagnosis
took minutes: it terminated the faulting process rather than the
machine, and printed the bytes at `rip`.

**3. Two holes in the instruments, both found by trying to make a test
fail.**

- **The alignment assertion agreed with the bug.**
  `tests/test_malloc.c` had checked `(unsigned long)a % 8 == 0` since
  M19. **An alignment test that asserts the wrong alignment is worse
  than no alignment test, because it is evidence.** It now asserts 16,
  over every size from 1 to 512 and on the mmap path as well.
- **The test binary did not rebuild when `malloc.c` changed.**
  `tests/test_malloc.c` `#include`s `../user_space/lib/malloc.c` after
  renaming its functions, so malloc.c was in no prerequisite list.
  Breaking `HEAP_ALIGN` on purpose to check the new tests could fail
  produced a **pass** — from a binary built before the edit. Same hole
  M93 found in the headers, in a different place, and the same lesson:
  *a test tier that cannot notice an edit is a slower way of writing
  PASS.* `TEST_USER_DEPS` closes it. With it closed, the deliberate
  break fails 262 assertions across two tests.

**And the first test written for it was not sharp enough either**, which
is the third instrument lesson. "Every size comes back aligned" passed
against `HEAP_ALIGN 8`, because malloc rounds every *request* up — the
first block is always fine. It is the block placed immediately **after**
a badly-rounded one that lands wrong. The test now allocates a pair at
every size and checks the second.

**4. The same `:=` trap, twice in two milestones.** M114 added
`$(NETSURF_BIN): ... $(LIBC_A)` so a libc fix relinks the browser. It
expanded to **nothing** — `LIBC_A` is a `:=` variable defined eighty
lines further down — so `make browser` said nothing and the browser kept
the C library it had been linked with hours earlier. *The prerequisite
that prevents stale linkage was itself defeated by the same class of
mistake, in the commit that documented it.* Both prerequisites now live
next to `$(LIBC_A)`'s own rule.

**What it is like now, measured rather than described.**

| | result |
|---|---|
| `https://example.com/` | loads and renders, 8.2 s |
| `https://en.wikipedia.org/wiki/Unix`, before | **GP fault**, browser gone at ~56 KiB of 83 |
| ...after | survives; HTML fully fetched, still laying out at 300 s |

So the crash is fixed and **the slowness is not**. A page of this size
taking minutes is the first measured number this project has for its TCP
receive path under a real workload, and `milestones.md` has deferred
window scaling, SACK and Nagle on exactly the ground that no such number
existed. It exists now, and it is an open box rather than a claim,
because "slow" without a breakdown between TCP, TLS, layout and the
1024x768 framebuffer is not a diagnosis.

- [x] **Where the minutes go.** `https://en.wikipedia.org/wiki/Unix`
      fetches 82.72 KiB and is still working at 300 s. **The condition**:
      a breakdown that attributes the time — bytes/second on the socket,
      time in mbedtls, time in layout — before touching any of it. M69's
      rule says performance work on an unmeasured path does not get
      done, and this is measured only at the outermost level. The three
      deferred TCP items (window scaling, SACK, Nagle) are the obvious
      suspects and must not be started on that basis alone.
      **Closed by M116**, and the suspects were none of those three: the
      minutes were a corrupting NIC ring, an ignored FIN and a clock that
      ran backwards. See M116, which also opens the box this page now
      needs instead.

### M116 — three complaints, ten bugs, and the instruments that missed them `[x]`

*Landed 2026-09-10.* **Asked for directly**, the evening M115 landed —
*"when I run run-qemu.sh, there seems to be lots and lots of repeating
logs... the web browser can't connect to google.com... the qemu screen
seems to be very small in comparison to just before the browser work.
Your tests are not good enough because these issues keep coming up. Fix
it thoroughly."* It jumped nothing and changed nothing for any row of
the queue. **The last sentence is the milestone**: M114 had already
"fixed" the small screen and M115 had already "fixed" the browser, and
both came back, because neither fix was graded by anything that looks at
what the person was looking at.

Every one of the three was diagnosed by measurement, and not one of them
was what reading the code suggested.

#### 1. "Lots and lots of repeating logs" was the test suite editing the tree

Not the guest. Booted headless, with cocoa, and through
`tools/run-qemu.sh` itself, QEMU printed nothing to the terminal and the
serial log had no repetition in it. What the terminal showed was the
**build**: `build/*.elf` and `kernel.bin` had been relinked at 20:05,
after the last commit, by the person's own `run-qemu.sh` — and a
relink here prints one `x86_64-elf-ld` line per program, each carrying
every object in `USER_LIBOBJS`. **135 KB**, sixty-six near-identical
2 KB lines, which a terminal wraps into hundreds of screens.

Why it relinked: `tools/iconv-test.sh` — in **every** tier, including
`--fast` — checked the generated charset table by *regenerating it in
place*. Same bytes, new mtime. `git status` stays clean, because git
compares contents; `make` rebuilds, because make compares mtimes. So
after every test run since M100, the next `make all` rebuilt the table,
relinked every program, relinked the kernel that embeds them, and
**recreated the disk image** — wiping `/etc/settings.conf`, the session,
and everything else on the machine the person was about to use.
`tools/mutate.py` had the same habit with the kernel's sources: it put
their contents back and not their timestamps.

- `gen-iconv-tables.py` takes an output path; `iconv-test.sh` generates
  beside the file and compares. `mutate.py` restores mtimes (safe: it
  builds no kernel or user object from a mutant — the unit binary is
  deleted before every run, the differential engines always recompile).
- **`tools/tree-stamps.py`**, run around **every stage** of
  `run-tests.sh`: any stage that changes a tracked file's mtime or size
  fails, by name, listing the files. `git status` cannot see this class
  of bug by construction; this is the instrument that can.
- **"a second build does nothing"**: after the build stage, `make all`
  again must print nothing. A rule that always rebuilds is the same flood
  from the other direction.
- The two link rules and the `ar` print `  LD      build/x.elf` unless
  `V=1`. A full relink is now 86 lines and 5 KB. The flood was a symptom,
  but a legitimate libc change relinks everything too, and the object
  list said nothing a person reading it needed.

#### 2. "Can't connect to google.com" was four bugs below the browser, and a missing trust store

The page did load — at **0.4 KiB/s**: 12 KiB after 30 s, `Fetching,
Processing` at 90 s. A packet capture (QEMU `filter-dump`) of a 60 KB
download from a host `http.server` found the first two:

| bug | what the capture showed | cost |
|---|---|---|
| **The RTL8139 ring.** `rtl8139.c` sets `RCR_WRAP`, which makes the card write a packet that runs past the end of the 8 KiB ring *contiguously into the pad*. The driver reassembled such a frame from the ring's **start** — the `RCR_WRAP`-clear layout. QEMU's `rtl8139_write_buffer` says so in one line ("non-wrapping path or overwrapping enabled"). | a 1440-byte segment at exactly `rcv_nxt`, inside the window, answered `ack 5949` as if it had not arrived; 1.5 s later the peer resent it | one full-sized frame in five (1504-byte stride on an 8192-byte ring), **since M27** |
| **The FIN.** `tcp.c` judged a FIN against `seq`, the segment's first byte, *after* the data had moved `rcv_nxt` past it. A FIN riding on the last data segment — how nearly every server closes — never matched. | `[FP.] seq 59229:60189` answered `ack 60189`, not `60190`; the FIN came back alone 1.44 s later | 1.4 s per server-closed connection |

Why a corrupt frame cost seconds rather than a log line: **TCP's checksum
discarded it without a word** — "a corrupt segment is one that never
arrived". Correct, and the reason the symptom anywhere was only
"slow". It now counts (`tcp_checksum_failures()`) and logs, rate-limited.

With both fixed, the 60 KB download went from **never finishing** (fetch
gave up at 8 s with 30 KB) to **35 ms**. google.com then took 22.6 s, and
the third bug needed a different instrument — the guest's instruction
pointer sampled through the monitor, beside the capture:

| bug | what the capture showed | cost |
|---|---|---|
| **`gettimeofday` ran backwards.** It was RTC seconds plus `uptime_ms % 1000` — two counters not in phase — so it stepped back by up to a second whenever the millisecond wrapped first. M98 found that once and told interval-measurers to use `CLOCK_MONOTONIC`; NetSurf's framebuffer scheduler did not get the message. It sets every timer by `gettimeofday`, and its fetch poller reschedules itself every 10 ms. | a 301 that arrived at 0.135 s was read at 4.033 s — with the CPU **~70% idle** in between | up to 1 s per 10 ms timer |

`user_space/libc/src/wallclock.c`: a process's time of day is uptime
plus one offset that only moves forward, to the RTC's second edges — so
it is monotonic, its milliseconds are the monotonic clock's, and it
follows `SYS_settime` backwards when the RTC is more than 1.25 s behind.
NetSurf relinked (it is static; `make browser` does it in 12 s).

| | before | after |
|---|---|---|
| `http://www.google.com/` | never finished | **4.2 s**, rendered |
| `https://www.google.com/` | certificate failure | **5.9 s**, verified and rendered |
| 256 KiB host stream through the NIC (`[m116]`) | ~36 corrupted frames, about a minute | 40 ms of transfer |

**And https** was failing for a reason M115 had already recorded and not
reached: `os install ca-certificates` needs `/pkg/repo`, and
`tools/run-qemu.sh` never put one on the image. So the image a person
booted had no way to trust anybody, and google.com worked for exactly one
page. The browser's install now puts the repository on the image and
names `ca-certificates` in `/pkg/repo/preinstall`; **init runs `os
preinstall`** before the desktop, which installs it through the same
three hashes and registry `os install` uses, once — `/pkg/db/preinstalled`
records it, so `os remove ca-certificates` **sticks**. It is still a
package: removable, verifiable, replaceable without rebuilding the image.
**The assumption, recorded:** an image with the browser on it trusts the
build host's authorities by default, the way every browser anyone uses
ships a root store; an image without the browser — `make all` alone —
still trusts nobody. That reverses the default M115 wrote down, on the
person's instruction, and keeps every property the reasoning behind it
cared about.

**What still does not work, measured rather than guessed: Google
search.** The results page is served only to a browser that runs modern
JavaScript; to anything else it is a `<noscript>` meta-refresh to an
"enable JavaScript" notice (fetched on the host with NetSurf's user
agent to be sure). Duktape is ES5, so NetSurf runs the script, it fails,
and the page is blank. That is docs/browser.md's right-hand column, not
the network.

#### 3. "The screen is very small" was a Homebrew upgrade

The guest was 1024x768 before and after — every screendump said so,
which is exactly why no test could see it. The window was not:
**CGWindowList measured it at 512x412 points**, title bar included, on a
Retina display. `/opt/homebrew/Cellar/qemu/11.1.1` was installed at
11:17 on 2026-09-10, alongside `libpng` — the host library
`tools/build-netsurf.sh` needs — so installing the browser's build
dependency upgraded QEMU, and QEMU 11's cocoa sizes its window as guest
pixels divided by the backing scale (`ui/cocoa.m`, `resizeWindow`): one
guest pixel per *physical* pixel. "Small since the browser work" was
literally true, and nothing to do with the browser.

`zoom-to-fit=on` is not the fix — measured, it opens at 267x228 and
waits for a drag. What restores the old window is telling macOS the
process is not high-resolution capable: `run-qemu.sh` now launches QEMU
through `build/qemu-app/lean_os.app`, a bundle whose Info.plist says
`NSHighResolutionCapable=false` and whose executable is a symlink to
whatever `qemu-system-x86_64` is on the PATH. Measured: **1024x796
points**. `QEMU_HIDPI=1` opts out.

M114's `QEMU_RES` stays as the way to get a *bigger* guest; the complaint
was about the window, and the window is back.

#### The instruments, which is the part the person asked for

Each bug above got the instrument that would have caught it, and each
instrument was run against the bug to prove it is not vacuous:

| instrument | what it grades | against the bug |
|---|---|---|
| `tests/test_rtl8139_ring.c` (5 tests) | the driver's ring reader against a model of the card transcribed from QEMU's `rtl8139_write_buffer`, every frame length, the deepest straddle, ring allocated at exactly its size under ASan | the old reassembly: fails at byte 668 of the first straddling frame |
| `tcp_state` `a_FIN_carried_with_data_is_taken_with_it` (+ the not-fitting half) | FIN-with-data | `expected 1006, got 1005` — the capture, reproduced |
| `tcp_state` `a_corrupt_segment_is_dropped_counted_and_reported` | the checksum drop is counted and logged | — |
| `tests/test_wallclock.c` (4 tests) | monotonic, never ahead, <1 s behind and locked after one edge, at all 1000 phases; `SYS_settime` both ways; a PIT that loses ticks | the old formula: 11,280 failed assertions |
| `libctest` | the real `gettimeofday` on the machine across an RTC edge, against `CLOCK_MONOTONIC` | — |
| **`[m116]`** boot self-test + `/bin/netrecv` | 256 KiB from the **host** (a QEMU `guestfwd` running `cat`) through SLIRP, the ring and TCP, over the POSIX sockets libcurl uses: every byte against a pattern, zero checksum failures, and a `nic_stream_recv_ms` budget. **The first test in this tree to put a full-sized segment through the NIC** | — |
| **`browser_loads_a_page_from_another_machine`** (input suite, `--quick`) | a 120 KiB page from a one-request server behind a `guestfwd`, typed into the address bar; a block that comes *last* in the source must be on screen within 10 s | the ring bug put back: FAIL; fixed: 2.1-2.9 s |
| `tests/test_poll.c` (6 tests) | `poll()` over a scripted `SYS_waitfds`: readable and writable are independent | the old code: 2 tests fail, "expected 1, got 0" |
| `net_arp` (3 tests) | the first packet to an unresolved neighbour is held and sent on the answer; newest wins; unreachable by the third unanswered send | all three failed against the drop |
| **`tools/window-test.sh`** (default tier) | boots through `run-qemu.sh` with its real display and measures the window with CGWindowList: one guest pixel is at least one point | with the wrapper off: `512x412 points`, FAIL, "Half the width" |
| **`tools/tree-stamps.py`** around every stage | no stage changes a tracked file | `touch` on the table: fails naming it |
| "a second build does nothing" | `make all` twice prints nothing | — |
| `set-resolution.sh --check` | its mode list is the driver's — the check its header claimed `run-tests.sh` ran, and it did not | — |

#### What else the new test found, and fixed

- **The first packet to a neighbour was dropped.** `[m116]`'s first run
  measured **980 ms to connect** to a host one hop away. `ip.c` sent the
  ARP request and dropped the packet — "what BSD has always done", its
  comment said. BSD holds it (`la_hold`). Every first contact with a
  machine on the local link waited out TCP's 1 s RTO on a SYN that never
  left. `arp_hold` now keeps one packet per unresolved neighbour, newest
  wins, sent when the address is learned from any packet; a neighbour
  asked three times without answering is reported unreachable (Linux's
  three probes, counted rather than timed so the host test can hold it to
  the number). `nettest`'s "unreachable" check changed with it, from the
  first send to the third: its point was always *no hang, no panic, and
  the program is told*.

- **`poll()` hid a writable socket that was also readable.** The new
  interactive test's first fixture was `cat page.http`, a server that
  speaks before it is spoken to. The browser connected, received the
  first 2.9 KB, and **never sent its request**: `poll.c` reported only
  the POLLIN half of what a readable descriptor was asked, so a socket
  asked for POLLOUT alone - which is how libcurl waits for a non-blocking
  connect - reported nothing while data sat on it. A fast server, or any
  protocol whose server speaks first (SMTP, FTP, SSH), left the browser
  on "Loading" with its window at zero. `tests/test_poll.c` includes
  `poll.c` with a scripted `SYS_waitfds`; two of its six tests failed
  against the old code.

#### What went wrong in this milestone's own work

- **The first google.com drive discarded QEMU's stderr** (the input
  harness sends it to `/dev/null`), so it could not have seen host-side
  log spam. The terminal half of complaint 1 was only answered by
  re-running inside `run-qemu.sh` itself.
- **The first capture analysis filtered out the ACKs** and read as SLIRP
  ignoring the window by 78 KB. It was not; the ACKs were interleaved
  and the window respected. The real stall was ten seconds later.
- **A counter reset**: the checksum test failed its log assertion because
  the counter was process-wide and the rate limit had already been
  reached by earlier tests. `tcp_init` resets it — "since boot" is what
  the number means.
- **The interactive test's fixture was wrong twice.** `cat` never read
  the request, and a socket closed with unread data in it is RESET, so
  the forward threw away everything past 58 KB - every time. And the
  closing block was first `position:absolute`, which NetSurf does not
  paint when empty; that failed on a page that had loaded in 0.3 s. The
  fixture is now a four-line server that reads the request first, and an
  ordinary block. Both mistakes were found the same way the bugs were:
  a capture and a screendump rather than a guess. The test was then run
  against the ring bug put back: it fails; with the fix, it passes in
  2.1-2.9 s, of which the network is 0.57 s.
- **One negative check proved nothing**: a `sed` idiom that is GNU-only
  left the iconv table unedited, and the test "passed". Redone in Python
  and it fails as it should. A mutation that did not apply is
  indistinguishable from a sharp test unless the diff is checked.

#### Cost

Seven bugs fixed in the machine (the ring, the FIN, the clock, `poll`,
the dropped first packet, a trust store no image could reach, the window
scale) and three in the instruments (a test editing the tree, a harness
restoring contents without timestamps, a claimed check that did not
exist). 22 new host tests (383 total), one boot marker and program, one
interactive test, four harness stages, one budget row. No third-party
source touched; NetSurf relinked twice, not edited.

**Graded**: 383/383 host tests; every `--fast` stage; the commit tier
twice - the first green except the new interactive test (its fixture,
above), the second red on two stages that are **not this milestone's**
and are recorded rather than hidden:

- the graded boot **stalled silently for 900 s** inside `[m89]`'s toybox
  pipeline, a spawn-then-wait - the symptom M112 recorded for `[m100d]`
  and `[m100f]`. `build/test-history.tsv` has the same 900 s stall twice
  on each of 686f7b2 and c11d3f3, before any of this work; toybox links
  the sysroot's libc, so `poll.c` does not reach it. The same image
  re-run: **127/127 markers**, `nic_stream_recv_ms` 213 ms.
- `smp-test.sh` panicked with "this CPU is not on the stack of the task
  it thinks it is running" - row 5's reproduction, word for word. Re-run
  three times on the same image: 3/3 pass.

Then the whole interactive suite, **54/54**, and `window-test.sh` in
both tier runs: 1024x796 points.

- [x] **Where the minutes go** (M115's box). Attributed, with a
      capture of `http://google.com/` after every fix here: **each of
      Google's responses arrives 50-70 ms after its request, and all the
      remaining time is the browser not having asked yet** - 0.6 s between
      the 301 arriving and the redirect's DNS lookup, 0.9 s parsing the
      30 KB page before fetching its images and scripts, and 3.4 s between
      receiving Google's JavaScript and the next request, which is Duktape
      running it on TCG. The page is done at 5.7 s. None of that is TCP,
      so the three TCP deferrals (window scaling, SACK, Nagle) stay
      deferred: the measurement their condition asked for exists now and
      does not point at them. What it points at is NetSurf's own work,
      which is the engine and is third-party code this tree does not edit.
- [ ] **Wikipedia now aborts the browser instead of stalling it.**
      `https://en.wikipedia.org/wiki/Unix` - the page M115 recorded as
      "still laying out at 300 s" - now arrives in full in 4.6 s (150 KB
      of TCP, first byte at 0.33 s), and NetSurf then stops on its own
      assertion: `layout_flex.c:246 layout_flex_item: box ... layout
      failed`, followed by `assertion failed: containing_block->width !=
      UNKNOWN_WIDTH at content/handlers/html/layout.c:4504`. The network
      fixes did not cause this; they are what let the layout engine reach
      a path the old 0.4 KiB/s never got it to. NetSurf's own build never
      defines NDEBUG, so an upstream 3.11 build asserts too if the flex
      failure happens there. **The condition**: find out whose failure it
      is before touching anything - the same NetSurf 3.11, built for the
      host (its `monkey` front end needs no display), against a saved
      copy of the page. If it fails there, it is upstream's, and the
      answer is a newer NetSurf rather than an edit; if it does not, it
      is this port - most likely an allocation or a font call that fails
      here and nowhere else - and it is this project's bug to find.
      Building with -DNDEBUG to make the assertion go away is not on the
      list: it would turn a stop into a layout computed with a width of
      INT_MAX.

---

### M117 — a desktop that sleeps, a click that shows, and the browser's first backtrace `[x]` (five boxes open)

*Landed 2026-09-11.* **Asked for directly**, the morning after M116:
*"the browser crashed after I tried visiting apple.com. it was able to
go to google.com, but I am not sure it is live data. I want the browser
to be fully functional. It is also extremely slow. I want the OS to be
high performant. everything should be snappy."* Three complaints, taken
in the order the person put them, and each answered by a measurement
before anything was changed.

#### 1. google.com is live

It is. The M116 capture is the evidence: each of Google's responses
arrives 50-70 ms after its request over SLIRP, through the RTL8139 ring
and this TCP, with TLS 1.3 verified against the `ca-certificates`
package. Nothing on the image holds a copy of google.com; `make all`
writes no page at all, and NetSurf's disc cache is on leanfs, which a
kernel rebuild recreates from nothing. What a person sees is what Google
sent that second. The doubt is reasonable, though, and the reason for it
is the next point: the page does not *look* live, because it barely
moves.

#### 2. "Extremely slow" - measured, and two-thirds of it was the desktop

The host is an Apple M2 Max; the guest is x86-64 under QEMU's TCG, so
every guest instruction is translated, and that ratio is the floor
nothing here can move. What the OS was adding on top of it was measured
with `ps` on the QEMU process and with the guest's instruction pointer
sampled through the monitor, and it was not small:

| idle desktop, nothing touched | before | after |
|---|---|---|
| no window open | **22%** of a host core | **11%** |
| eight windows open | **44%** | **23%** |
| the browser alone | 33% | 13% |

An idle guest that halts should sit near the cost of its timer tick.
This one did not, for a reason M68 wrote down and then reverted: **every
wmclient program's main loop was `poll the event pipe; yield`**, so every
open window was a process that never left the run queue, the idle task
never ran while one was open, and - the part that matters on TCG - every
one of those yields was a CR3 reload that emptied the host's translation
cache underneath whatever was actually working, the browser included.
M68 built `SYS_waitfds` for exactly this, converted the loops, and took
the conversion back because fifty boot self-tests with fixed
`pit_sleep_ms` budgets broke at once. M69 replaced those budgets with
condition waits and converted the compositor's own loop; the clients
were never revisited. The `[m68]` line in any graded log shows the
result: 2-3% idle across a whole boot.

The second finding was the compositor's other half of the bargain: it
had **no way to know a client had drawn**. A clock ticking, an editor
after a keystroke, a browser laying out a page - none of it reached the
screen until `REDRAW_INTERVAL_MS`, a 100 ms fallback poll, which also
meant a full-screen composite ten times a second on a desktop where
nothing had changed. The NetSurf surface, for its part, waited for input
by `usleep(5 ms)` in a loop: two hundred wakeups a second with nothing
to do.

Three changes, all in this project's code and none in NetSurf's:

- **`wm_wait_ms`** (`user_space/lib/wmclient.c`): sleep in
  `SYS_waitfds` on the window's event pipe, plus any extra descriptors,
  until an event, a deadline, or a 250 ms liveness cap - the cap is what
  keeps M55's "is the compositor still alive" check running in a client
  with nothing to do. Every one of the twelve client loops now ends in
  it, with its own deadline (the clock's next redraw, the terminal's
  child output, the shell's pressed button), replacing `sys_yield`.
- **`WM_ACTION_PRESENT`** (`system_api/include/wm.h`): one 16-byte write
  after a frame. The compositor coalesces every present in a pass into
  one rectangle and composites it with `redraw_rect`, drains every
  complete action per pass rather than one (eight windows presenting
  together are one pass, not eight), and the 100 ms poll is a **1 s
  safety net** for a client that never presents - which nothing in this
  tree is.
- **The NetSurf surface** presents once per turn of NetSurf's event
  loop (libnsfb's `update` callback sets a flag; `leanos_input` sends it
  before waiting) and waits in `wm_wait_ms` for the bounded time libnsfb
  asks for.
- **The compositor's animation loop sleeps until the next frame is
  due** rather than spinning through `sys_yield`, presents wait while
  anything animates (the animation's closing full frame carries them),
  - the reasons are under *what went wrong*.

**Measured after**, on the same boot conditions:

- the M116 network page (120 KiB from a host server) is on screen
  **0.43 s** after Enter with seven idle windows open, the same as with
  none - the first load of a session is 2.9 s either way, which is
  NetSurf's own start-up (fonts, the default stylesheet) and not the
  desktop.
- **`click_to_photon_us`**, a new budget row: a press on a `wm_zorder`
  window until *that window's own tick* is on screen - the press
  routed, the client drawing, the client presenting, the compositor
  compositing that rectangle. 63-87 ms worst of three, polled the way
  M69's cursor rows are polled (the observer competes, and the cursor
  alone costs 10-38 ms in the same boots); before, this path ended on
  the 100 ms poll, and a compositor that stopped honouring presents now
  lands on the 1 s net, which the ceiling is set to catch - and did,
  six times, which is under *what went wrong*.
- **`desktop_busy_pct`**: what the compositor and four idle windows cost
  in CPU over two seconds, from the tasks' own user+sys tick counters:
  **2%**. The run-queue version of the same measurement read 100 - see
  *what went wrong*.
- and a number nobody asked for: **`input_to_photon_idle_us`**, the
  cursor's own path, went from 40.6 ms at its last recorded measurement
  to **9.6 ms** in the same graded boot, because the compositor is no
  longer competing with a run queue full of windows polling their
  pipes.
- **and window animations, which turned out to have been the worst
  thing on this desktop and unmeasured.** With the report built into
  HEAD, eight open/close animations drew **3-5 frames each, 60-100 ms
  between frames**, every time - a 140 ms animation shown as three
  pictures. After this milestone: **4-7 frames, 40-50 ms apart** (one
  at 90 in seven runs); with the withdrawn display task it was 7-8 and
  20-30, which is what that open box is worth. That is the new
  `anim_frame_gap_ms` row, one line per animation, graded in place of
  a rule that had timed one composite and failed a boot at 16 ms while
  the person waited 100 between frames.

Where the rest of the 11% goes, from the sampler: 75-80% of samples are
the CPU halted inside `pit_sleep_ms` (init's wait, which halts in
place), and 5-12% are the compositor compositing - the shell presents
every 300 ms and the settings window every 500 ms whether or not their
content changed, and each present is a rectangle with a blended shadow
under it. That is the next thing to take, and it is recorded rather
than done: it is a few percent of a host core, and the crash outranked
it.

#### 3. apple.com - reproduced, bisected, and found under the browser

`https://www.apple.com/` killed the browser 13.5 s after Enter, on
NetSurf's own assertion: `box->height != AUTO at
content/handlers/html/layout.c:5333`, in
`layout_calculate_descendant_bboxes` - the walk after layout that
requires every box to have a height, and which `layout_document` runs
**whether or not the layout succeeded**. So any layout step that returns
false without saying so ends here, some frames later, with no trace of
where it gave up. M116 left the Wikipedia crash (`layout.c:4504`, the
same family) with a condition: the same NetSurf 3.11, built for the
host, decides whose failure it is.

**The host twin.** Built from the same unmodified tarball on this Mac,
in a scratch directory: first the `monkey` front end, then the
`framebuffer` front end on libnsfb's display-less `ram` surface, with
freetype and the guest's own DejaVu files, the JPEG decoder from the
same jpeg-9f tarball, Duktape running the same eleven scripts (the
same eleven errors), the guest's `Choices`, and the guest's 800x600
window. Every one of them rendered apple.com. Three things had to be
worked around on macOS, all in the build system and none in the tree -
Apple's `ld` has no `--whole-archive` or `--trace`, macOS hides
`strcasestr` under `-std=c99`, and Apple's libcurl carries LibreSSL
while NetSurf's fetcher pokes the SSL context through Homebrew's
OpenSSL 3 (a `SIGBUS` inside the host's libssl before the first byte;
`NETSURF_USE_OPENSSL=NO`). And one thing went wrong that is worth the
record on its own: for three builds the "freetype twin" and the "JPEG
twin" were the internal-font, no-JPEG build again, because the script
that built it wrote its own `Makefile.config` over the one I had
written, and only `nm` on the binary said so. **`tools/build-netsurf-host.sh`**
is that recipe with one writer, and it checks the binary for
`FT_Init_FreeType`, `jpeg_read_header` and `duk_create_heap` before it
calls the result a twin.

So it was this port's, by M116's own condition. Two instruments then
found where:

- **The first backtrace.** This libc's `__assert_fail` printed one line
  and aborted, and the function has eleven callers. It now walks the
  frame chain - bounded, range-checked, symbolised on the host with
  `nm -n` on the unstripped binary - and `tools/build-netsurf.sh` builds
  everything with `-fno-omit-frame-pointer` so the chain exists. The
  first run gave the path: `html_box_convert_done` → `content_broadcast`
  → `browser_window_callback` → `content__reformat` → `html_reformat` →
  `layout_document` → nine levels of `layout_calculate_descendant_bboxes`.
  The **first** layout after box conversion, and a subtree left unsized.
- **The same bytes to both.** A proxy on the harness's guestfwd (the
  command it runs per connection fetches the path from apple.com, so the
  guest and the twin get identical bytes over plain http) and a
  bisection of the page: no scripts → crashes; no `<main>`, no footer →
  crashes; **no `nav#globalnav` → renders**. The nav alone → crashes.
  Its four items one at a time: the shopping bag. The bag without its
  SVGs, without its badge: crashes; **without its flyout - three nested
  empty `div`s - renders.** The 145 stylesheet rules those divs can see,
  inlined and halved twice: one rule.

```css
#globalnav .globalnav-submenu-content { margin: 0 auto; box-sizing: border-box;
    width: 100%; max-width: 1024px; display: flex; ... }
```

**An empty `display: flex` container.** `layout_flex_ctx__create` sizes
its item list with `calloc(box_count_children(flex), sizeof item)` -
zero children - and **this libc's `malloc(0)` returned NULL**, which
C permits and which NetSurf, like every program written against glibc,
macOS or musl (all of which hand back a unique pointer), reads as out
of memory. `layout_flex` returned false, silently; `layout_block_context`
returned false, silently; `layout_document` walked the tree anyway, and
the assertion fired on the first box the failed subtree had left at
AUTO. The host never saw it because the host's `calloc(0, n)` is a
pointer. Google's page has no empty flex container; apple.com's nav has
one per flyout, and Wikipedia's article layout has them too.

`malloc(0)` returns the minimum block now: real, distinct from every
other live allocation, aligned like any other, and taken back by
`free`. `tests/test_malloc.c` says so in two tests, and the one test
that had asserted the old answer as law (`zero_is_no_allocation…`,
from M19) keeps its `free(NULL)` half and lost the other. This is the
lesson the archive already holds under M100 - *a refusal only refuses
when the caller looks*, and this refusal was legal, silent, and
sixteen milestones old.

**Verified on the machine**: apple.com renders (`Done (6.8s)`, apple's
compact layout - plain, because libcss 0.9 has no `calc()` or `var()`),
and **`https://en.wikipedia.org/wiki/Unix` renders**, which closes
M116's open box. The reduction is checked in as
`tools/netsurf-port/flex.html`, installed at
`/usr/share/netsurf/flex.html`, and typed into the browser by the input
suite's `browser_survives_an_empty_flex_container`: apple's properties
on an empty flex container and a red block *after* it in the source,
so the block is on screen only if the layout finished. Against the old
allocator the browser is gone before the block could appear.

**What did not cause it, each ruled out by a run rather than by
reading**: JavaScript (the page with every script removed crashes),
the JPEG decoder (the guest with `NETSURF_USE_JPEG=NO` crashes), fonts
(the freetype backend never fails a width call - a missing glyph is
skipped), the window width (the twin at 800 px renders), an allocation
being *refused* by the kernel (a temporary trace on `sbrk` printed
nothing), and the arrival order of stylesheets (the fixture has one,
inline).

#### The instruments

| instrument | what it grades | against the bug |
|---|---|---|
| `tests/test_malloc.c` (+2) | `malloc(0)`: a real block, distinct, aligned, freed and reused, ten thousand in a row leak nothing | the old allocator: `expected non-NULL` |
| **`window_animations_stay_smooth`** (input suite, `--quick`) | eight animations started on a quiet desktop (a close that lands mid-animation shares the run, so at least four are reported), the compositor's own `anim_frame_gap_ms` lines read back from the serial log: the two best animations at ≤ 50 ms, the worst ≤ 250 | HEAD's best two: 60 and 60, fails |
| **`browser_survives_an_empty_flex_container`** (input suite, `--quick`) | the reduction typed into the browser; the red block after the empty flex container must be on screen within 20 s and the window still there | the old allocator: the window is gone |
| `tools/build-netsurf-host.sh` | the M116 condition, repeatable: the same NetSurf for this Mac, `run <url>` reports rendered or crashed, and it refuses to call itself a twin without freetype, jpeg and duktape in the binary | — |
| `__assert_fail`'s backtrace + `-fno-omit-frame-pointer` in the port | where a ported program died, not only that it did | the first run: 22 frames, symbolised |
| `anim_frame_gap_ms` | the longest gap between two frames of every window animation, from the compositor, one row per animation - in place of the M61 grep on one composite's length | HEAD's own numbers: 60-100 ms, every animation |
| `tests/test_wmclient.c` (10 tests) | `wm_wait_ms`: the event pipe first, extras after, negatives skipped, the cap, zero as a poll, the return contract; `wm_present`: one `wm_action_request_t` naming this window and `WM_ACTION_PRESENT`, the pipe opened once, no window id means no write. wmclient.c compiled whole against scripted syscalls, the way test_poll.c does poll.c | — |
| **`[m117]`** boot self-test | click-to-photon through a client (three presses on `wm_zorder`, the tick's pixel polled from the press), and the CPU the compositor and four idle clients use over two seconds, from per-task ticks | the first version measured idle from the run queue and read 100% busy with clients that were provably asleep - see below |
| `click_to_photon_us`, `desktop_busy_pct` | `tests/budgets.tsv` rows for both | — |
| the sampler | the guest's RIP/CR3 through the monitor, 400 samples over 25 s, symbolised against `kernel.elf` and the user binaries - a scratch script this time, and the instrument that separated "halted in `pit_sleep_ms`" from "compositing" | — |

#### What went wrong in this milestone's own work

- **The first idle measurement in the self-test was the wrong one.**
  `sched_idle_ticks` counts ticks with nothing runnable, and in the
  middle of the battery something always is: the `[m68]` line in the
  same log said 2.7% idle for the whole boot. The row read 100 with four
  clients that the host-side sampler showed asleep. Per-task user+sys
  ticks (M101's) charge a spinning client and not a blocked one whatever
  else is on the run queue, and that is what the row measures now.
- **`rm -rf netsurf-all-3.11/*/build` deleted source.** NetSurf's
  libraries keep their *build scripts* in `build/` and their objects in
  `build-<host>-<target>-release-lib-static/`; the command meant to
  force a rebuild with the new flag removed `libparserutils/build/
  make-aliases.pl` and two more, and `make` then failed with no output
  at all - `make -d` was the instrument that found it, 7,960 lines in.
  Restored from the tarball, byte for byte, which is what keeping the
  tarball is for. The generated directories are the `build-*` ones.
- **Six graded boots read `click_to_photon_us` at 734-751 ms** - three
  presses each landing on the 1 s safety net (573, 733, 734: three
  draws from a 0-1 s wait) - after three had read 63-70. The scheduler's
  display task was suspected, withdrawn, and cleared: the number stayed
  with it gone. The allocator was suspected and cleared: a trace showed
  no zero-byte request on the desktop, and a variant with the old
  allocator read 139 ms under threefold host load. A trace in the
  compositor then showed the thing itself: during those presses **no
  present from `wm_zorder` ever reached the compositor**, while three
  clocks spawned seconds later presented through the same pipe and were
  composited at once. The client's present is one `sys_pipe_open` of
  the shared action pipe - its first, on the first press - and one
  16-byte write. Two changes in the client made the number 62-87 again
  in every boot since: a trace line after the present (an extra
  syscall, nothing more), and then presenting the first frame at
  startup, which moves that first open of the action pipe from the
  first click to the moment the window exists. **Why a named pipe's
  first open from a second process, followed at once by a write, loses
  that write while a client blocked in `SYS_waitfds` on another pipe
  waits, is not understood** - `kernel/ipc/pipe.c` is persistent for
  named pipes and never drops a byte by reading; it is the open box
  below with a host test named as the instrument. One clean run of a
  probabilistic failure was believed twice on the way; the third time
  the trace was demanded first.
- **Nine clients drew their first frame and never said so.** Every
  program here paints once before entering its loop, and the loop
  presents only what it draws afterwards; under the 100 ms poll the
  first frame was on screen within a tenth of a second whatever the
  order, and under the 1 s net it is on screen when the compositor's
  own window-creation redraw happens to come *after* the client's
  paint, or a second later. `[m44]` caught it once - the wallpaper
  read black 1.2 s after `desktop_icons` was spawned, with the trace
  showing the desktop window's only present composited before its
  paint - and the input suite's app launches had been showing chrome
  with the content a beat behind. Every client presents its first
  frame now. The terminal had the opposite habit: with a child running
  it set `changed` on every pass, which was twenty redraws and presents
  a second of nothing once the loop woke on a timer instead of
  spinning; it redraws when output arrived or a child ended.
- **`--full` had never been green, and three of its stages were red for
  reasons older than this milestone.** `make coverage` had not compiled
  since M111: its CFLAGS lacked `-Iuser_space/lib` and
  `-DLEANOS_HOST_TEST`, which every other host build has, so every test
  that includes user-space code failed to build there and the stage
  said FAIL without saying why. With the flags, one floor moved by
  0.26% (`wchar.c`, three lines the define now compiles and the tests
  never reached - lowered, and said so) and six files had no floor at
  all; they have one now. The I/O APIC battery ran out of its 900 s
  inside `[m100]`'s library suites: the default battery is 405-423 s
  on a populated image (M100 measured 362 on a bare one) and that path
  costs 2x - the ceiling is 1200. And once, on that path, `libctest`
  read `time()` twice across a 1.5 s `poll` and got the same second -
  a boot that ran straight after the 28-minute mutation census; two
  I/O APIC boots since, one of HEAD and one of this tree, passed the
  same check. Recorded, not explained, and the host is on the record
  as a suspect with form (M100).
- **A test can't stub what another test already defines.** The host
  runner links every `tests/test_*.c` into one binary, so `test_poll.c`'s
  `sys_waitfds` fake is global; the wmclient test renames the call on
  the way in with a macro rather than fight for the symbol.
- **`[wm] animation missed its frame budget`, and four wrong answers
  before the right one.** The graded boot reported it where none had
  before, and the person's desktop showed it too: four of eight
  open/close animations with a worst frame of 40-50 ms. In order:
  (1) *a loaded host* - no, it recurred quiet; (2) *a present
  composited between two animation frames* - deferring presents during
  animations changed nothing (the deferral stays: it is cheap and
  right); (3) *the scheduler demoting a compositor that spun through
  `sys_yield` to batch priority once nothing else spun with it* - a
  real hazard, and the animation loop now sleeps until the next frame
  is due instead of spinning, which stays too, but it made the misses
  *worse*; (4) *`malloc(0)`* - a variant with it reverted ran a clean
  battery, and a trace then showed **no program on the desktop calls
  `malloc(0)` at all**. That variant's clean run was chance, and the
  lesson is old: one clean run of a probabilistic failure is not
  evidence. What was: **HEAD, built in a worktree, running the same
  eight animations with the report printed after every run** - 3 to 5
  frames per 140 ms animation, so the frame *interval* has been
  30-45 ms since M61 under TCG, and one animation in seven missed at
  HEAD too. The budget never measured the interval; it timed the
  composite, and the composite excludes the yields between frames. Then
  HEAD plus one change at a time, on the desktop rather than in the
  battery: the new clients alone - clean; the new compositor alone -
  misses; and of the compositor's changes, the 1 s fallback alone -
  misses, the sleeping animation loop alone - misses. Both make the
  compositor *block more*, and the mechanism is coincidence: every
  client now wakes on a PIT tick, every frame starts on one, and a
  client's redraw that used to land at the compositor's own yield
  between frames now lands inside the frame at the compositor's slice
  expiry, where `took` counts it. Under the old spin the rotation put
  a client's redraw between frames by accident. **The obvious fix is what every
  windowing system has: the process that owns the screen goes first** -
  and it was built three ways, measured three times, and withdrawn.
  `task_t.display`, set at spawn from `CAP_FRAMEBUFFER` and reachable
  by nothing a program can call, with `pick_next` taking a ready
  display task first and a woken one preempting a client at the next
  tick. "Keeps the CPU for three slices": M69's input-to-photon
  self-test - a kernel task polling the framebuffer with `schedule()` -
  stopped making progress and the battery sat silent from `[m67]` to
  the 900 s ceiling. "One slice": the batch demotion put a *starting*
  compositor behind task 0 halting in `pit_sleep_ms`, and the first
  `[wm]` test never saw a frame. "First at a pick, preempt on wake,
  nothing more": the desktop was at its best (7-8 frames, 20-30 ms), but the
  battery stopped after `[m67]` again and `click_to_photon_us` read
  738 ms - three 250 ms liveness caps, a client that was not being
  woken and ran only when its own cap expired. Four scheduler tests on
  the fake CPU passed all three versions, which says what they tested
  and what they did not. The mechanism between a display task, a
  kernel task that polls through `schedule()`, and a client blocked on
  the poll channel is not understood, and a scheduler change that is
  not understood does not ship; it is the open box below, with the
  host scheduler test named as the instrument that has to reproduce
  it first. The message carries
  the worst frame's duration now, because "1 of 5 frames" was the
  whole of what the first runs had to say. And then the number that
  ended it: the same report built into HEAD, with the gap between
  frames added, showed HEAD's animations at 3-5 frames with 60-100 ms
  gaps - the sleeping desktop draws 4-7 frames 40-50 ms apart, and
  7-8 at 20-30 with the display task that was withdrawn. The old rule had graded the composite, the one part
  of a frame that got *longer* here (a cold vCPU thread runs each
  frame from a halt, which is the price of a guest that sleeps), while
  the gap, the part a person waits through, fell by two-thirds and had
  never been measured. `anim_frame_gap_ms` is the row now; the
  per-animation line carries the frame count and the longest composite
  beside it.
#### Cost

Seven bugs fixed in the machine: `malloc(0)` returning NULL (M19's
answer, sixteen milestones old); twelve client loops that spun through
`sys_yield`; a compositor with no way to learn a client had drawn;
NetSurf's surface waking two hundred times a second; the compositor's
animation loop spinning; nine first frames drawn and never presented;
the terminal redrawing twenty times a second with a child running. Four
instruments that did not exist: a backtrace on assert and a port built
with frame pointers, the host twin (`tools/build-netsurf-host.sh`),
the flex fixture and its suite test, and the animation-gap row with its
suite test - plus `[m117]`'s two rows, ten wmclient tests, two malloc
tests, and a RIP sampler that stayed a scratch script. One box from
M116 closed (Wikipedia renders); three opened here.

What it cost is on the record above and is the larger part of the
entry: twelve graded boots, five wrong theories about one number,
a scheduling class built three ways and withdrawn, three host twins
that were not twins, a `rm -rf` that deleted somebody else's source, a
QEMU left running for two hours by a `pkill` that killed only the
script, and two clean runs of a probabilistic failure believed before
the trace was demanded. **Graded**: 395/395 host tests; every `--fast` stage; the commit tier;
and `--full` end to end - 8,308 s - with every stage green except
three, recorded rather than hidden: the four-core stage (the M106-tail
timeout before `[m106]`, row 5 of the queue, "failing 2 runs in 3");
the I/O APIC battery, which no `--full` in this project's history has
passed and which is a box below; and the bootstrap build, which is
the other. In that run: coverage and its ratchet (green for the first
time since M111), the sampled mutation census, fuzzing, the graded
boot (430 s, presses 62/66/66 ms), somebody else's configure, all 56
interactive tests (the animation test read gaps of 50/50/50/80/130 ms),
sixteen power cuts, both refusing disks, and inside the bootstrap
stage every measurement it took: 35.9 s per C translation unit against
32.1, 92.5 s per C++ unit against 86.9, bzip2 built in 232 s against
203 - gcc on this libc, with a real `malloc(0)`, about 12% slower
across the board, which is also what the host's own load looked like
that evening. `--full` has never been green here; the two runs on
record before this one failed at 2,635 s and 7,048 s.

---

### M118 — AF_UNIX with SCM_RIGHTS: the condition under every modern engine `[x]`

*Landed 2026-09-11.* Asked for directly — *"systematically build the
needed pieces so I can install chromium later"* — and what that asks for
is already written down with numbers: `docs/browser.md`'s five conditions,
in its own order. This is the first of them, and the only one its own
measurement called not-a-number: **`AF_UNIX` with `SCM_RIGHTS`. Without
descriptor passing there is no Mojo, and without Mojo there is no
Chromium — not a slow one, not a limited one, none.**

**What reading the question again added, and it is the reason this is
worth a milestone rather than a chore.** The measurement had framed this
as Chromium's condition. It is not. WebKit's `IPC::Connection`, Gecko's
IPDL and Ladybird's LibIPC all pass descriptors over a Unix-domain
socket, and **none of the four engines still has a supported
single-process mode**. So one kernel feature stands in front of every
multi-process browser engine that exists, and the only modern engine that
would not need it is Servo — whose own condition is a Rust `std` port for
`x86_64-lean_os`, which is larger. That is what made this the right thing
to build *before* the engine is chosen: a port that picks the engine first
finds this gap second, with a deadline attached.

**What was built.** `socket(AF_UNIX, SOCK_STREAM|SOCK_SEQPACKET)`,
`socketpair`, `bind`, `listen`, `connect`, `accept`, `read`, `write`,
`sendmsg`, `recvmsg`, `shutdown`, the abstract namespace, `MSG_TRUNC` and
`MSG_CTRUNC`, and `SCM_RIGHTS` — a descriptor of any kind crossing to
another process as a reference to the same kernel object. 619 lines of
kernel (`kernel/ipc/unixsock.c`), six syscalls, a new `FD_UNIX`
descriptor kind, and the libc half including the `CMSG_*` macros. Six
files are new, fifteen changed.
[docs/unix-sockets.md](docs/unix-sockets.md) is the design note.

**Four decisions, each of which could have gone the other way:**

1. **It lives in `kernel/ipc`, not `kernel/net`.** A Unix-domain socket
   shares nothing with that directory but a spelling — no address, no
   checksum, no retransmission, no device — and shares everything with
   `pipe.c`. Putting it in `kernel/net` would have meant `net_lock` held
   over a channel no packet can reach, and would have answered the next
   question wrongly by default.
2. **It needs no capability.** `SYS_socket` checks `CAP_NETWORK`; this
   family does not, and that is the point rather than a gap. What it
   reaches is another process on this machine that is already listening,
   which is what a pipe reaches. And gating it on `CAP_NETWORK` would
   have been backwards for the thing it exists for: a renderer is the one
   program that must hold *no* network capability and the one that cannot
   work without this call. `/bin/unixtest`'s last section is that
   assertion, with a control: a child calls `dropcaps(0)`, is refused an
   `AF_INET` socket, and then talks to its parent.
3. **Nothing in the file parks.** `kernel/net`'s arrangement, not
   `pipe.c`'s: `syscall.c` does the blocking around a non-blocking
   `unixsock_recv`, which is what makes the whole of `SCM_RIGHTS`'
   bookkeeping reachable from a host test.
4. **The kernel's message is bytes and a descriptor list, not a
   `msghdr`.** iovec gathering and `CMSG_NXTHDR` walking are a user-space
   calling convention; libc does them at the same seam that already
   converts byte order for `sockaddr_in`.

**The bug that was found by reading rather than by running, and the test
that would have hung instead of failing.** The obvious shape of
`unixsock_send` copies each passed descriptor into the record and calls
`fd_retain` there, under the file's own lock. It deadlocks — and the case
is exactly the one this milestone exists for: a passed descriptor may
*itself* be a Unix-domain socket (Mojo passes channel endpoints over
channels), and `fd_retain` on one calls `unixsock_ref`, which takes the
same non-recursive lock. Found in the call graph before the first boot.
The references are taken before the lock now, and the failure paths give
back what they did not keep. The test for it is
`a_socket_passed_over_a_socket_is_released_too`, and it is worth knowing
that it would have *hung* rather than failed — which is why that test's
comment says so.

**What the instruments said, in the order they spoke.**

- `tests/test_unixsock.c`: **30 tests**, graded against
  `tests/fakes/fake_kernel_objects.c`, which counts the references on a
  passed descriptor — so "the receiver got one and the sender's own is
  untouched" is a number. Every test ends by asserting no socket, no
  queued descriptor and no reference survived.
- `make mutate` on the new file: **61.5% first, 87.2% after.** Nine of
  the fifteen survivors were real missing assertions and are now nine
  tests — a type that does not exist being refused, a name at exactly
  `UNIX_PATH_MAX`, a SEQPACKET message that does not fit *right now*
  waiting whole rather than partially, a stream of 64 one-byte writes not
  exhausting a 16-record queue, `recv` clearing the out-parameters it is
  about to report, a descriptor delivered once even when its record
  survives the read, and a truncated message not taking the next one with
  it. Of the five that remain, three are equivalent mutations and two are
  one-past-the-end reads that only a sanitizer sees — and the campaign
  runs with `TEST_SAN=0`.
- **`/bin/syscalltest` failed the first graded boot**, and was right to:
  its table requires every syscall number to be classified and six new
  ones were not. Classifying them also moved a rule into this code — the
  three name-taking calls validate their user pointer *before* looking at
  the descriptor, because the sweep pins the descriptor to one that
  cannot exist, and a handler that checks the fd first has a pointer
  check that passes for the wrong reason. `SYS_pread`'s entry in that
  file is where that rule was already written down. 288 checks before
  this milestone, **313 after**, 0 failures.
- `[m118]`, ten sections in two real processes, **1200 ms**: a pair both
  ways, a SEQPACKET boundary kept, a pipe end *and an open file* passed
  to a forked child, a socket passed over a socket, a path name and an
  abstract name dialled from another process, a blocking read woken by
  its peer, `shutdown` seen as end of stream, `MSG_CTRUNC` reported, and
  a child holding no capabilities doing all of it. Plus two claims the
  program cannot make about itself, counted by the kernel on either side
  of it: every socket it created was given back, and no passed descriptor
  is still sitting in a queue nobody read.
- The sharpest section is the third, and it is sharp because of what it
  rules out: the parent reads two bytes of a ten-byte file and passes the
  descriptor; the child must read the remaining **eight**. An
  implementation that re-opened the path would pass every other section
  and fail that one.

**What it cost.** One afternoon. The graded tier: 426 host tests (21→30
new ones), 129 boot markers, the interactive quick subset, four cores —
all green, 565 s. The new file's coverage floor is 92.52%.

**What is explicitly not built, with the condition attached** (all in
[docs/unix-sockets.md](docs/unix-sockets.md)): `SOCK_DGRAM` on this
family — condition, a program that sends to a bound name without
connecting; a bound path as a real filesystem node — condition, a program
that needs `stat()` or `unlink()` on it to succeed, and the abstract
namespace has none of that half-truth in it; and `SCM_CREDENTIALS` —
condition, the one multi-user names, since two of its three numbers would
be constants here.

**Where this leaves the Chromium arc.** Condition 1 of five is closed and
`docs/browser.md` says so next to the original measurement rather than
instead of it — the syscall table there is left as it was measured, with
the four that moved named underneath. **Condition 2 is next and is
ordinary work: an epoll-shaped readiness interface plus `eventfd` and
`timerfd`.** `base`'s `MessagePumpEpoll` calls `epoll_wait`; this kernel
has `poll`, `select` and `SYS_waitfds`, and nothing here can wake a
thread the way `eventfd` does. Conditions 3 to 5 stay an arc rather than
a port.

### M119 — a message pump: epoll, eventfd and timerfd `[x]`

*Landed 2026-09-12.* The second of `docs/browser.md`'s five conditions,
which M118's own entry had promoted to next: *"an epoll-shaped readiness
interface, plus `eventfd`/`timerfd`. The message pump is not optional and
`poll` is not what it calls."* Chromium's `base::MessagePumpEpoll` names
`epoll_create1`, `epoll_ctl` and `epoll_wait`, owns an `eventfd` to be
woken from another thread and a `timerfd` for its delayed work, and has no
`poll` path left. So do libevent and glib.

**What it is honestly for, stated against the usual argument.** The
textbook case for epoll over poll is O(1) against O(n). On this machine
that argument is worth nothing — `MAX_FDS` is 128 and a scan of a whole
descriptor table is a few hundred cycles — and `kernel/ipc/epoll.c` scans
its set linearly and says so. What epoll buys here is three things `poll`
cannot express at all: **a set the kernel remembers**, **a cookie that
comes back with the event** (the feature ported code leans on hardest),
and **edge-triggered and one-shot modes**, which are statements about what
changed since the last call. M69's rule, applied to a feature whose
marketing is a performance claim.

**What it added that was missing rather than renamed: write-readiness.**
Nothing in this kernel could answer *would a write block?* `<poll.h>` has
reported `POLLOUT` for any open descriptor since M88 and said why — there
was no such answer, and *ready* was a better lie than never reporting it.
`EPOLLOUT` now means a pipe with room, a socket with send-buffer space, a
Unix-domain socket whose buffer **and** record queue have room, a counter
below saturation; and a descriptor whose write would *fail* rather than
block reports `EPOLLERR`. **So two calls in this kernel now answer the
same question differently, deliberately**: `poll` keeps M88's answer
because M116 measured the browser through that path, and the condition for
unifying them is a measured case of a program spinning on a `POLLOUT` that
is not true. epoll is now the instrument that can produce one.

**Three decisions worth the argument:**

1. **Every call in `timerfd.c` takes the time as a parameter.** `tcp.c`
   reads the clock itself and its tests pay for that with a fake PIT whose
   every read advances time; this file is the other choice, made after
   seeing that bill. It is why a test can ask what a timer reports at
   1099.999 ms and at 1100.
2. **`epoll_scan` takes the readiness question as a function pointer.** So
   the set management — add, modify, delete, full, stale, edge, one-shot —
   is a pure function of the set and the answers, and is graded against a
   scripted readiness table with no descriptors under it at all. The
   kernel passes its one implementation (`fd_epoll_mask_for`).
3. **A registration does not hold its descriptor open**, which is the one
   deliberate divergence from Linux. The set records the descriptor number
   *and what it pointed at*; a closed or reused descriptor is dropped by
   the next wait rather than reported against whatever took the slot.
   Holding a reference would keep a pipe alive because somebody forgot to
   deregister it — a leak wearing an epoll set as a disguise.

**The instrument this milestone is proudest of, and it is not new.** The
failure this work was most likely to ship is a `poll`-shaped
implementation that works perfectly and burns a core, and no unit test can
see it. `/bin/epolltest` waits 200 ms in `epoll_wait(-1)` over a timerfd
and measures `SYS_idle_ticks` across the wait: ten of twenty ticks idle,
or the self-test fails. M68 built that counter for exactly this reason —
*"a desktop that spins and one that halts look identical from the
outside, which is precisely how this OS shipped sixty-seven milestones
without anyone noticing"* — and this is the second milestone to need it.

**And the section that hangs rather than fails.** That same wait has no
timeout of its own, so the *only* thing that can end it is the timer. If
the park's deadline were not computed from the armed timers, the self-test
would hang, and a hang reaches `tools/qemu-serial-test.sh` as `[m119]`
never appearing. That is the right way for that particular bug to be
reported, and the marker's comment in the harness says so.

**What the instruments said.**

- `tests/test_readyfds.c`: **29 tests**. Mutation scores first run
  **50.0% / 58.3% / 85.7%**, after the tests those survivors asked for
  **87.5% / 83.3% / 95.7%**. The survivors fell into three groups and only
  one was interesting: null guards nothing called with null, a reference
  count nothing had taken twice (`--refs <= 0` mutated to `<= 1` frees an
  object somebody still holds, and every test held exactly one), and the
  clock boundaries, which is where a real defect would hide.
- **Two coverage floors fell and were fixed rather than lowered.**
  `unixsock.c` dropped from 92.52% because M119 added three accessors the
  host tier never called, and `sched.c` because `fd_release`/`fd_retain`
  grew three more cases. Both are now tested — and the second matters more
  than the number: the descriptor union means `slot->event` and
  `slot->timer` are the same bits, so a case that unref'd the wrong *kind*
  would compile, run, and leak.
- `/bin/syscalltest` again earned its keep, twice. Its census caught seven
  unclassified numbers, and then three of them could not go in the sweep at
  all: `eventfd`, `timerfd_create` and `epoll_create` **create a
  descriptor for any plausible argument**, and the sweep's hostile values
  are perfectly plausible here — a kernel address read as an eventfd's
  initial count is just a large number. Twenty calls would have left twenty
  descriptors behind and moved every fd number the later checks depend on.
  They are `CLASS_SKIP` with that reason and are graded by name in a new
  section 7, which closes what it creates. 313 checks before, **344**
  after, 0 failures.
- `[m119]`, eight sections, **700 ms**.

**What it cost.** One session, alongside M118. The graded tier: 460 host
tests, 130 boot markers, the interactive quick subset, four cores.

**What is explicitly not built, with conditions** (all in
[docs/readiness.md](docs/readiness.md)): nesting an epoll set inside
another — condition, a program that does it; `signalfd` — condition, a
program that waits for a signal through a descriptor, which `base` is not;
`EPOLLEXCLUSIVE` and `EPOLLWAKEUP`, accepted and ignored, one waiter per
set and nothing here suspends.

**Where this leaves the arc.** Conditions 1 and 2 of five are closed, and
they were the two that were ordinary work. **Condition 3 is clang and
libc++ for `x86_64-lean_os`** — a compiler, not a few hundred lines of
kernel — then a 16 GB machine with 100 GB of disk, then a sandbox story
that is not a pretence. What these two milestones bought beyond Chromium is
worth more than what they bought towards it: every multi-process engine's
IPC layer and every modern event loop's core. The next thing that could
run here because of them is anything built on libevent, glib or Mojo.

### M120 — a buffer that crosses the channel: memfd_create `[x]`

*Landed 2026-09-12.* The third of the three pieces a multi-process engine is
built out of, and the third of the six absent syscalls `docs/browser.md`'s
measurement singled out: *"`memfd_create` — shared memory between renderer
and GPU process"*. M118 built the channel, M119 the wait, and with both in
place a renderer could talk and could sleep and **could not share a pixel**.
Mojo carries anything larger than a message as a handle to memory;
`base::WritableSharedMemoryRegion` is `memfd_create` plus `ftruncate`, and
what crosses the channel is the descriptor.

**Why this machine's existing shared memory was not it.**
`kernel/ipc/shm.h` has had shared memory since M19 — it is how the
compositor and every window client share a pixel buffer. Two differences,
and only the second forced a new object: a segment is named by a **global
id every process can guess at**, which is right for a rendezvous and wrong
for a sandbox; and it is **not a descriptor**, so it cannot be passed over
a channel, inherited, counted by the fd table or closed by `SYS_close`.
M118 built descriptor passing, and a thing that is not a descriptor cannot
use it. So this is shm's authority model turned the right way round:
**memory nobody can name and anybody holding the descriptor can map** —
the shape `docs/capabilities.md` argues for everywhere else, arrived at from
the other direction.

**The difficulty was not the memory. It was the lifetime.** `mmap` a memfd
and then close the descriptor: the mapping stays valid. That is POSIX, it is
what every program that shares memory does (Chromium's
`SharedMemoryMapping` outlives the `Region` that made it), and it means the
object is refcounted by **two different things** — every descriptor naming
it, and every mmap region naming it. The second is the new one, and it is
where the work was:

- `mmap_region_t` grew a tag rather than a pointer. A `struct memfd *` there
  would have cost 8 bytes in each of `MAX_MMAP_REGIONS * MAX_TASKS` slots —
  **128 KiB of kernel .bss** — and the tag fits in padding the struct
  already had.
- The tag is (slot, **generation**), which is M54's pid trick against the
  same hazard for the same reason: a slot freed and handed out again must
  not let a stale region name *a different object's memory*. That is the one
  failure in this milestone that would have been silent corruption rather
  than a fault, and a host test rolls the generation over in four lines to
  prove it cannot happen.
- Four paths hand that reference over — munmap removing a slot, execve
  clearing the table, a task slot being recycled, and fork copying the
  table (which takes one rather than dropping one) — plus two that *split* a
  region and so create a second holder. Six call sites, three helpers in
  `sched.c` so each is one line, and the whole of it graded by
  `tests/test_memfd.c` against real reference counts rather than a fake:
  one too few frees memory two processes are reading, one too many leaks
  16 MiB.

**Seals, because read-only sharing is otherwise a comment.** `F_SEAL_WRITE`,
`F_SEAL_GROW`, `F_SEAL_SHRINK` and `F_SEAL_SEAL`, with Linux's rule that
`MFD_ALLOW_SEALING` is required — implemented with the mechanism that
already existed rather than a second flag: a descriptor created without it
gets `F_SEAL_SEAL` immediately, and since seals never come off it can never
be sealed. A seal is what lets a sender hand over memory the receiver can
*verify* is read-only instead of trusting that it is, which is exactly what
`PlatformSharedMemoryRegion::ConvertToReadOnly` does.

**What the instruments said.**

- `tests/test_memfd.c`: **20 tests**. Mutation **82.1% first run, 89.3%
  after**. The survivors were mostly unkillable for one reason and that
  reason was worth acting on: two of them changed assignments in
  `memfd_init` and **nothing could notice**, because every field they set is
  set by `memfd_create_obj` on the way in and cleared by `memfd_unref` on
  the way out. So `memfd_init` does one thing now instead of six. A third
  showed the `name` field was write-only, which is now readable and is what
  the leak check prints.
- **The zeroing test is the one that could not be written on the machine.**
  A frame handed to a second process must not carry what this machine last
  used it for - and a freshly booted machine's frames are mostly zero
  anyway, so a kernel that forgot the `memset` would pass every boot test
  and leak on the hundredth allocation. `tests/fakes/fake_pmm.c` hands out
  frames filled with `0xCD` on purpose, and says so in its own comment; this
  is the first test to depend on that.
- `/bin/memfdtest`, six sections, **100 ms**, and its second section is the
  first time this project has run the whole engine shape: a socketpair, a
  forked child that calls `dropcaps(0)`, a buffer created and sized by the
  parent, the descriptor sent over the channel, the child mapping it and
  writing, and **the parent reading what the child wrote through a mapping
  it made before the child existed and kept after both descriptors were
  closed**.
- The `[m120]` marker adds the claim the program cannot make: **every frame
  came back.** A memfd's frames belong to the object rather than to the
  address space, so a process exiting does not return them - which is
  precisely the leak that needed a counter either side rather than a
  comment.
- `/bin/syscalltest`: 344 checks before, **353** after, 0 failures. Both new
  calls classified; `memfd_create` joins M119's three in `CLASS_SKIP`,
  because it creates a descriptor for any plausible argument and a sweep
  that left twenty behind would move every fd number the later checks
  depend on.
- **One coverage floor was lowered, deliberately and in writing.**
  `sched.c` 51.00% -> 50.83%: M120's two fault-path branches need real page
  tables and this tier has none. They are graded on the machine instead, and
  sharply - the child reads bytes the parent wrote, which only works if the
  fault maps the object's own frame.

**The bug found by reading, and the two failures that were not bugs.**
While the first tier run was booting, the asymmetry between
`sched_regions_forget_memfds` (which walks every region slot) and
`sched_regions_retain_memfds` (which skips empty ones) turned out to
matter: `mmap_slot_remove` shifts the table down by one and leaves a
**duplicate of a live region** in the vacated slot, tag and generation
included - so the forget walk would have dropped a reference nobody took,
freeing memory a process was still mapping. A free slot holds nothing now,
including the tag, and `tests/test_memfd.c` has the shape of the bug
written against the fixed code.

That same tier run failed twice and **neither failure was this
milestone's**, which is worth recording because the temptation was to
assume otherwise. `[m55]`'s pixel check - a compositor SIGKILLed out from
under two clients - read the wallpaper where it expected a window; and the
four-core stage failed, which milestones.md's queue already lists as
*"`smp-test.sh` itself failing 2 runs in 3"*. The same kernel passed
`[m55]` on three other boots, and the archive has the whole phenomenon
written down from M68: roughly fifty boot self-tests are written against
fixed `pit_sleep_ms` budgets, **M55's crash recovery is one of the three
named as wandering when anything perturbs the suite's timing**, and this
milestone grew `task_t` by a kilobyte. A third observation from the same
afternoon: one boot stalled with no output for fifteen minutes, which is
M112's recorded shape, and the machine had a QEMU from a killed run at
100% of a core - the hazard my own notes say to check for before timing
anything. The tier was re-run on a quiet machine and passed every stage.

**What is explicitly not built, each with a condition.** A **shrink** is
refused outright, which is a divergence rather than an omission: Linux
answers a mapping of freed pages with SIGBUS and this kernel has no such
machinery, so it refuses the call instead - the conservative half of the
same answer; the condition is SIGBUS itself. **MAP_PRIVATE of a memfd** -
copy-on-write over anonymous shared memory, which nothing asks for.
**`read`/`write` on one** - Linux allows both, nothing that shares memory
does it, and a second path to the same bytes with different rules is worth
less than the refusal. **16 MiB per object, 32 objects**: the small machine
this project's harnesses must pass on has 128 MiB, so one region at the cap
is already an eighth of it.

**Where this leaves the arc.** Three of `docs/browser.md`'s conditions and
six-of-six's worth of IPC are now answered, and all three were ordinary
kernel work of a few hundred lines each. What remains is not: **clang and
libc++ for `x86_64-lean_os`** (a compiler), a machine with 16 GB of RAM and
100 GB of disk, and a sandbox story that is not a pretence. And the thing
worth saying at the end of three milestones aimed at a browser nobody can
build yet: what they actually bought is a machine on which *any*
multi-process program can be written - a channel, a wait, and a buffer -
and the first programs to use them will be this project's own.

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
- [x] **a real but small engine — done 2026-09-10.** NetSurf 3.11, with
      libcss, libdom, libhubbub and Duktape, over libcurl/mbedtls and
      M66's TCP. `/bin/netsurf` and the **Browser** icon; graded by the
      input suite's `browser_renders_a_page` and by `[m100h]`. Zero
      edits to any of the fifteen third-party projects; the display port
      is one file, because libnsfb registers surfaces at runtime. Five
      gaps in this libc named by the build - pread/pwrite, the lround
      family, scandir, STDIN_FILENO and **iconv, which did not exist at
      all**. See the ninth increment and [docs/browser.md](docs/browser.md).
- [x] **the measurement — done 2026-09-10.** Numbers, not an estimate,
      in [docs/browser.md](docs/browser.md): Chromium's own docs ask for
      8 GB of RAM, 32 GB of swap and 100 GB of disk against this
      machine's 128 MiB, no swap and 2 GiB; its `DEPS` names **535
      sub-repositories**; clang and libc++ are its only supported
      toolchain against this project's GCC 14.2; and its seccomp
      sandbox names **427 syscalls of which this kernel has 79**. The
      one that decides it is not the count: **`AF_UNIX` with
      `SCM_RIGHTS`**, without which there is no Mojo and therefore no
      Chromium at all.
- [x] **`AF_UNIX`/`socketpair` — done 2026-09-11, as M118.** Opened by
      CPython's `test_stat` and named by the measurement above as the
      smallest change with the largest effect on the browser gap. Built
      whole: `socketpair`, `bind`/`connect` by path name and by abstract
      name, `sendmsg`/`recvmsg` with `SCM_RIGHTS`, `MSG_TRUNC` and
      `MSG_CTRUNC`, a real `shutdown`, graded by `[m118]` and 30 host
      tests. What reading it again added to the measurement: the
      condition is **not Chromium's** - WebKit, Gecko and Ladybird pass
      descriptors over a Unix-domain socket too, and none of the four has
      a single-process mode. See [docs/unix-sockets.md](docs/unix-sockets.md).
- [ ] **Two headers named `signal.h`.** Not a missing feature - a
      fragility in this project's own sysroot that the curl port found
      (see the ninth increment). Any third-party build that puts
      `<sysroot>/usr/include` on the compiler line shadows this libc's
      headers with system_api's and breaks `<setjmp.h>`. Worked around
      in `tools/build-netsurf.sh`, not fixed. The fix is to stop having
      two headers with one name.

### M116 — the browser on the real web, graded where the person looks `[x]` (closed 2026-09-11)

- [x] **Wikipedia aborts the browser.** Closed by M117: the condition was
      met (the same NetSurf, built for the host, rendered the page), and
      the failure was this port's - `malloc(0)` returning NULL, read by
      NetSurf's flex layout as out of memory for every empty flex
      container. `https://en.wikipedia.org/wiki/Unix` renders in 17.4 s.
      The condition's guess ("an allocation ... that fails only here")
      was right; the mechanism - a legal refusal, not a failure - was
      not, which is why it took a bisection to find.

### M117 — a desktop that sleeps, a click that shows, and the browser's first backtrace `[~]`

- [ ] **The display task.** The compositor as a scheduling class of
      its own - first at a pick, preempting at a wake, derived from
      `CAP_FRAMEBUFFER` at spawn - was built, and withdrawn: see the
      M117 entry for the three versions and what each did to the
      battery. **The condition**: `tests/test_sched.c` reproduces, on
      the fake CPU, a kernel task polling through `schedule()` while a
      client blocked on the poll channel waits for a wake the
      compositor's event write should deliver - the shape of `[m117]`'s
      738 ms click - and fails against the flag before any version of
      it is tried on the machine again. Without it the desktop draws
      4-7 frames per animation 40-50 ms apart (HEAD: 3-5, 60-100);
      with it, 7-8 and 20-30. That difference is what the box is worth.
- [ ] **A first present that never arrives.** Six graded boots saw
      `wm_zorder`'s presents never reach the compositor when its first
      `sys_pipe_open` of `WM_ACTION_PIPE` happened on the first click
      (details under M117's *what went wrong*). Every client presents
      at startup now, and the number has held since - but the
      mechanism is unknown, and an unknown mechanism is not fixed. **The
      condition**: a host test in `tests/` - `test_pipe` beside
      `test_poll` - that has one task open an existing named pipe and
      write 16 bytes while a second task is blocked in `SYS_waitfds`
      on the poll channel for a different pipe, and reads the 16 bytes
      back from the first pipe; if it passes, the failure is in the
      timing the fake cannot make, and the next instrument is the
      compositor's action-pipe reads logged with `pipe_buffered` on the
      machine.
- [ ] **The I/O APIC battery, twice red in one afternoon, differently.**
      `--full` runs the battery a second time with every legacy line
      through the I/O APIC (M103), a path that costs 2x and that no
      `--full` had ever passed. Two boots of this tree there: one ended
      inside `[m100]`'s library suites at the old 900 s ceiling (now
      1200); one panicked at `[m61]` - "motion is switched off and
      something still animated: 2 pixels lit" - right after an
      animation the compositor reported at 5 frames and a 190 ms gap,
      while a third boot of this tree on the same path passed `[m61]`
      and reached `[m100]`. The shape is the sleeping animation loop
      retiring its last frame a beat late on a slow path, against a
      self-test that samples on a fixed clock; the `[m117]` press on
      that path read 212 ms. **The condition**: `[m61]`'s "motion off"
      check waits for the previous animation's report line (the
      compositor prints one per run now) before it minimizes, and the
      I/O APIC battery is run three times on this tree with that change
      in; if the two pixels come back, the culprit is the compositor's
      end-of-animation frame and `dirty = 1` at animation end has to be
      followed by the redraw in the same pass on every path.
- [ ] **The bootstrap stage hung in bzip2's own test suite.** The
      machine built bzip2 with its own gcc (232 s), ran every compress
      and decompress of the suite, compared the three `.bz2` outputs,
      and stopped inside `cmp sample1.tst sample1.ref` - a read that
      never returned, on a file the shell had just written by redirect
      - until the stage's 3,600 s ceiling. `cmp` is toybox's and reads
      into a static buffer, so the allocator is not in it; `[m89]` and
      `[m94]` ran toybox and bzip2 on the same boot without complaint.
      Whether this predates M117 is unknown: the stage is `--full`-only,
      M100's second increment saw it finish once at cdbca6e, and no
      `--full` since has been recorded. **The condition**:
      `tools/bootstrap-test.sh` on HEAD's image with the native
      toolchain installed (a worktree, `make all`,
      `install-native-toolchain.sh`), then on this tree twice; if only
      this tree hangs, the next instrument is the kernel's read path
      logged for that one `cmp` - which descriptor, which inode, which
      block - because a regular file that never reaches end-of-file is
      the filesystem's bug or the writer's, not the reader's.
- [ ] **The rest of the idle cost.** 11% of a host core with nothing
      open (was 22%), 23% with eight windows (was 44%). The sampler
      says where: 75-80% of samples halted in `pit_sleep_ms`, and 5-12%
      compositing rectangles for presents that timers send whether or
      not anything changed - the shell every 300 ms, settings every
      500 ms - each with a blended shadow under it. **The condition**:
      the same sampler (the scratch script is described in the M117
      entry; it belongs in `tools/` when it is needed a second time)
      run before and after presenting only on change, and the number
      it reports. A few percent of a host core; the crash outranked it.

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
- **Shared memory has two shapes now (M120).** `kernel/ipc/shm.h` is named
  by a global id any process can guess at and is not a descriptor;
  `kernel/ipc/memfd.h` is named by nothing and *is* a descriptor. The first
  is right for a rendezvous (the compositor and its clients), the second for
  handing a buffer to someone specific. A memfd cannot be shrunk - refused,
  because this kernel has no SIGBUS for a mapping of freed pages - cannot be
  mapped MAP_PRIVATE, and cannot be read or written with `read`/`write`.
- **`AF_UNIX`/`socketpair` exist as of M118** - with `SCM_RIGHTS`, an abstract namespace and a real `shutdown`, needing no capability ([docs/unix-sockets.md](docs/unix-sockets.md)). Two divergences remain and are written down there rather than here: a bound path is a name in a kernel table and not a node in leanfs (so `stat()` on it fails and `unlink()` does not unbind), and `SCM_CREDENTIALS` does not exist because two of its three numbers would be constants on a machine with one principal. `SOCK_DGRAM` on this family is not built; its condition is a program that sends to a bound name without connecting.
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
- **A browser — half of this is no longer deferred, and the other half
  now has a condition instead of an estimate.** The list said "HTML,
  CSS, layout, a JS runtime, TLS, GPU compositing, codecs, a sandbox,
  and a Linux-scale syscall surface". **M100's ninth increment
  delivered the first five** by porting NetSurf rather than writing an
  engine - it is `/bin/netsurf` and it renders pages, runs JavaScript
  and fetches over https. What is still deferred is a browser of
  *Chromium's* kind, and [docs/browser.md](docs/browser.md) replaces the
  estimate with measurements: 100 GB of checkout, 535 repositories,
  clang-and-libc++ only, and 427 sandbox syscalls against this kernel's
  79. **That condition was `AF_UNIX` with `SCM_RIGHTS` - and M118 built
  it** (2026-09-11), which also corrected the measurement: the same
  feature is in front of WebKit, Gecko and Ladybird, none of which has a
  single-process mode either, so it was never Chromium's condition
  alone. **M119 then closed the second** (2026-09-12): `epoll`, `eventfd`
  and `timerfd`, which is `base`'s whole message pump - and which also
  gave this kernel write-readiness it had never had. **M120 added
  `memfd_create`** the same day, which was not one of the five conditions
  but was one of the six absent syscalls behind them: with it a buffer can
  cross a channel, which is how Mojo moves anything bigger than a message. **The condition is
  now the third, and it is where this stops being ordinary work: clang
  and libc++ for `x86_64-lean_os`.** The two that are done were a few
  hundred lines of kernel each; this is a second compiler port, with
  M94's nine edits as the template for what that costs. After it: a
  machine with 16 GB of RAM and 100 GB of disk, and a sandbox story that
  is not a pretence - Chromium's code calls `seccomp` and namespaces and
  this OS assigns a capability set at spawn, which is not the same shape.
  Chrome itself is proprietary and so not a porting question at all. GPU
  compositing stays refused on its own terms above.
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
- **A twin that was not a twin proves nothing, and it will not say so.**
  M117 built "the same NetSurf, for the host" three times with the
  wrong switches - a script wrote its own config over the one being
  edited - and each one rendered the page the guest died on, for a
  reason that had nothing to do with the guest. `nm` on the binary was
  the check that should have come first; `tools/build-netsurf-host.sh`
  does it before it calls the result a twin.
- **In a third-party tree, `build/` may be source.** NetSurf's
  libraries keep their build *scripts* there and their objects in
  `build-<host>-<target>-…`. `rm -rf */build` to force a rebuild deleted
  `libparserutils/build/make-aliases.pl` and `make` then failed with no
  output at all - `make -d`, 7,960 lines in, was the only thing that
  said why. The tarball is what restored it, byte for byte.
- **A legal refusal is the hardest kind to find.** `malloc(0)` returning
  NULL is permitted by C, was tested for, and was wrong for every
  program written against every other libc. It failed one layout
  silently, sixteen milestones after it was written, and only a
  backtrace plus a bisection of a real page could name it.
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
