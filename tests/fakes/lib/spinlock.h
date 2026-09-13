#pragma once

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

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
