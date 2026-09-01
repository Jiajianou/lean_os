/* tests/fakes/arch/x86_64/io.h - Q4
 *
 * The second and last header this test build shadows.
 *
 * The real one is port I/O and flag manipulation - `outb`, `inb`, `rep
 * insw`, `pushfq`. There is no honest host version of any of it, and
 * there does not need to be: the code under test here is a packet parser,
 * and a packet parser that touches a port would be the bug.
 *
 * So every function is present and every one is inert, except the two
 * that have a meaningful answer on a host: interrupts are always "on"
 * (this process is not in an interrupt handler), and halting is a no-op
 * (there is nothing to wait for, and the callers all check a deadline
 * they will reach by other means).
 *
 * A port write reaching here would be silently discarded, which is the
 * one thing about this file worth being uneasy about. It is the right
 * trade at this tier - a parser has no business writing to a port - and
 * the boot self-tests are what cover the drivers that do. */
#pragma once

#include <stdint.h>

/* Q11: the byte ports route to a scriptable fake rather than to a
 * constant, so kernel/dev/fwcfg.c - which is nothing but two ports - can
 * be tested off the machine. Everything unscripted still reads 0xFF,
 * which is both what an unclaimed port does and what every test written
 * before this one assumed. */
uint8_t fake_port_inb(uint16_t port);
void fake_port_outw(uint16_t port, uint16_t value);

static inline void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline uint8_t inb(uint16_t port) { return fake_port_inb(port); }
static inline void outw(uint16_t port, uint16_t val) { fake_port_outw(port, val); }
static inline uint16_t inw(uint16_t port) { (void)port; return 0xFFFF; }
static inline void outl(uint16_t port, uint32_t val) { (void)port; (void)val; }
static inline uint32_t inl(uint16_t port) { (void)port; return 0xFFFFFFFFu; }
static inline void insw(uint16_t port, void *dst, uint32_t words) {
    (void)port; (void)dst; (void)words;
}
static inline void outsw(uint16_t port, const void *src, uint32_t words) {
    (void)port; (void)src; (void)words;
}

static inline uint64_t irq_save_disable(void) { return 0; }
static inline void irq_restore(uint64_t flags) { (void)flags; }

/* On a host process, interrupts are conceptually on: nothing here runs in
 * an interrupt handler, and the callers use this to decide whether they
 * may wait. Saying "on" keeps them on the path a booted kernel takes. */
static inline int cpu_interrupts_enabled(void) { return 1; }
static inline void cpu_halt(void) {}
