#pragma once

struct LibError {
    int code;
    explicit LibError(int c) : code(c) {}
    virtual ~LibError() {}
};

struct LibDerived : LibError {
    explicit LibDerived(int c) : LibError(c) {}
};

extern "C" void lib_throw(int *counter, int which);
extern "C" int lib_catches(void (*thrower)());
