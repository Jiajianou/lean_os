/* tests/cxx/throwlib.cpp - M97: the shared object an exception comes out of.
 *
 * This is the far side of M97's headline test. It is compiled -fPIC and
 * linked as an ET_DYN with crtbeginS.o/crtendS.o, dlopen'd at runtime by
 * a program that never named it on its link line, and asked to throw.
 *
 * The types it throws are defined in a header both sides include, and
 * that is the whole difficulty: for the catch on the other side to match,
 * the `std::type_info` this object throws and the one the executable's
 * catch clause names have to be THE SAME OBJECT, not two objects that
 * happen to describe the same type. libstdc++ compares them by address
 * first, and the reason that is sound is symbol interposition - the
 * loader has to resolve this object's typeinfo symbol to the definition
 * already in the executable. An implementation that got that wrong would
 * fail this test and pass every single-object one.
 *
 * The destructors are counted through a pointer the caller supplies, so
 * the caller can assert that unwinding this object's frames ran them -
 * which is the other thing that only works if the .eh_frame of a MAPPED
 * object was registered with the unwinder.
 */
#include "throwlib.h"

/* Two frames inside this shared object, each with a live object, so a
 * throw from the inner one has two destructors to run before control
 * leaves this ET_DYN at all. noinline so they stay two frames. */
namespace {

struct Marker {
    int *counter;
    int tag;
    Marker(int *c, int t) : counter(c), tag(t) {}
    ~Marker() { (*counter)++; }
};

__attribute__((noinline)) void inner(int *counter, int which) {
    Marker m(counter, 2);
    (void)m.tag;
    if (which == 0) {
        throw LibError(7);
    }
    throw LibDerived(9);
}

} /* namespace */

void lib_throw(int *counter, int which) {
    Marker m(counter, 1);
    (void)m.tag;
    inner(counter, which);
}

/* A function that catches something the CALLER threw into it, so the
 * boundary is crossed in both directions. A throw that unwinds out of a
 * shared object and one that unwinds into a catch inside a shared object
 * are different code paths - the second needs this object's own
 * .gcc_except_table to be readable, which nothing else here proves. */
int lib_catches(void (*thrower)()) {
    try {
        thrower();
    } catch (const LibError &e) {
        return e.code;
    } catch (...) {
        return -2;
    }
    return -1;
}
