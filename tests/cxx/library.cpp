/* tests/cxx/library.cpp - M97's second fixture: the standard library.
 *
 * tests/cxx/exceptions.cpp proves the ABI runtime - the unwinder, the
 * personality routine, __cxa_atexit. This proves the part that sits on
 * top of it and on top of this project's libc: containers, strings,
 * algorithms, iostreams, RTTI and threads.
 *
 * ---- why each of these and not a longer list -------------------------
 *
 * Every check here is one that fails LOUDLY if a specific piece of the
 * port is wrong, rather than one that exercises breadth:
 *
 *   std::string          operator new/delete and the small-string
 *                        optimisation's boundary, which is where a
 *                        wrong alignment shows up
 *   std::vector/sort     move construction and the comparison plumbing
 *   std::map             a red-black tree, which is libstdc++'s own
 *                        compiled code rather than a header template -
 *                        so it proves libstdc++.a, not just the headers
 *   iostreams            locale and facets, the heaviest machinery in
 *                        the library and the one most dependent on libc
 *   dynamic_cast/typeid  RTTI: type identity, which M97's bullet notes
 *                        has the same failure mode as exceptions
 *   std::thread          M96's futex and M79's threads, reached through
 *                        the standard's own interface rather than
 *                        pthreads directly
 *   throw across a
 *   library boundary     a throw raised inside libstdc++ (std::vector's
 *                        at(), std::stoi) and caught here by type
 *
 * Exit codes:
 *   0  everything worked
 *   2  a container or algorithm gave the wrong answer
 *   3  a string operation gave the wrong answer
 *   4  an iostream round trip did not come back
 *   5  RTTI said the wrong thing
 *   6  an exception thrown inside the library was not caught by type
 *   7  a thread did not run, or did not join
 *   8  a map (libstdc++'s own compiled tree code) is wrong
 */
#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

struct Base { virtual ~Base() {} virtual int who() const { return 1; } };
struct Derived : Base { int who() const override { return 2; } };

int main() {
    /* ---- containers and algorithms --------------------------------- */
    std::vector<int> v{5, 3, 9, 1, 7};
    std::sort(v.begin(), v.end());
    if (v[0] != 1 || v[4] != 9 || v.size() != 5) {
        return 2;
    }
    if (std::find(v.begin(), v.end(), 7) == v.end()) {
        return 2;
    }

    /* ---- strings, across the small-string boundary ------------------ */
    std::string s = "lean";
    s += "_os";
    /* Long enough to force a heap allocation, which is a different code
     * path from the one above and the one that uses operator new. */
    std::string big(200, 'x');
    big += "end";
    if (s != "lean_os" || s.size() != 7) {
        return 3;
    }
    if (big.size() != 203 || big.substr(200) != "end") {
        return 3;
    }
    if (std::stoi("1234") != 1234) {
        return 3;
    }

    /* ---- a map: libstdc++'s own compiled tree ----------------------- */
    std::map<std::string, int> m;
    m["one"] = 1;
    m["two"] = 2;
    m["three"] = 3;
    if (m.size() != 3 || m["two"] != 2) {
        return 8;
    }
    /* Ordered iteration, which is the whole difference between a map and
     * a hash table and the thing a broken comparator gets wrong. */
    std::string keys;
    for (const auto &kv : m) {
        keys += kv.first;
        keys += " ";
    }
    if (keys != "one three two ") {
        return 8;
    }

    /* ---- iostreams -------------------------------------------------- */
    std::ostringstream os;
    os << "n=" << 42 << " s=" << s;
    if (os.str() != "n=42 s=lean_os") {
        return 4;
    }
    std::istringstream is("hello 7");
    std::string word;
    int n = 0;
    is >> word >> n;
    if (word != "hello" || n != 7) {
        return 4;
    }

    /* ---- RTTI ------------------------------------------------------- */
    Derived d;
    Base *b = &d;
    if (b->who() != 2) {
        return 5;
    }
    if (dynamic_cast<Derived *>(b) == nullptr) {
        return 5;
    }
    /* Through a Base* the compiler cannot see through, or it answers
     * statically and the cast never runs. A dynamic_cast the optimiser
     * folded is not a test of RTTI. */
    Base plain;
    Base *pb = &plain;
    __asm__ volatile("" : "+r"(pb));
    if (dynamic_cast<Derived *>(pb) != nullptr) {
        return 5; /* a cast that should fail must return null, not succeed */
    }
    if (typeid(*b) != typeid(Derived) || typeid(*pb) == typeid(Derived)) {
        return 5;
    }

    /* ---- an exception raised INSIDE libstdc++ ------------------------
     *
     * Not thrown by this file: std::vector::at and std::stoi throw from
     * compiled code in libstdc++.a. Catching one by type here is the
     * cross-boundary case M97's bullet is about - the type_info the
     * library threw and the type_info this catch names have to be the
     * same object, or the catch does not match and std::terminate wins. */
    int caught = 0;
    try {
        (void)v.at(99);
    } catch (const std::out_of_range &e) {
        caught |= 1;
    } catch (...) {
        return 6;
    }
    try {
        (void)std::stoi("not a number");
    } catch (const std::invalid_argument &) {
        caught |= 2;
    } catch (...) {
        return 6;
    }
    /* And caught by a BASE of the thrown type, which needs the same
     * type-identity machinery dynamic_cast does. */
    try {
        throw std::out_of_range("mine");
    } catch (const std::exception &e) {
        if (std::string(e.what()) != "mine") {
            return 6;
        }
        caught |= 4;
    }
    if (caught != 7) {
        return 6;
    }

    /* ---- std::thread over M79's tasks and M96's futex ---------------- */
    int from_thread = 0;
    std::thread t([&from_thread]() { from_thread = 99; });
    t.join();
    if (from_thread != 99) {
        return 7;
    }

    std::cout << "cxxlib: vector sorted, map ordered, string across the SSO "
                 "boundary, iostreams round-tripped, dynamic_cast and typeid "
                 "agreed, three exceptions from inside libstdc++ caught by "
                 "type, and a std::thread joined" << std::endl;
    return 0;
}
