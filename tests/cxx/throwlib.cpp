#include "throwlib.h"

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

}

void lib_throw(int *counter, int which) {
    Marker m(counter, 1);
    (void)m.tag;
    inner(counter, which);
}

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
