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

struct AtStartup {
    AtStartup() { constructed = true; }
    ~AtStartup() {
        std::printf("clangcxxtest: a static destructor ran\n");
    }
    static bool constructed;
};
bool AtStartup::constructed = false;
static AtStartup at_startup;

int main() {
    check(AtStartup::constructed, "a namespace-scope constructor before main");

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
    check(destroy_n == 3 && destroy_order[0] == 2 && destroy_order[1] == 1 &&
          destroy_order[2] == 3,
          "the destructors ran innermost-frame-first");

    bool as_base = false;
    try {
        throw Derived("again", 1);
    } catch (const std::runtime_error &) {
        as_base = true;
    }
    check(as_base, "a derived exception caught as its base");

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
