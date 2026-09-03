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

/* ---- Q9: the lock order, recorded rather than commented ---------------
 *
 * `heap.c` documents its lock order in a comment and `spinlock.h`
 * documents the interrupt rule in three paragraphs. **A comment cannot
 * fail.** Q9 asked for the order to be recorded and an inversion to be
 * an error; Q13 deferred it with a condition - "a second lock in the
 * same tier to invert against" - and that condition is now met, because
 * this build links kernel/sched/sched.c, kernel/mm/heap.c and
 * kernel/fs/leanfs.c, each with a lock of its own.
 *
 * How it works: every time B is taken while A is held, the pair (A, B)
 * is remembered. If (B, A) is ever seen afterwards, that is an
 * inversion - two call paths that take the same two locks in opposite
 * orders - and two threads following them on the real machine deadlock.
 * The bug it exists for (M56) presented as "about one boot in ten
 * hangs"; here it is a failed assertion naming both locks.
 *
 * Learned rather than declared, deliberately. A hand-written hierarchy
 * would have to be maintained, would be wrong the first time somebody
 * added a lock, and would be exactly the kind of comment this is meant
 * to replace. The cost is that an order is only checked once both
 * directions have been *executed* - so this finds an inversion the tests
 * reach, not every inversion that exists, and says so rather than
 * implying more.
 *
 * Learned pairs are NOT cleared between tests. The whole value is
 * cross-test: one test takes the heap lock under the scheduler lock and
 * another takes them the other way round, and neither is wrong on its
 * own. */
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
            return; /* already known */
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
        /* On the machine this is a hang, and a hang in a lock nobody can
         * see is the most expensive class of bug this kernel has had -
         * see spinlock.h's M56 note, which cost "about one boot in ten"
         * to find. Here it is one line. */
        panic("spinlock: recursive acquire - this would deadlock on the machine");
    }
    /* Every lock already held is an outer for this one. Recorded before
     * this lock joins the list, or it would record itself as its own
     * outer. */
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
