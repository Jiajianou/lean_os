/* user_space/libc/include/setjmp.h - M80 groundwork
 *
 * See user_space/lib/setjmp.asm for what is saved and, more usefully,
 * what deliberately is not.
 */
#pragma once

/* rbx, rbp, r12-r15, rsp, rip. Eight words, and the layout is shared
 * with the assembly by nothing but this comment and that file's own -
 * the one place in this project where two halves of a contract are not
 * held in a struct, because nasm cannot read a C header. */
typedef unsigned long jmp_buf[8];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int value) __attribute__((noreturn));

/* No sigsetjmp/siglongjmp. M76's signal mask is a plain word a program
 * can save and restore with sigprocmask around a setjmp, and a second
 * pair of functions that differ only by doing that would be two ways to
 * say one thing. */
