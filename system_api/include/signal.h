/* system_api/include/signal.h
 *
 * M14's "basic set" of signals - just enough for SYS_kill to be
 * meaningful. Numbered to match POSIX for familiarity, though this
 * project only recognizes these two and only supports the default
 * (terminate) action - no handler registration/blocking/masking.
 */
#pragma once

#define SIGTERM 15
#define SIGKILL 9
