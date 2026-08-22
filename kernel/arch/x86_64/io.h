/* kernel/arch/x86_64/io.h
 *
 * Port I/O helpers shared by anything that talks to legacy PC hardware
 * (PIC, PIT, serial, PS/2, and now (M12) ATA PIO, which transfers its
 * 512-byte sectors 16 bits at a time through one data port).
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

/* Write to the unused POST-code port 0x80 as a cheap ~1us delay: some
 * legacy PIC/controller programming sequences need a beat between writes
 * on real hardware. Harmless no-op-ish write, but a real bus cycle. */
static inline void io_wait(void) {
    outb(0x80, 0);
}
