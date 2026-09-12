# Unix-domain sockets, and why they were built before the browser

*M118, 2026-09-11. The subsystem is `kernel/ipc/unixsock.c`; the ABI is
five calls in `system_api/include/syscall.h`; the user-facing half is
`<sys/socket.h>` and `<sys/un.h>`.*

---

## What this is for

[docs/browser.md](browser.md) measured what a browser of Chromium's kind
asks of this machine and ended with five conditions. The first is the
only one that is not a number:

> **`AF_UNIX` with `SCM_RIGHTS`.** Without descriptor passing there is no
> Mojo, and without Mojo there is no Chromium — not a slow one, not a
> limited one, none. This is the single smallest change with the largest
> effect on the list, it is a few hundred lines … **It should be the next
> thing.**

Asking the same question of the other three modern engines said
something the measurement had not. They all need it too:

| engine | its IPC layer | what it does |
|---|---|---|
| Chromium / Blink | **Mojo** | `socketpair(AF_UNIX, SOCK_STREAM)`, descriptors over `SCM_RIGHTS` |
| WebKit (GTK and WPE) | **`IPC::Connection`** | `AF_UNIX`, `sendmsg` with `SCM_RIGHTS` |
| Gecko / Firefox | **IPDL** | a `socketpair`, descriptors passed as `FileDescriptor` |
| Ladybird | **LibIPC** | `SOCK_SEQPACKET` + `sendfd`/`recvfd` |

None of the four still has a supported single-process mode. So this is
not Chromium's price of admission — it is **every multi-process engine's**,
and that is what made it worth building before any of them was chosen.
A port that picks the engine first and discovers this second has the
same gap with a deadline attached.

The one modern engine that would *not* need it is Servo, which is
single-process; its own condition is a Rust `std` port for
`x86_64-lean_os`, which is a different milestone and a larger one.

## What was built

- `socket(AF_UNIX, SOCK_STREAM | SOCK_SEQPACKET, 0)`, `socketpair`,
  `bind`, `listen`, `connect`, `accept`, `read`, `write`, `sendmsg`,
  `recvmsg`, `shutdown`, `close`, and `fstat` reporting a socket.
- **`SCM_RIGHTS`**: a descriptor of any kind — a pipe end, an open file,
  a TCP socket, another Unix socket — crosses to another process as a
  reference to *the same kernel object*. The receiver's descriptor shares
  the sender's file position, because it is the same open-file entry
  (`kernel/fs/openfile.h`), and that is the claim `/bin/unixtest`'s third
  section is built around: the parent reads two bytes of a ten-byte file
  and passes the descriptor, and the child reads the remaining eight.
- `MSG_TRUNC` and `MSG_CTRUNC`, reported rather than inferred.
- The **abstract namespace** (a name whose first byte is NUL), which is
  what Chromium's sandbox and most Linux programs use.
- A half-close that is real: `shutdown(SHUT_WR)` reaches the peer as an
  end of stream with the descriptor still open.

`AF_UNIX SOCK_DGRAM` is **not** built. Its condition: a program that
sends to a bound name without connecting first.

## The four decisions worth arguing

### 1. It is IPC, not networking

`kernel/ipc/unixsock.c`, beside the pipes — not `kernel/net/`. A
Unix-domain socket shares nothing with that directory but a spelling: no
address, no checksum, no retransmission, no device. What it shares with
`pipe.c` is everything. Putting it in `kernel/net` would have meant one
`net_lock` held over a channel no packet can reach, and would have
answered the next question wrongly by default.

### 2. It needs no capability, and that is the point

`SYS_socket` checks `CAP_NETWORK`. An `AF_UNIX` socket does not.

A capability here answers *what may this program reach*
([capabilities.md](capabilities.md)), and what an `AF_UNIX` socket
reaches is another process on this machine that is already listening for
it — which is what a pipe reaches, and pipes need no capability.

Gating it on `CAP_NETWORK` would also have been precisely backwards for
the thing it exists for. A renderer process is the one program on the
machine that must hold **no** network capability, and it is the one
program that cannot work without this call. `/bin/unixtest`'s last
section is that assertion: a child calls `dropcaps(0)`, is refused an
`AF_INET` socket, and then talks to its parent and uses a descriptor the
parent handed it.

What the capability model still decides is what travels. A passed
descriptor carries exactly the authority it already had, because what
crosses is a reference to the same object. A process cannot manufacture
authority by sending it — and it *can* hand over authority it holds,
which is what the call is for. A capability set still only ever shrinks.

### 3. Nothing in this file parks

Every call in `unixsock.c` is non-blocking, including the two a program
experiences as blocking. `syscall.c`'s read and write loops do the
parking, around a non-blocking `unixsock_recv`.

That is `kernel/net`'s arrangement rather than `pipe.c`'s, and
deliberately: `tcp.c` never parks a task, which is what makes it a unit
a host test can drive. `tests/test_unixsock.c` is 30 tests for the same
reason, and the whole of `SCM_RIGHTS`' reference counting is reachable
from there — against `tests/fakes/fake_kernel_objects.c`, which counts
the references on a passed descriptor, so "the receiver got one and the
sender's own is untouched" is a number rather than an argument.

### 4. The kernel's message is bytes and descriptors, not a `msghdr`

`SYS_sendmsg` takes an `os_msg_t`: a buffer, a length, and a list of
descriptor numbers. `<sys/socket.h>`'s `struct msghdr` — the iovec array
and the `cmsghdr` records walked with `CMSG_NXTHDR` — is a *user-space
calling convention* with alignment rules, and parsing it in the kernel
would mean the kernel validating somebody else's convention. libc does
it, at the same seam that already converts byte order for `sockaddr_in`
(`user_space/libc/src/socket.c`).

## Divergences, stated rather than discovered

- **A bound path is not a filesystem node.** `bind("/tmp/x.sock")` claims
  a name in a kernel table; nothing appears in the directory, `stat()` on
  it fails, and `unlink()` does not unbind it. That is exactly what
  `pipe_named` has been since M20, and it is enough for two programs that
  agree on a string. *Condition for changing it: a program that needs
  `stat()` or `unlink()` on the path to succeed, or two programs
  rendezvousing through a path neither hard-coded.* The abstract
  namespace has none of this half-truth in it and is the better choice
  here.
- **`SCM_CREDENTIALS` does not exist.** It hands a peer a pid, a uid and
  a gid; this machine has one principal, so two of those three would be a
  constant, and M65's rule refuses a check with nothing behind it.
  Chromium's `base::UnixDomainSocket` uses it to learn a peer's pid.
  *Condition: the same one multi-user names.*
- **Eight descriptors per message**, against Linux's 253 and Chromium's
  own `kMaxFileDescriptors` of 16. Matching Linux would be 60 KiB of
  kernel memory per socket for a message nothing sends; a caller asking
  for more is refused rather than truncated.
- **4 KiB per direction, 16 records in flight.** A full buffer and a full
  record queue both read as "would block", never as an error, because a
  sender that loops on a refusal it cannot fix is a hang.

## What is graded, and by what

| instrument | what it can say |
|---|---|
| `tests/test_unixsock.c` (30 tests, `--fast`) | the object: every boundary, every refusal, and the exact reference count on a passed descriptor. Mutation score **87.2%** |
| `/bin/unixtest`, ten sections, `[m118]` | the syscalls, in two processes: a descriptor that really crossed, a blocking read really woken, a child with no capabilities |
| the `[m118]` marker's own two checks | what the program cannot see about itself: every socket it made was given back, and no passed descriptor is still queued |
| `/bin/syscalltest` (`[q5]`) | all six new calls swept with hostile arguments — 288 checks before this milestone, 313 after |

`/bin/syscalltest` is worth a line of its own: it failed the first graded
boot of this work, because its table requires *every* syscall number to
be classified and six new ones were not. It also made the three
name-taking calls validate their user pointer *before* looking at the
descriptor, so that the pointer check is not satisfied by the descriptor
lookup failing first.

## What this does and does not unblock

It closes condition 1 of five in [browser.md](browser.md). **Condition 2
is next and is not optional**: an epoll-shaped readiness interface plus
`eventfd` and `timerfd`. Chromium's message pump calls `epoll_wait`, not
`poll`; this kernel has `poll`, `select` and `SYS_waitfds`. Conditions 3
to 5 — clang and libc++ for `x86_64-lean_os`, a machine with 16 GB of RAM
and 100 GB of disk, and a sandbox story that is not a pretence — remain
an arc rather than a port, and Chrome itself remains proprietary and
therefore not a porting question at all.
