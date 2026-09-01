/* tests/fakes/lib/spinlock.h - Q2
 *
 * The one header this test build shadows, and the only one it should.
 *
 * kernel/lib/spinlock.h is real x86_64: `pause`, and a `pushfq/popfq`
 * pair around cli for the interrupt-safe variant. Neither assembles on
 * the host this suite runs on, and neither has anything to say on a
 * single-threaded test process.
 *
 * The shadow is a header rather than a fake .c file because the real one
 * is entirely `static inline` - there is no object to substitute. It is
 * in tests/fakes/lib/ so that -Itests/fakes ahead of -Ikernel resolves
 * `#include "lib/spinlock.h"` here without any change to the kernel
 * source, which is the property that matters: the code under test is the
 * code that ships, byte for byte.
 *
 * What this deliberately does NOT do is pretend to be a lock. These tests
 * are single-threaded, so a counter that catches the one bug a no-op
 * would hide - a lock taken twice, or released without being held - is
 * worth more than a real mutex. Concurrency is Q9's job and it needs the
 * real machine, not this. */
#pragma once

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

/* Recursion and unbalanced release are the two failures a single-threaded
 * harness can still see, and both are real deadlock bugs on the machine.
 * Declared in tests/fakes/fake_spinlock.c. */
void test_spin_lock(spinlock_t *lock, const char *where);
void test_spin_unlock(spinlock_t *lock, const char *where);

static inline void spin_lock(spinlock_t *lock) { test_spin_lock(lock, "spin_lock"); }
static inline void spin_unlock(spinlock_t *lock) { test_spin_unlock(lock, "spin_unlock"); }

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    test_spin_lock(lock, "spin_lock_irqsave");
    return 0;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    test_spin_unlock(lock, "spin_unlock_irqrestore");
}
