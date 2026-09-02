/* user_space/libc/include/sys/syscall.h - M89
 *
 * `syscall()`, and a warning that is the entire reason to read this
 * file.
 *
 * **This machine's syscall numbers are its own and they are not
 * Linux's.** SYS_read is 3 here (system_api/include/syscall.h) and 0 on
 * Linux; every other number differs too, because they were assigned in
 * the order this project needed them over eighty-nine milestones. So a
 * program that calls `syscall(SYS_something, ...)` with a number it got
 * from a Linux header is not making a slightly wrong call - it is
 * making a completely different one.
 *
 * That is why **no SYS_* constants are defined here**. A program that
 * needs one does not compile, which is where that failure belongs: at
 * build time, naming the call it wanted. Defining them would move the
 * failure to run time, where a program asks for `getdents64` and gets
 * whatever this kernel has at that number.
 *
 * The function is declared because toys.h includes this header
 * unconditionally, and it is a truthful failure: -1 with ENOSYS. A
 * program reaching for a raw syscall on this machine wants a Linux
 * syscall this machine does not have, and telling it so is more useful
 * than dispatching on a number that means something else.
 *
 * The numbers themselves are in system_api/include/syscall.h, and the
 * way to make a call here is the wrapper in
 * user_space/lib/syscall_wrappers.h - which is what every program in
 * user_space/bin uses and what libc is built on.
 */
#pragma once

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

long syscall(long number, ...);

#ifdef __cplusplus
}
#endif
