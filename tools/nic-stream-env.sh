# tools/nic-stream-env.sh - sourced, not run: the host's end of [m116]'s
# stream, and the guest RAM every boot that has one must be given.
#
# The stream is a QEMU guestfwd: a connection to 10.0.2.100:7777 runs `cat` on
# a file this side wrote, and fw_cfg says how many bytes to expect. SLIRP runs
# that command the moment the guest's handshake completes, through
# g_spawn_async_with_fds with a child-setup function - which is fork(2), not
# posix_spawn, and it is fork in the process that holds the guest's memory.
#
# On Linux QEMU marks guest RAM MADV_DONTFORK (ram_block_add, in QEMU's
# system/physmem.c) and the fork never sees it. macOS has no MADV_DONTFORK -
# QEMU's madvise.h makes it a no-op - so the child inherits every page the
# guest has touched, copy-on-write. Under TCG that went unnoticed, because
# there it hardly matters: the fork stops the guest for 140-210 ms with the RAM
# shared or not (nic-stream-test at 4 GiB: 138 ms of "connect" shared, 147
# private - what it copies under TCG is QEMU's own, not the guest's), and that
# is what the battery's TCG number still carries. Under hvf, where those pages
# are also mapped into the hypervisor, the fork froze the whole VM - every
# vCPU, the PIT, the NIC - for about 4.3 s per GiB touched: by [m116] the
# battery's [m102] has touched all of it, and netrecv measured 4293 ms of
# "connect" at QEMU_MEM=1024, 17423 ms at 4096 (35171 ms with LEANOS_IOAPIC=1)
# and, in tools/nic-stream-test.sh, 68155 ms at the ThinkPad's 16384 - against
# a 1000 ms budget. The machine was not slow; it was stopped, and its clock
# agreed with the host's: the serial lines, timestamped on this side, are
# 17.6 s apart across the stall. A packet capture (-object filter-dump) shows
# the guest's SYN, SLIRP's SYN-ACK and the guest's ACK in the same millisecond,
# no ARP or SYN retry, then nothing for 17.6 s until the first byte - and with
# the command changed to `sleep 3; cat`, connect still took the freeze while
# read took the three seconds, so the guest's own connect returns the moment
# the handshake lets it.
#
# Shared guest RAM is not copy-on-write across a fork: the child shares the
# mapping until it execs, and nothing has to be write-protected. With it, the
# hvf battery measured 76 ms at 4 GiB, 58 ms at 8 cores and 16 GiB and 159 ms
# with LEANOS_IOAPIC=1; TCG stayed where it was (368 ms). Nothing else about
# the machine changes - the backend's id is pc.ram, which is the name -m alone
# gives the same memory.
#
# guest_ram_args MiB   sets GUEST_RAM_ARGS to the QEMU arguments for MiB of
#                      RAM, shared, replacing a plain -m.
#                      LEANOS_PRIVATE_GUEST_RAM=1 gives the plain -m back:
#                      the A/B, for showing what the sharing is for.
# nic_stream_prepare   writes the stream file and sets NIC_STREAM_BYTES,
#                      NIC_STREAM (the file - the caller removes it) and
#                      NETDEV (a -netdev user,... value with the guestfwd).

guest_ram_args() {
  if [ "${LEANOS_PRIVATE_GUEST_RAM:-0}" = "1" ]; then
    GUEST_RAM_ARGS=(-m "$1")
    return
  fi
  GUEST_RAM_ARGS=(-m "$1"
                  -object "memory-backend-ram,id=pc.ram,size=$1M,share=on"
                  -machine memory-backend=pc.ram)
}

nic_stream_prepare() {
  NIC_STREAM_BYTES=262144
  NIC_STREAM="$(mktemp -t leanos-nicstream-XXXXXX)"
  python3 -c "
import sys
n = int(sys.argv[2])
sys.stdout = open(sys.argv[1], 'wb')
sys.stdout.write(bytes(((i * 7 + (i >> 9)) & 0xFF) for i in range(n)))
" "$NIC_STREAM" "$NIC_STREAM_BYTES"
  NETDEV="user,id=net0,guestfwd=tcp:10.0.2.100:7777-cmd:cat $NIC_STREAM"
}
