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

/* M56: the interrupt-safe pair, and the one every lock this CPU's own
 * interrupt handlers can reach has to use.
 *
 * `sched_lock` has always been taken this way, with a comment explaining
 * that a plain `cli` there "isn't optional hardening the way it might
 * look, it's what keeps this CPU's own interrupt handlers out of a
 * critical section it's already inside". That reasoning was never
 * specific to the scheduler, and M54 made it bite somewhere else: a task
 * killed by a SIGKILL delivered *at a timer tick* now tears down its own
 * address space on the way out, so a tick landing while that task was
 * inside vmm_map_page_in - holding vmm_lock, with interrupts on - re-
 * entered vmm_destroy_address_space and spun forever on a lock its own
 * interrupted stack was holding. One CPU, interrupts already off inside
 * the IRQ handler: the whole machine.
 *
 * It presented as an intermittent boot hang, about one boot in ten,
 * always somewhere after a self-test that kills a process. The lesson is
 * the general one: a lock reachable from an interrupt handler must be
 * taken with interrupts off, and "reachable from an interrupt handler"
 * now includes anything on the task-exit path, because a fatal signal is
 * delivered from a timer tick.
 *
 * The saved flags are an ordinary local, so they travel with whichever
 * stack took the lock. */
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
