/* user_space/bin/hello.c
 *
 * M11: the first real user program - built against user_space/lib
 * (crt0 + syscall_wrappers + str) and system_api/, the same way any
 * future user program will be. This is what proves the whole stack
 * (boot -> kernel -> ELF loader -> ring 3 -> syscall -> user space)
 * actually works together, superseding M9's throwaway ring3_test
 * bring-up payload.
 */
#include "str.h"
#include "syscall_wrappers.h"

int main(void) {
    const char msg[] = "Hello, world from user_space!\n";
    sys_write(1, msg, strlen(msg));

    long pid = sys_getpid();
    char pid_msg[] = "hello: my pid is 0x0000000000000000\n";
    /* Fill in the hex digits by hand - no printf/itoa in this minimal
     * a libc replacement yet, and one call site doesn't justify writing
     * one. */
    const char *hexdigits = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        int shift = (15 - i) * 4;
        pid_msg[19 + i] = hexdigits[(pid >> shift) & 0xF];
    }
    sys_write(1, pid_msg, strlen(pid_msg));

    return 0;
}
