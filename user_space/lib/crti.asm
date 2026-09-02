; user_space/lib/crti.asm - M94
;
; The first half of _init and _fini.
;
; ---- what these are, and why a C library needs them ---------------------
;
; A compiler emits code that has to run before main and after it -
; `__attribute__((constructor))` functions, C++ static initializers, and
; anything a runtime registers at load. The mechanism every ELF toolchain
; uses is that .init and .fini are *sections built out of fragments*: this
; file opens each one with a function prologue, GCC's own crtbegin.o and
; every object with a constructor drop their fragments into the middle,
; and crtn.asm closes each with an epilogue. Link order is what makes the
; pieces a function, which is why the sysroot's specs put crti.o before
; crtbegin.o and crtn.o last.
;
; ---- why this project needs them now ------------------------------------
;
; It has not until M94. Every program here is compiled by a hand-written
; link line that names its objects, and nothing in the tree has a
; constructor. A `./configure`-driven build names none of that: it invokes
; `x86_64-lean_os-gcc hello.c -o hello` and the driver supplies the
; startup files by name from the sysroot. A target whose sysroot has no
; crti.o is a target whose gcc cannot link, and one whose crti.o is a stub
; is a target where a constructor silently does not run - which is the
; failure M97's libstdc++ would find, far from here.
;
; In assembly because there is no way to write half a function in C.

bits 64

section .init
global _init
_init:
    push rbp
    mov rbp, rsp
    ; the fragments other objects contribute land here

section .fini
global _fini
_fini:
    push rbp
    mov rbp, rsp
    ; likewise
