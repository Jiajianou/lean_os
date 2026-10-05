#include "library/spinlock.h"

#include <sched.h>
#include <stddef.h>

void panic(const char *message);

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

/* M225: a lock held by "another processor". Single-threaded, a held lock
   can only mean a recursive acquire - unless a test says otherwise: it holds
   the lock as that other processor would, installs this, and the moment
   something waits for the lock the hook runs as the other processor
   finishing its work and letting go. One shot, so a second wait is the
   recursion it always was. */
static void (*contention_hook)(void);

void fake_spinlock_on_contention(void (*hook)(void));
void fake_spinlock_on_contention(void (*hook)(void)) {
    contention_hook = hook;
}

/* M225: the moment a lock is let go of is the moment another processor can
   act on what it guarded - and the windows the panel found were all of that
   shape: a table read under the scheduler lock and acted on after it was
   dropped. This runs after EVERY release while it is installed (not during
   its own run), and the hook decides from what it can see whether this is
   the release it was waiting for; it uninstalls itself, or the end of the
   test does. */
static void (*release_hook)(void);
static int release_hook_running;

void fake_spinlock_on_release(void (*hook)(void));
void fake_spinlock_on_release(void (*hook)(void)) {
    release_hook = hook;
}

/* M225: one-shot hooks elsewhere in the fakes register how to clear
   themselves here, so a test that fails (a REQUIRE longjmps out) before it
   uninstalls one does not leave it armed for the next test. The runner
   calls fake_spinlock_release_all after every test. */
#define MAX_TEST_END 16
static void (*test_end[MAX_TEST_END])(void);
static int test_end_count;

void fake_spinlock_at_test_end(void (*clear)(void));
void fake_spinlock_at_test_end(void (*clear)(void)) {
    for (int i = 0; i < test_end_count; i++) {
        if (test_end[i] == clear) {
            return;
        }
    }
    if (test_end_count < MAX_TEST_END) {
        test_end[test_end_count++] = clear;
    }
}

static int threaded;

void fake_spinlock_threaded(int on);
void fake_spinlock_threaded(int on) {
    __atomic_store_n(&threaded, on, __ATOMIC_RELEASE);
}

void test_spin_lock(spinlock_t *lock, const char *where) {
    (void)where;
    if (__atomic_load_n(&threaded, __ATOMIC_ACQUIRE)) {
        while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
            while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED)) {
                sched_yield();
            }
        }
        return;
    }
    if (lock->locked && contention_hook) {
        void (*hook)(void) = contention_hook;
        contention_hook = 0;
        hook();
    }
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
    if (__atomic_load_n(&threaded, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
        return;
    }
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
    if (release_hook && !release_hook_running) {
        release_hook_running = 1;
        release_hook();
        release_hook_running = 0;
    }
}

void fake_spinlock_release_all(void) {
    threaded = 0;
    contention_hook = 0;
    release_hook = 0;
    release_hook_running = 0;
    for (int i = 0; i < test_end_count; i++) {
        test_end[i]();
    }
    for (int i = 0; i < held_count; i++) {
        held[i]->locked = 0;
    }
    held_count = 0;
}
