# tests/

The host tier: kernel code compiled for the machine you are sitting at,
and run in under a second.

Everything here is additive. Not one boot self-test was deleted to make
room for it, and the two tiers answer different questions - see
[check.h](check.h) for the long version of that argument, which is worth
reading before adding anything here.

```
check.h              the whole framework: CHECK, REQUIRE, CHECK_PANIC
runner.c             the registry, the reporting, main()
budgets.tsv          performance ceilings, with the commit each was measured at
fakes/               the seams
  lib/spinlock.h     shadow: the real one is x86 inline assembly
  arch/x86_64/io.h   shadow: the real one is port I/O
  fake_pmm.c         frames from malloc, with a settable failure point
  fake_vmm.c         a real host mmap behind vmm_map_page
  fake_blk.c         a RAM disk that counts what it was asked to do
  fake_panic.c       panic() as a longjmp, so a test can assert one
  fake_panic_abort.c panic() as an abort, for the fuzzers
  ...
fuzz/                libFuzzer targets
corpus/              inputs worth keeping, including anything that ever crashed
```

## Running

```sh
make test-fast                      # everything quick
make test-fast TEST_FILTER=leanfs   # one suite
make test-fast TEST_SAN=0           # without ASan/UBSan, for timing
./build/tests/leanos-tests --slow   # including the tests that exhaust real resources
make coverage                       # per-file coverage of the units under test
make coverage-check                 # ...and fail if any file went down
make mutate                         # does this suite detect anything? (Q12)
make mutate FILE=kernel/mm/heap.c   # one file
make fuzz-run                       # 60 seconds per fuzz target
tools/crash-test.sh                 # 16 power cuts, then check the filesystem
tools/leanfs-fsck.py <image>        # an independent structural check
```

## Two rules worth stating

**Shadow a header only when there is no honest host version of it.** Two
qualify - `lib/spinlock.h` and `arch/x86_64/io.h`, both of which are
architecture rather than logic. Everything else is a fake `.c` linked in
place of the real one, so the code under test is byte for byte the code
that ships.

**A fake must include the header it stands in for.** `fake_socket.c` was
written without one and its parameter order came out wrong; it linked
silently, because nothing cross-checks a definition against a prototype it
never saw. A fake that does not match is worse than no fake — every
assertion built on it is confidently wrong.

## Read the mutation score before the coverage number

`make coverage` says which lines ran. `make mutate` says whether anything
would have noticed if they were wrong, and the two disagree more than is
comfortable:

| file | line coverage | mutation score |
|---|---|---|
| `net/ethernet.c` | 100% | 88.5% (**was 0.0%**) |
| `mm/heap.c` | 100% | 89.4% |
| `net/udp.c` | 96% | 80.5% (**was 3.7%**) |
| `net/arp.c` | 84% | 76.6% |
| `lib/libk.c` | 100% | 73.8% |
| `net/icmp.c` | 74% | 59.4% |
| `dev/fwcfg.c` | 98% | 49.5% |
| `net/ip.c` | 62% | 40.9% |

`ethernet.c` is the one worth remembering: it sat at **100% line coverage
with a 0% mutation score** - every line ran and not one injected fault
was detected, because every test that "covered" it asserted only that a
malformed frame produced *no* reply.

The lesson generalises: **a test that asserts an absence constrains
almost nothing.** Dropping a bad packet is one bit of behaviour; the
several hundred bits that matter are in the reply. If you are adding a
test here, the question to ask is not "does this line run" but "what
would this test say if the line were wrong".

## Coverage, as of Q11-Q20

Measured, not estimated, and it covers only the units the host tier
builds. It is **not** a number for the kernel as a whole and should not be
quoted as one.

| file | line coverage |
|---|---|
| `lib/libk.c` | 100% |
| `mm/heap.c` | 100% |
| `net/ethernet.c` | 90% |
| `net/arp.c` | 84% |
| `net/icmp.c` | 74% |
| `net/ip.c` | 62% |
| `fs/leanfs.c` | 56% |
| `net/udp.c` | 28% |
| `net/tcp.c` | 63% (**was 4%**) |
| `dev/fwcfg.c` | 98% |

`tcp.c` was the honest one: 3.74%, the single largest gap in the suite,
named as such from Q8 until Q11 closed it. `tests/test_tcp_states.c`
drives the eleven-state machine one segment at a time - `tcp_handle_packet`
for arriving bytes, `tcp_tick` for the clock, so nothing waits on real
time - and found three remotely-triggerable defects on its first run: a
RST believed at any sequence number, a control block leaked when a
connection was killed before it was accepted, and a `tcp_abort` that did
not free.

Two things it taught that are worth carrying to the next file:

- **The checksum is not optional.** The first draft left it zero and all
  nineteen tests failed identically with nothing transmitted, because
  `tcp.c` correctly dropped every segment. UDP's exemption for a zero
  checksum does not apply here.
- **Compute the expected value independently.** Both the TCP and UDP
  checksum helpers in these tests are written out by hand rather than
  calling the kernel's own. A test that asks the code under test what the
  answer should be proves only that it agrees with itself.
