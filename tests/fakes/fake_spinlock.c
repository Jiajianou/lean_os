#include "library/spinlock.h"

#include <stddef.h>

void panic(const char *msg);

#define MAX_HELD 32
static spinlock_t *held[MAX_HELD];
static int held_count;

#define MAX_PAIRS 128
static struct {
    const spinlock_t *outer;
    const spinlock_t *inner;
} order[MAX_PAIRS];
static int order_count;

static void note_order(const spinlock_t *outer, const spinlock_t *inner) {
    for (int i = 0; i < order_count; i++) {
        if (order[i].outer == inner && order[i].inner == outer) {
            panic("spinlock: lock order inversion - two paths take the same "
                  "two locks in opposite orders, which is a deadlock on the "
                  "machine (see fake_spinlock.c)");
        }
        if (order[i].outer == outer && order[i].inner == inner) {
            return;
        }
    }
    if (order_count < MAX_PAIRS) {
        order[order_count].outer = outer;
        order[order_count].inner = inner;
        order_count++;
    }
}

int fake_spinlock_order_pairs(void);
int fake_spinlock_order_pairs(void) { return order_count; }

void fake_spinlock_release_all(void);

void test_spin_lock(spinlock_t *lock, const char *where) {
    (void)where;
    if (lock->locked) {
        panic("spinlock: recursive acquire - this would deadlock on the machine");
    }
    for (int i = 0; i < held_count; i++) {
        note_order(held[i], lock);
    }
    lock->locked = 1;
    if (held_count < MAX_HELD) {
        held[held_count++] = lock;
    }
}

void test_spin_unlock(spinlock_t *lock, const char *where) {
    (void)where;
    if (!lock->locked) {
        panic("spinlock: release of a lock that was not held");
    }
    lock->locked = 0;
    for (int i = 0; i < held_count; i++) {
        if (held[i] == lock) {
            held[i] = held[--held_count];
            break;
        }
    }
}

void fake_spinlock_release_all(void) {
    for (int i = 0; i < held_count; i++) {
        held[i]->locked = 0;
    }
    held_count = 0;
}
