# Networking

What this OS can do on a network, what it deliberately cannot, and how a
program uses it.

Built across two milestones with thirty-six between them: **M27** put an
RTL8139 driver, Ethernet, ARP, IPv4 and ICMP in the kernel, and **M64**
made all of it reachable from a program.

## The stack

```
user program            socket(2)-shaped syscalls
  |
  |  SYS_socket / bind / sendto / recvfrom / sockpoll
  v
kernel/net/socket.c     32 sockets, datagram or stream,
  |                     each entry a refcounted fd_slot_t
  |
  +-> kernel/net/tcp.c  16 connections: the eleven-state machine,
  |                     retransmission with an RFC 6298 timer,
  |                     Reno congestion control, 4 KiB each way
  v
kernel/net/udp.c        ports + checksum over RFC 768's pseudo-header
  |
  v
kernel/net/ip.c         IPv4, no options, no fragmentation,
  |                     one on-link/off-link routing decision, loopback
  v
kernel/net/arp.c        a small cache, opportunistic learning
  |
  v
kernel/net/ethernet.c   Ethernet II framing
  |
  v
kernel/drivers/rtl8139.c
```

`kernel/net/dhcp.c` sits beside all of this rather than on top of it: it
has to send before there is an address to bind a socket to, so it drives
`udp_send` directly and takes its replies through a kernel-side handler
registered for port 68.

## One thing that will surprise you

**A `sendto` to an address nobody has ARPed yet returns -1.** The packet
is dropped while the ARP request goes out; send again and it works. That
is real BSD behaviour and not a shortcut, but the reason it is *forced*
here is worth knowing:

`int 0x80` goes through an interrupt gate, so IF is clear for the whole
of a syscall. Nothing inside one can wait for a timer, because the timer
interrupt cannot fire - not with `hlt`, and not by yielding either, since
the tick that would advance any deadline is the interrupt that is
blocked. So `resolve_neighbor` waits only when it is called with
interrupts on (kernel context), and returns immediately otherwise.

If this ever becomes a real limitation, the fix is a **trap gate** on
vector 0x80 so IF stays set through a syscall. That is what production
kernels do. It also makes every syscall in the system preemptible at
once, which is why it has not been done casually.

## Addresses

Host-order `uint32_t`, everywhere - in the kernel, in `os_net.h`, and in
every program. `10.0.2.2` is `0x0A000202`. There is no `htons` in this OS
and there does not need to be one: it only ever runs little-endian, never
puts an address in a register the wire format has to match, and reads
every header field out byte by byte at the point of use. A byte order
that changed at the API boundary would be a second representation with a
conversion between them to get wrong.

`system_api/include/os_net.h` has `OS_IPV4(a,b,c,d)`, `os_ip_to_string`
and `os_ip_from_string`.

## Writing a program that uses it

```c
#include "os_net.h"
#include "syscall_wrappers.h"

int fd = sys_socket();
sys_bind(fd, 0);                    /* 0 asks for an ephemeral port */
sys_sendto(fd, OS_IPV4(10,0,2,2), 123, packet, sizeof(packet));

while (sys_uptime_ms() < deadline) {
    if (sys_sockpoll(fd) > 0) {
        os_sockaddr_t from;
        long n = sys_recvfrom(fd, buf, sizeof(buf), &from);
        ...
    }
    sys_yield();
}
sys_close(fd);
```

A socket is an ordinary file descriptor. It is closed by `sys_close`,
inherited by a child across `sys_spawn`, and duplicated by `sys_dup2` -
because it is an entry in the same table as pipes and open files, not a
handle of its own kind.

`sys_recvfrom` **never blocks**. There is no blocking receive in this OS;
`sys_sockpoll` is how a caller tells "nothing yet" from "nothing ever",
and the loop above is the shape every network program here takes.
`user_space/lib/sntp.c` is a complete worked example in sixty lines.

## TCP

```c
/* server */
int l = sys_socket(OS_SOCK_STREAM);
sys_bind(l, 8080);
sys_listen(l);
while (sys_sockpoll(l) == 0) { sys_yield(); }
os_sockaddr_t who;
int c = sys_accept(l, &who);

/* client */
int c = sys_socket(OS_SOCK_STREAM);
sys_connect(c, OS_IPV4(127,0,0,1), 8080);   /* starts the handshake */
while (sys_connstat(c) == 0) { sys_yield(); }  /* 1 = up, -1 = refused */

/* both */
long put = sys_send(c, buf, len);   /* may be partial, may be 0 - loop */
long got = sys_recv(c, buf, max);   /* 0 = nothing yet, -1 = end of stream */
sys_close(c);                       /* an active close: FIN, then TIME_WAIT */
```

Three things that differ from Berkeley sockets and matter:

- **`connect` does not block.** It starts the handshake and returns;
  `sys_connstat` gives 1, 0 or -1. There is no blocking anything in this
  OS's network API, for the reason the header says: a blocking connect
  would need wait-queue machinery the kernel has never had, and adding
  it for TCP alone would be a scheduler change hiding inside a
  networking milestone.
- **`send` is partial by design.** It takes what fits in the 4 KiB send
  buffer and tells you how much. Callers loop.
- **`recv` returns 0 and -1 for different things.** 0 is "nothing right
  now", -1 is end of stream - the peer closed and the buffer is drained,
  or the connection was reset. A reader that conflates them either spins
  forever or stops early.

What TCP here does *not* have: window scaling, SACK, timestamps, PAWS,
Nagle, urgent data, and out-of-order reassembly. The last is the
interesting one - a segment arriving ahead of a gap is dropped rather
than held, which RFC 793 permits, costs a round trip when it happens,
and halves the size of the receive path.

## What is not here

- **DNS.** Which is why `nettime` takes an address rather than a name.
  `netconf` prints the DNS server DHCP handed over; nothing resolves
  through it yet.
- **DHCP renewal.** The client gets a lease and never renews it. A
  machine that runs past its lease loses its address. This OS has never
  run for a week.
- **Raw sockets.** ICMP is kernel-only, so there is no `ping` program -
  the boot self-test pings the gateway from inside the kernel.
- **A server that outlives the program that started it.** There is no
  inetd, no daemon, nothing listening on this machine by default. The
  only TCP endpoints that exist are the ones a running program made.
- **A permission model.** Any process can open a socket and talk to
  anything. See the stretch-goal entry in `milestones.md`; a network is
  the thing that makes it stop being theoretical.
- **Fragmentation, IP options, multiple interfaces, a routing table.**
  One interface, one subnet, one gateway.

## Testing it

`user_space/bin/nettest.c` is the self-test, in user space on purpose -
what M64 added is the *syscall* surface, and a test that called the
kernel's socket functions directly would prove the layer underneath the
one that is new. The kernel spawns it and grades its exit code.

Most of what it asserts runs over **loopback** (127.0.0.1), so the
substance of the test does not depend on the emulator's network answering
anything. `ip_send` short-circuits anything addressed to this machine
back up the stack - building a real header rather than taking a shortcut
around one, so a loopback datagram takes the path a real one takes.

Loopback delivery is **queued, not recursive**. Handing a packet straight
back up with a direct call is fine for UDP, where delivering a datagram
cannot produce another one. It is not fine for TCP, where a data segment
produces an ACK which lets the sender produce more data: a whole transfer
would run inside one recursive call chain, dozens of frames deep, every
level clobbering the buffer the level below was reading. So a loopback
send appends to a 32-slot queue, and the outermost caller drains it in a
loop - stack depth stays at two no matter how much traffic one `send`
sets off. A full queue drops the packet, which is what a congested
interface does.

`user_space/bin/tcptest.c` is the TCP one, also over loopback and also
both ends of every connection. Two of its claims are worth calling out:

- **A 16 KiB transfer through a 4 KiB buffer**, compared byte for byte.
  That is what exercises the window, the congestion window, buffer
  compaction on every ACK and a sender that has to stop and resume; a
  one-segment "hello" touches none of it.
- **Retransmission on a link that cannot lose anything.** Loopback is a
  perfect network, which makes it the worst possible place to discover
  that the recovery path is broken. So `tcp_debug_drop_next()` - not
  reachable from user space - tells the stack to throw away the next few
  segments *after* building them and advancing the sequence numbers, and
  the self-test demands both that the transfer still completes and that
  a retransmission actually happened.

Two things the UDP test checks that are easy to leave out:

- That **DHCP actually got a lease**. On QEMU the leased configuration is
  byte-for-byte the fallback one, so a client that sent nothing would
  produce an identical result and an identical log line.
  `net_config_is_leased()` is the only thing that can tell them apart.
- That the things that should fail **do fail, and without a panic**.
  Sending to an address nothing answers ARP for used to halt the machine.
