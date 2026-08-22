/* kernel/lib/spinlock.h
 *
 * A minimal test-and-set spinlock - the one new primitive SMP support
 * needs everywhere shared mutable kernel state (the task table, the pmm
 * bitmap, the heap free list, the vmm page tables, klog's shared display/
 * serial output) stops being safe to touch from more than one CPU at once.
 *
 * Before SMP, this project's single-core critical sections were just
 * `cli`/`sti` (see klog.c's own now-outdated comment on that) - correct
 * when there's only one CPU to ever race with, not once a second core can
 * be genuinely executing kernel code at the same instant. A real lock is
 * the smallest fix that's still correct on both cases: cheap/uncontended
 * with one CPU, actually mutually exclusive with more than one.
 *
 * `__atomic_exchange_n`/`__atomic_store_n` are GCC/Clang builtins that
 * lower directly to `lock xchg`/a plain aligned store on x86_64 for a
 * native-width integer - no libatomic call, no SSE, nothing this
 * freestanding kernel doesn't already have.
 */
#pragma once

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

static inline void spin_lock(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        while (lock->locked) {
            __asm__ volatile("pause");
        }
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}
