extern "C" {
#include <stdio.h>
#include <stdlib.h>
}

static int destroyed;
static int order[16];
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

struct Alpha { int v; explicit Alpha(int x) : v(x) {} };
struct Beta  { int v; explicit Beta(int x) : v(x) {} };
struct Derived : Alpha { explicit Derived(int x) : Alpha(x) {} };

static int static_ctor_ran;
static int static_dtor_ran;
struct StaticProbe {
    StaticProbe() { static_ctor_ran = 1; }
    ~StaticProbe() { static_dtor_ran = 1; }
};
static StaticProbe the_probe;

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
        b.v += 1;
        throw;
    }
}

int main(void) {
    if (!static_ctor_ran) {
        return 7;
    }

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
    if (order[0] != 3 || order[1] != 2 || order[2] != 1) {
        return 3;
    }

    try {
        throw Beta(9);
    } catch (Alpha &) {
        return 4;
    } catch (Beta &b) {
        if (b.v != 9) {
            return 5;
        }
    }

    try {
        throw Derived(11);
    } catch (Alpha &a) {
        if (a.v != 11) {
            return 5;
        }
    } catch (...) {
        return 4;
    }

    try {
        rethrower();
        return 6;
    } catch (Beta &b) {
        if (b.v != 8) {
            return 6;
        }
    } catch (...) {
        return 6;
    }

    int any = 0;
    try {
        throw 5;
    } catch (...) {
        any = 1;
    }
    if (!any) {
        return 8;
    }

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
