/* kernel/arch/x86_64/io.h
 *
 * Port I/O helpers shared by anything that talks to legacy PC hardware
 * (PIC, PIT, serial, PS/2, ATA PIO which transfers its 512-byte sectors
 * 16 bits at a time through one data port, and now (stretch goal:
 * networking) PCI config space and the RTL8139 NIC, both of which are
 * 32-bit-register hardware).
 */
#pragma once

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ---- block port I/O -------------------------------------------------
 *
 * `rep insw`/`rep outsw` move `words` 16-bit units between a port and
 * memory in one instruction. This is the idiomatic form and it replaced
 * a hand-written `for (i...) dst[i] = inw(port);` loop in ata.c, so it
 * is three lines shorter at each of the two call sites and one
 * instruction instead of 256 per 512-byte sector.
 *
 * What it is NOT is a measured speedup, and that is worth writing down
 * so nobody re-derives it hopefully. The reasoning was that each
 * discrete `inw` is a VM exit into QEMU's device emulation and a `rep`
 * string form is one exit for the whole run - which is true, and on a
 * boot that pushes ~1.5 MB through this port seeding 47 programs onto a
 * fresh disk it sounded like it would show. It does not: that seed takes
 * 0.2 s either way, and boot to `[init] PID 1 spawned` measured 148.7 s
 * before and 148.3 s after, which is noise. Disk I/O is simply not where
 * this boot's time goes: the network self-tests are, at 34 s of 148 s,
 * and they are round trips rather than work.
 * Kept because it is the smaller and more conventional way to write the
 * same transfer, not because it made anything faster here; it may on
 * real hardware, where the per-access cost is a real bus cycle rather
 * than an emulator's.
 *
 * `rep` uses ES:RDI / DS:RSI and depends on the direction flag being
 * clear, which is the ABI's guaranteed state at every function boundary
 * and which nothing in this kernel ever sets - the only `std` in the
 * tree would have to be written by hand, and there is none.
 */
static inline void insw(uint16_t port, void *buf, uint32_t words) {
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(words) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *buf, uint32_t words) {
    __asm__ volatile("rep outsw" : "+S"(buf), "+c"(words) : "d"(port) : "memory");
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Write to the unused POST-code port 0x80 as a cheap ~1us delay: some
 * legacy PIC/controller programming sequences need a beat between writes
 * on real hardware. Harmless no-op-ish write, but a real bus cycle. */
static inline void io_wait(void) {
    outb(0x80, 0);
}

/* Saves RFLAGS and disables interrupts on *this* CPU, returning the saved
 * value so irq_restore can put it back exactly as it was - not
 * unconditionally re-enable, since the caller might already be running
 * inside an interrupt handler (IF already 0) where blindly turning
 * interrupts back on would be wrong (only the eventual `iretq` is
 * supposed to do that). Originally klog.c's own local helper (see its
 * header comment on why one character's console update needs this);
 * promoted here once kernel/sched/sched.c needed the exact same "keep
 * this CPU's own interrupt handlers out of a critical section" primitive
 * for sched_lock (SMP: an IPI landing on a CPU that already holds
 * sched_lock would otherwise reenter schedule() and deadlock on itself). */
static inline uint64_t irq_save_disable(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" ::"r"(flags) : "memory", "cc");
}

/* Q4: two primitives that were written inline in kernel/net/ip.c.
 *
 * Whether interrupts are on, and halting until the next one, are facts
 * about this architecture rather than about IPv4 - and io.h is where
 * every other one of them already lives (irq_save_disable above is the
 * same `pushfq` this reads). ip.c had its own copies because it needed
 * them and nothing here offered them.
 *
 * Moving them also makes the IPv4 parse path compile somewhere other than
 * x86_64, which is what lets tests/test_net.c feed it malformed packets
 * on the host. That is a side effect of putting the code where it goes,
 * not the reason for it - but it is the reason it happened now. */
static inline int cpu_interrupts_enabled(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
    return (flags & (1u << 9)) != 0;
}

/* Halt until the next interrupt. Only correct with interrupts enabled -
 * see cpu_interrupts_enabled, which every caller here checks first. */
static inline void cpu_halt(void) {
    __asm__ volatile("hlt");
}
