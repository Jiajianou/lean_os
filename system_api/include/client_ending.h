#pragma once

#include "signal.h"
#include "syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the compositor makes of a window whose program has stopped, given
   SYS_task_end_status's answer and whether the window had been asked to go.

   Until M209 every non-zero exit was "stopped unexpectedly". That made a
   program that quit from its own File menu, one the Task Manager ended, and
   one that faulted all the same event, so the README editor reported a crash
   every time it was closed. A crash is a FAULT - the processor refused an
   instruction - and nothing else is one. */
typedef enum {
    CLIENT_RUNNING = 0,
    CLIENT_KEEP_WINDOW,
    CLIENT_GONE,
    CLIENT_FAILED,
    CLIENT_CRASHED,
} client_ending_t;

static inline int client_ending_is_fault(int signal_number) {
    switch (signal_number) {
    case SIGSEGV:
    case SIGBUS:
    case SIGILL:
    case SIGFPE:
    case SIGABRT:
    case SIGSYS:
    case SIGTRAP:
        return 1;
    default:
        return 0;
    }
}

static inline client_ending_t client_ending(long end_status, int close_requested) {
    if (end_status == -OS_ERROR_AGAIN) {
        return CLIENT_RUNNING;
    }
    if (close_requested || end_status < 0) {
        return CLIENT_GONE;
    }
    int signal_number = (int)(end_status & 0x7F);
    if (signal_number != 0) {
        return client_ending_is_fault(signal_number) ? CLIENT_CRASHED : CLIENT_GONE;
    }
    int exit_status = (int)((end_status >> 8) & 0xFF);
    return exit_status == 0 ? CLIENT_KEEP_WINDOW : CLIENT_FAILED;
}

#ifdef __cplusplus
}
#endif
