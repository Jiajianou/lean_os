    .hidden _DYNAMIC

    .text
    .globl _start
    .hidden _start
    .type _start, @function
_start:
    movq    %rdi, %rbx

    leaq    _start(%rip), %rdi
    leaq    __ld_start_linked(%rip), %rax
    subq    (%rax), %rdi

    leaq    _DYNAMIC(%rip), %rsi
    call    _dl_relocate_self

    movq    %rbx, %rdi
    call    _dl_entry

    movq    %rbx, %rdi
    jmpq    *%rax
    .size _start, .-_start

    .section .data.rel.ro
    .align 8
__ld_start_linked:
    .quad _start

    .text
    .globl _dl_setjmp
    .hidden _dl_setjmp
    .type _dl_setjmp, @function
_dl_setjmp:
    movq    %rbx,  0(%rdi)
    movq    %rbp,  8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rax
    movq    %rax, 48(%rdi)
    movq    (%rsp), %rax
    movq    %rax, 56(%rdi)
    xorl    %eax, %eax
    ret
    .size _dl_setjmp, .-_dl_setjmp

    .globl _dl_longjmp
    .hidden _dl_longjmp
    .type _dl_longjmp, @function
_dl_longjmp:
    movq     0(%rdi), %rbx
    movq     8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    movq    56(%rdi), %rdx
    movl    %esi, %eax
    testl   %eax, %eax
    jnz     1f
    movl    $1, %eax
1:
    jmp     *%rdx
    .size _dl_longjmp, .-_dl_longjmp
