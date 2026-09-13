#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long jmp_buf[8];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int value) __attribute__((noreturn));

#include <signal.h>

typedef struct {
    jmp_buf jb;
    sigset_t mask;
    int savemask;
} __sigjmp_state;

typedef __sigjmp_state sigjmp_buf[1];

void __sigjmp_save(__sigjmp_state *env, int savemask);

#define sigsetjmp(env, savemask) \
    (__sigjmp_save((env), (savemask)), setjmp((env)->jb))

void siglongjmp(sigjmp_buf env, int value) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
