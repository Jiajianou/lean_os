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
