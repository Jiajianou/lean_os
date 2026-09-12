/* tests/clang/cxx.cpp - M121's C++ fixture.
 *
 * Compiled by x86_64-lean_os-clang++ with **no flag supplied by hand**,
 * against the libc++ tools/build-libcxx.sh installs. See
 * tools/clang-test.sh for the command line.
 *
 * ---- what this grades that tests/cxx/exceptions.cpp does not ----------
 *
 * M97's fixture is the same idea for g++ and libstdc++, and its opening
 * argument applies here unchanged: nothing is checked by "did control
 * reach the handler", because a catch that fires while skipping a
 * destructor is the bug this gets wrong and it looks like success. So
 * every check below is a count or an identity.
 *
 * What is new here is the seam. This program's exceptions are thrown and
 * caught by **libc++abi**, and unwound by **libgcc_eh** - LLVM's C++
 * runtime over GCC's unwinder, which is a combination this project chose
 * on purpose (see tools/build-libcxx.sh) and which nothing else in the
 * tree exercises. A mismatch there does not fail to link: it aborts in
 * std::terminate with no message, from a program whose every other test
 * passes.
 *
 * The last section is conditional, and the condition is the news:
 * <sstream> exists only if libc++ was built with localization, which
 * needs more of this libc's <locale.h> than M89 built. The fixture
 * reports which libc++ it is running against rather than assuming.
 */
#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

#ifndef _LIBCPP_HAS_NO_LOCALIZATION
#  include <sstream>
#endif

static int fails;

static void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("clangcxxtest: FAIL %s\n", what);
        fails++;
    }
}

/* ---- destructor counting, which is the whole point ------------------ */
static int destroyed;
static int destroy_order[8];
static int destroy_n;

struct Tracer {
    int id;
    explicit Tracer(int i) : id(i) {}
    ~Tracer() {
        destroyed++;
        if (destroy_n < 8) {
            destroy_order[destroy_n++] = id;
        }
    }
};

struct Derived : std::runtime_error {
    int payload;
    Derived(const char *m, int p) : std::runtime_error(m), payload(p) {}
};

__attribute__((noinline))
static void inner() {
    Tracer a(1);
    Tracer b(2);
    throw Derived("thrown from inner", 99);
}

__attribute__((noinline))
static void middle() {
    Tracer c(3);
    inner();
}

/* Static initialization, and the __cxa_atexit path out of it. */
struct AtStartup {
    AtStartup() { constructed = true; }
    ~AtStartup() {
        /* Nothing inside the program can observe this, so it writes -
         * a destructor that did not run is a line that is not there. */
        std::printf("clangcxxtest: a static destructor ran\n");
    }
    static bool constructed;
};
bool AtStartup::constructed = false;
static AtStartup at_startup;

int main() {
    check(AtStartup::constructed, "a namespace-scope constructor before main");

    /* ---- the unwind, counted ------------------------------------- */
    bool caught_by_type = false;
    int payload = 0;
    try {
        middle();
    } catch (const Derived &e) {
        caught_by_type = true;
        payload = e.payload;
        check(std::string(e.what()) == "thrown from inner",
              "the object caught is the object thrown");
    } catch (...) {
        check(false, "the catch matched the wrong type");
    }
    check(caught_by_type, "a throw caught by its derived type");
    check(payload == 99, "the payload survived the unwind");
    check(destroyed == 3, "three destructors ran during the unwind");
    /* Innermost frame first: b(2), a(1) from inner, then c(3) from
     * middle. A cleanup run in the wrong order is a real bug that a
     * count alone would miss. */
    check(destroy_n == 3 && destroy_order[0] == 2 && destroy_order[1] == 1 &&
          destroy_order[2] == 3,
          "the destructors ran innermost-frame-first");

    /* A base-class catch, which needs RTTI comparison rather than an
     * address match. */
    bool as_base = false;
    try {
        throw Derived("again", 1);
    } catch (const std::runtime_error &) {
        as_base = true;
    }
    check(as_base, "a derived exception caught as its base");

    /* And a rethrow, which has to preserve both. */
    bool rethrown = false;
    try {
        try {
            throw Derived("rethrow me", 7);
        } catch (...) {
            throw;
        }
    } catch (const Derived &e) {
        rethrown = e.payload == 7;
    }
    check(rethrown, "a rethrow preserved the type and the payload");

    check(typeid(Derived).name() != nullptr, "RTTI has a name for a type");

    /* ---- the containers, which is most of what a program uses ----- */
    std::vector<int> v{5, 3, 9, 1, 7};
    std::sort(v.begin(), v.end());
    check(v.front() == 1 && v.back() == 9 && v.size() == 5,
          "std::vector and std::sort");

    std::string s = "lean";
    s += "_os";
    s.append(1, '!');
    check(s == "lean_os!" && s.size() == 8, "std::string, past its small buffer");
    std::string big(500, 'x');
    check(big.size() == 500 && big[499] == 'x',
          "a std::string that had to allocate");

    std::map<std::string, int> m;
    m["one"] = 1;
    m["two"] = 2;
    check(m.size() == 2 && m["two"] == 2 && m.count("three") == 0,
          "std::map with std::string keys");

    auto p = std::make_unique<Tracer>(42);
    check(p && p->id == 42, "std::unique_ptr");
    p.reset();

    std::function<int(int)> f = [](int x) { return x * 3; };
    check(f(14) == 42, "std::function over a lambda");

    check(std::to_string(1234) == "1234", "std::to_string");

    /* ---- and the part that says which libc++ this is -------------- */
#ifdef _LIBCPP_HAS_NO_LOCALIZATION
    std::printf("clangcxxtest: libc++ without localization "
                "(no <sstream>, no <iostream>)\n");
#else
    std::ostringstream os;
    os << "n=" << 42 << " d=" << 1.5;
    check(os.str() == "n=42 d=1.5", "std::ostringstream formatting");
    std::istringstream is("7 8");
    int a = 0, b = 0;
    is >> a >> b;
    check(a == 7 && b == 8, "std::istringstream parsing");
    std::printf("clangcxxtest: libc++ with localization and iostreams\n");
#endif

    if (fails) {
        std::printf("clangcxxtest: %d checks failed\n", fails);
        return 1;
    }
    std::printf("clangcxxtest: every check passed\n");
    return 0;
}
