#pragma once

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

/* A core waiting here with interrupts off cannot take a TLB shootdown's IPI,
   and the core asking for the shootdown may be the one holding this lock - so
   the wait answers the request itself instead of deadlocking against it. */
extern volatile uint32_t smp_shootdown_pending;
void smp_tlb_service_pending(void);

static inline void spin_lock(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        while (lock->locked) {
            if (smp_shootdown_pending) {
                smp_tlb_service_pending();
            }
            __asm__ volatile("pause");
        }
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}
