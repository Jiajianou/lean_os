# Waiting, on this machine

*M119, 2026-09-12. `epoll`, `eventfd` and `timerfd`: the second of the five
conditions in [browser.md](browser.md), and the one every event loop
written in the last twenty years is built on.*

---

## The four ways to wait here, and which to use

| call | what it answers | since |
|---|---|---|
| `SYS_waitfds` | one ready descriptor out of a handful, or a deadline | M68 |
| `poll` / `select` | a mask over an array the caller rebuilds each time | M88 |
| **`epoll_wait`** | **events from a set the kernel remembers, with a cookie each** | **M119** |
| a blocking `read` | this one descriptor, and nothing else | M14 |

They are all the same wait underneath — a task leaves the run queue and an
arrival wakes it (`SCHED_POLL_CHAN`) — and all of them exist because the
shapes are genuinely different, not because the newest replaces the oldest.
The desktop's window clients use `SYS_waitfds` (M117) and should keep doing
so: one channel, one event, one index back.

## What epoll buys, and what it does not

**It does not buy speed, and this is worth saying plainly because the usual
argument for epoll is exactly that.** The textbook case is O(1) against
`poll`'s O(n); with `MAX_FDS` at 128, a linear scan of a process's whole
descriptor table is a few hundred cycles, and M69's rule is that
performance work on an unmeasured path does not get done here.
`kernel/ipc/epoll.c` scans its set linearly and says so in its header.

What it buys is three things `poll` cannot express at all:

1. **A set the kernel remembers.** A pump registers once instead of
   rebuilding an array every pass.
2. **A cookie.** `epoll_event.data` comes back with the event, so a ready
   descriptor maps to its handler with no side table. This is the feature
   ported code leans on hardest.
3. **Edge-triggered and one-shot modes**, which are statements about what
   changed *since the last call* — something a stateless interface cannot
   make.

And `eventfd` and `timerfd` turn "wait for descriptors" into the only
primitive a pump needs: another thread pokes the counter to be heard, and a
deadline is a descriptor like any other.

## Write-readiness, which this kernel did not have

Before M119 nothing here could answer "would a write block?". `<poll.h>`
reports `POLLOUT` for any open descriptor and has said why since M88:
there was no such answer available, and *ready* was a better lie than
never reporting writability at all, which would have made a program
waiting for it wait forever.

`epoll` tells the truth. `EPOLLOUT` means a pipe with room, a socket with
send-buffer space, a Unix-domain socket whose peer's buffer and record
queue both have room, a counter below saturation. A descriptor whose write
would *fail* rather than block — a pipe with no reader — reports
`EPOLLERR`, because a pump told "writable" there would retry forever.

**So two calls in this kernel now answer the same question differently,
deliberately.** `poll` keeps M88's answer because M116 measured the
browser's behaviour through that path and changing it on a guess is
precisely what this project does not do. The condition for unifying them:
**a measured case of a program spinning in `poll` on a descriptor that is
not in fact writable.** epoll is now the instrument that can produce one.

## The three objects

### eventfd — a counter you can wait on

A 64-bit counter. A write adds; a read takes the whole thing (or exactly
one, with `EFD_SEMAPHORE`) and blocks while it is zero. It saturates one
short of 2<sup>64</sup> and a write that would cross that is `EAGAIN`,
never a wrap — a wrapped counter is a lost wake-up, which is the one
failure this object exists to avoid.

A pipe could carry the same wake-up and `SYS_pipe` has existed since M14.
The difference: a pipe costs two descriptors and 4 KiB to carry one bit,
and a wake-up written more often than it is read fills that buffer and then
**blocks the waker**.

### timerfd — a deadline you can wait on

**The granularity is 10 ms**, which is `PIT_HZ` and not a choice this code
makes. A request shorter than a tick is rounded **up**, never down to zero:
a timer that is readable before its time is an event loop that spins.

A read returns *how many times it fired since the last read*, so a 10 ms
periodic timer on a machine that was busy for 100 ms reports ten rather
than one. The reader learns it fell behind, which is the only honest
answer and the one Linux gives.

An absolute `CLOCK_REALTIME` deadline is converted through the wall clock,
whose resolution here is **one second** (`rtc_read` reads a calendar, not a
counter). `CLOCK_MONOTONIC` is what a pump should use, and what every
relative timer uses whatever it asked for.

### epoll — the set

Linux's events, values and `epoll_ctl` operations. Level-triggered by
default; `EPOLLET` reports only what is newly set since the last report;
`EPOLLONESHOT` disarms a registration until a `MOD` re-arms it. `EPOLLERR`
and `EPOLLHUP` arrive whether or not they were asked for, as on Linux,
because a program that did not ask about an error still has to be told.

**A registration does not hold its descriptor open**, and this is the one
place M119 diverges from Linux on purpose. Linux registers the open *file*;
here the set records the descriptor number *and what it pointed at*, and a
closed or reused descriptor is dropped by the next `epoll_wait` rather than
reported against whatever took the slot. Holding a reference would keep a
pipe alive because somebody forgot to deregister it — a leak wearing an
epoll set as a disguise — and reporting the old file, as Linux would, is
the less conservative of the two answers.

## Divergences, each with its condition

- **Nesting is refused.** An epoll set cannot watch another epoll set. It
  would mean a scan inside a scan with two sets' locks held in an order
  nothing controls, and nothing this project is aiming at does it —
  Chromium's pump has one set per thread. *Condition: a program that puts
  an epoll fd in an epoll set.*
- **`EPOLLEXCLUSIVE` and `EPOLLWAKEUP` are accepted and ignored.** One
  waiter per set here, and nothing suspends.
- **`EPOLLPRI` is defined and never set**, for the reason `<poll.h>`
  argues at length about `POLLPRI`: M99 reversed the original decision to
  omit it, because an absent constant is a compile error and an unraised
  one is not.
- **`signalfd` is not built.** Chromium's seccomp list names `signalfd4`,
  but `base` does not call it — signals are handled with `sigaction`.
  *Condition: a program that waits for a signal through a descriptor.*
- **32 registrations per set, 64 sets, 64 counters, 64 timers.** Refusals
  with numbers behind them rather than tables that grow. A pump watches a
  handful.
- **`struct epoll_event` is not glibc's 12-byte packed layout.** It is 16
  bytes with `data` aligned, which is exactly the kernel's
  `os_epoll_event_t`, so `epoll_wait` is a copy and not a translation.
  Nothing that uses the struct and the macros can tell.

## What is graded, and by what

| instrument | what it can say |
|---|---|
| `tests/test_readyfds.c` (29 tests, `--fast`) | the three objects at nanosecond precision: a timer at 1099.999 ms and at 1100, a counter at saturation, an edge-triggered registration on its second scan. Mutation scores **87.5% / 83.3% / 95.7%** |
| `/bin/epolltest`, eight sections, `[m119]` | the real clock and the real scheduler: a 60 ms timer that fired at 60, `EPOLLOUT` told the truth about a full pipe, a wake from another process |
| the same test's idle-tick measurement | **that `epoll_wait` sleeps.** 200 ms of waiting, ≥10 of 20 ticks idle, measured with `SYS_idle_ticks` — the counter M68 added because a spin and a sleep are indistinguishable from outside |
| the `[m119]` marker's own check | every counter, timer and set the program made was given back |
| `/bin/syscalltest` (`[q5]`) | all seven new calls: 313 checks before this milestone, **344** after |

Two of those deserve a note. The idle-tick assertion is the only kind of
test that can catch the failure this milestone was most likely to ship — a
`poll`-shaped implementation that works perfectly and burns a core — and
it exists because M68 had already built the instrument for exactly that.
And the `epoll_wait(-1)` section **hangs** rather than fails if the park's
deadline is not computed from the armed timers, because nothing in this
kernel interrupts when a deadline passes; a hang reaches the harness as
`[m119]` never appearing, which is the right way for that bug to be
reported.

## Where this leaves the Chromium arc

Conditions 1 and 2 of five are closed ([browser.md](browser.md)). What
remains is not a port: **clang and libc++ for `x86_64-lean_os`**, a
machine with **16 GB of RAM and 100 GB of disk**, and a **sandbox story**
that is not a pretence — Chromium's code calls `seccomp` and namespaces,
and this OS's answer to "what may this process do" is a capability set
assigned at spawn. Those are not the same shape, and pretending otherwise
would produce exactly the kind of thing M65 refused.

What these two milestones did buy, beyond Chromium: every multi-process
engine's IPC layer (M118) and every modern event loop's core (M119). The
next thing that could actually run here because of them is not Chromium —
it is anything built on libevent, glib or Mojo.
