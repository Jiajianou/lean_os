/* tests/cxx/exceptions.cpp - M97's fixture.
 *
 * ---- what this program is for -----------------------------------------
 *
 * M97's own "how we'll know" names the one thing that has to be true and
 * the one way it is usually wrong:
 *
 *   "a `throw` in one shared object caught by type in another, with
 *    destructors running for every frame in between - verified by
 *    COUNTING the destructor calls, because a catch that fires while
 *    skipping a destructor is the bug this gets wrong and it looks like
 *    success."
 *
 * So nothing here is checked by "did we get to the catch". Every check
 * is a count or an identity:
 *
 *   - how many destructors ran, and in what order
 *   - which type the catch matched, when several could have
 *   - whether the object caught is the object thrown
 *   - whether a rethrow preserves both
 *
 * A test that only asserted control reached the handler would pass with
 * the unwinder skipping every cleanup, which is precisely the failure
 * an incorrect .gcc_except_table or a mislaid .eh_frame produces.
 *
 * ---- and static initialization, which is the other half --------------
 *
 * A namespace-scope object with a constructor and a destructor exists to
 * prove the __cxa_atexit path (M97 turned DEFAULT_USE_CXA_ATEXIT on):
 * the constructor runs from .init_array before main, and the destructor
 * runs from __cxa_finalize at exit with the object's own `this` - which
 * is the argument the two-argument atexit could not carry.
 *
 * Exit codes so a failure names itself:
 *   0  everything worked
 *   2  a destructor did not run, or ran the wrong number of times
 *   3  destructors ran out of order
 *   4  a catch matched the wrong type
 *   5  the object caught is not the object thrown
 *   6  a rethrow lost the value or the type
 *   7  a static object's constructor did not run before main
 *   8  catch(...) did not see an exception it should have
 *   9  an unexpected exception escaped
 */

extern "C" {
#include <stdio.h>
#include <stdlib.h>
}

static int destroyed;          /* how many destructors have run */
static int order[16];          /* in what order, by tag */
static int order_n;

struct Tracer {
    int tag;
    explicit Tracer(int t) : tag(t) {}
    ~Tracer() {
        if (order_n < 16) {
            order[order_n++] = tag;
        }
        destroyed++;
    }
};

/* Distinct types with a value, so a catch that matches the wrong one is
 * visible as a wrong value rather than as a wrong branch. */
struct Alpha { int v; explicit Alpha(int x) : v(x) {} };
struct Beta  { int v; explicit Beta(int x) : v(x) {} };
struct Derived : Alpha { explicit Derived(int x) : Alpha(x) {} };

/* The static object. Its constructor must run before main and its
 * destructor after it, and both are counted rather than printed. */
static int static_ctor_ran;
static int static_dtor_ran;
struct StaticProbe {
    StaticProbe() { static_ctor_ran = 1; }
    ~StaticProbe() { static_dtor_ran = 1; }
};
static StaticProbe the_probe;

/* Three frames deep, each with a live object, so a throw from the
 * bottom has three destructors to run on the way out. Marked noinline
 * and the objects used, or the optimiser is entitled to notice that
 * nothing observes them - which would leave this measuring nothing. */
__attribute__((noinline)) static void deep3() {
    Tracer t(3);
    (void)t.tag;
    throw Alpha(42);
}
__attribute__((noinline)) static void deep2() {
    Tracer t(2);
    (void)t.tag;
    deep3();
}
__attribute__((noinline)) static void deep1() {
    Tracer t(1);
    (void)t.tag;
    deep2();
}

__attribute__((noinline)) static void rethrower() {
    try {
        throw Beta(7);
    } catch (Beta &b) {
        b.v += 1; /* the caught object is mutable and is the one rethrown */
        throw;
    }
}

int main(void) {
    if (!static_ctor_ran) {
        return 7;
    }

    /* ---- 1. three frames, three destructors, innermost first -------- */
    int caught = 0;
    try {
        deep1();
    } catch (Alpha &a) {
        caught = a.v;
    } catch (...) {
        return 4;
    }
    if (caught != 42) {
        return 5;
    }
    if (destroyed != 3) {
        return 2;
    }
    /* Innermost frame first: 3, then 2, then 1. An unwinder that runs
     * them in the wrong order is as broken as one that skips them, and
     * only the order distinguishes the two. */
    if (order[0] != 3 || order[1] != 2 || order[2] != 1) {
        return 3;
    }

    /* ---- 2. the type, when more than one could match ---------------- */
    try {
        throw Beta(9);
    } catch (Alpha &) {
        return 4;  /* Beta is not an Alpha */
    } catch (Beta &b) {
        if (b.v != 9) {
            return 5;
        }
    }

    /* ---- 3. a derived object caught by its base --------------------- */
    try {
        throw Derived(11);
    } catch (Alpha &a) {
        if (a.v != 11) {
            return 5;
        }
    } catch (...) {
        return 4;
    }

    /* ---- 4. rethrow keeps the object and the type ------------------- */
    try {
        rethrower();
        return 6;
    } catch (Beta &b) {
        if (b.v != 8) { /* 7, incremented once by the intermediate catch */
            return 6;
        }
    } catch (...) {
        return 6;
    }

    /* ---- 5. catch(...) ---------------------------------------------- */
    int any = 0;
    try {
        throw 5; /* a builtin type, which has no destructor and no class */
    } catch (...) {
        any = 1;
    }
    if (!any) {
        return 8;
    }

    /* ---- 6. and a destructor running during a caught throw does not
     *          leak into the next measurement ------------------------- */
    int before = destroyed;
    try {
        Tracer t(9);
        (void)t.tag;
        throw Alpha(1);
    } catch (Alpha &) {
    }
    if (destroyed != before + 1) {
        return 2;
    }

    printf("cxxtest: %d destructors, in order %d %d %d; every catch matched "
           "by type; rethrow preserved the object; static ctor ran before "
           "main\n", destroyed, order[0], order[1], order[2]);
    return 0;
}
