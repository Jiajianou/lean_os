/* tests/fakes/fake_spinlock.c - Q2: see tests/fakes/lib/spinlock.h.
 *
 * Held locks are tracked in a small table so the runner can release them
 * all between tests. That is not tidiness - it is required. A caught
 * panic (tests/check.h's CHECK_PANIC) longjmps out of whatever kernel
 * function was running, and on the machine that function never returns
 * either, so any lock it held stays held forever. That is fine when the
 * next instruction is `cli; hlt` and catastrophic when the next thing is
 * another test. Without this, one CHECK_PANIC inside a locked region
 * poisons every test after it. */
#include "lib/spinlock.h"

#include <stddef.h>

void panic(const char *msg);

#define MAX_HELD 32
static spinlock_t *held[MAX_HELD];
static int held_count;

void fake_spinlock_release_all(void);

void test_spin_lock(spinlock_t *lock, const char *where) {
    (void)where;
    if (lock->locked) {
        /* On the machine this is a hang, and a hang in a lock nobody can
         * see is the most expensive class of bug this kernel has had -
         * see spinlock.h's M56 note, which cost "about one boot in ten"
         * to find. Here it is one line. */
        panic("spinlock: recursive acquire - this would deadlock on the machine");
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
