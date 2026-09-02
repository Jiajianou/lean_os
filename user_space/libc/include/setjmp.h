/* user_space/libc/include/setjmp.h - M80 groundwork
 *
 * See user_space/lib/setjmp.asm for what is saved and, more usefully,
 * what deliberately is not.
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

/* rbx, rbp, r12-r15, rsp, rip. Eight words, and the layout is shared
 * with the assembly by nothing but this comment and that file's own -
 * the one place in this project where two halves of a contract are not
 * held in a struct, because nasm cannot read a C header. */
typedef unsigned long jmp_buf[8];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int value) __attribute__((noreturn));

/* ---- M89: sigsetjmp/siglongjmp, and why the paragraph below was wrong
 *
 * What stood here said: "No sigsetjmp/siglongjmp. M76's signal mask is a
 * plain word a program can save and restore with sigprocmask around a
 * setjmp, and a second pair of functions that differ only by doing that
 * would be two ways to say one thing."
 *
 * The first sentence is true and the conclusion does not follow, which
 * toybox demonstrates by putting a `sigjmp_buf` in a struct. A program
 * cannot wrap setjmp in its own save/restore, because the restore has to
 * happen *on the longjmp side* - after the stack has been unwound and
 * before the code that receives the non-zero return runs - and there is
 * no place in the caller to put it. The pair is not a convenience over
 * sigprocmask; it is the only way to express "unwind and put the mask
 * back" as one operation.
 *
 * sigsetjmp is a macro rather than a function for the reason setjmp
 * itself is special: a function that called setjmp on the caller's
 * behalf would return, and the buffer would then name a dead frame. So
 * the mask is saved by an ordinary call and the jump is set up in the
 * caller's own frame, in that order.
 */
#include <signal.h>

typedef struct {
    jmp_buf jb;
    sigset_t mask;
    int savemask; /* 0 means siglongjmp leaves the mask alone */
} __sigjmp_state;

typedef __sigjmp_state sigjmp_buf[1];

/* Saves the current mask into `env` if `savemask` is non-zero. Called by
 * the macro below before setjmp, never directly. */
void __sigjmp_save(__sigjmp_state *env, int savemask);

#define sigsetjmp(env, savemask) \
    (__sigjmp_save((env), (savemask)), setjmp((env)->jb))

void siglongjmp(sigjmp_buf env, int value) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
