/* tests/cxx/throwlib.h - M97: the types that cross a shared-object
 * boundary, and the two entry points that carry them.
 *
 * Both sides include this. That is what makes the test meaningful: the
 * executable's catch clause and the shared object's throw expression
 * name the same C++ type, and whether they agree at RUNTIME is exactly
 * what the ABI's type identity rules are for. */
#pragma once

struct LibError {
    int code;
    explicit LibError(int c) : code(c) {}
    virtual ~LibError() {}
};

/* Derived, so the executable can also catch by base - which needs the
 * ABI's __do_catch walk to work across the boundary rather than just a
 * pointer comparison. */
struct LibDerived : LibError {
    explicit LibDerived(int c) : LibError(c) {}
};

extern "C" void lib_throw(int *counter, int which);
extern "C" int lib_catches(void (*thrower)());
