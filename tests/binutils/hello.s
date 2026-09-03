# tests/binutils/hello.s - M98
#
# The smallest program the machine's own assembler and linker can make:
# no libc, no crt0, the raw int 0x80 ABI from syscall_wrappers.c. It is
# deliberately beneath the runtime everything else links, because the
# thing under test is `as` and `ld` themselves - a fixture that pulled
# in libc.a would be testing the archive reader before the assembler.
#
# Assembled and linked ON lean_os by the [m98] boot self-test:
#     as hello.s -o hello.o && ld hello.o -o hello && ./hello
# with no flags anywhere, for M94's reason: every flag invented by hand
# is a flag someone else's build system will not pass, and the ld
# emulation the port teaches is what has to place this at the address
# the kernel loads from.

.globl _start
.text
_start:
    mov $0, %rax                # SYS_write
    mov $1, %rdi                # stdout
    lea msg(%rip), %rsi
    mov $len, %rdx
    int $0x80

    mov $1, %rax                # SYS_exit
    xor %rdi, %rdi
    int $0x80

.section .rodata
msg: .ascii "as and ld made this program on this machine\n"
len = . - msg
