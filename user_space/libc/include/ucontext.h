#pragma once

#include <sys/ucontext.h>

#ifdef __cplusplus
extern "C" {
#endif

/* getcontext, setcontext, makecontext and swapcontext are not here. They are
   a coroutine mechanism rather than a signal one, and nothing on this machine
   has asked for them; what asked for this header was a signal handler
   reading the registers it interrupted. The condition for the other four is
   a program that calls one. */

#ifdef __cplusplus
}
#endif
