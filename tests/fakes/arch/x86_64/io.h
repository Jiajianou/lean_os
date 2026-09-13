#pragma once

#include <stdint.h>

uint8_t fake_port_inb(uint16_t port);
void fake_port_outw(uint16_t port, uint16_t value);

static inline void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline uint8_t inb(uint16_t port) { return fake_port_inb(port); }
static inline void outw(uint16_t port, uint16_t val) { fake_port_outw(port, val); }
static inline uint16_t inw(uint16_t port) { (void)port; return 0xFFFF; }
uint32_t fake_port_inl(uint16_t port);
void fake_port_outl(uint16_t port, uint32_t value);

static inline void outl(uint16_t port, uint32_t val) { fake_port_outl(port, val); }
static inline uint32_t inl(uint16_t port) { return fake_port_inl(port); }
static inline void insw(uint16_t port, void *dst, uint32_t words) {
    (void)port; (void)dst; (void)words;
}
static inline void outsw(uint16_t port, const void *src, uint32_t words) {
    (void)port; (void)src; (void)words;
}

static inline uint64_t irq_save_disable(void) { return 0; }
static inline void irq_restore(uint64_t flags) { (void)flags; }

static inline int cpu_interrupts_enabled(void) { return 1; }
static inline void cpu_halt(void) {}
