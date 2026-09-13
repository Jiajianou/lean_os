#include "str.h"
#include "syscall_wrappers.h"

int main(void) {
    const char msg[] = "Hello, world from user_space!\n";
    sys_write(1, msg, strlen(msg));

    long pid = sys_getpid();
    char pid_msg[] = "hello: my pid is 0x0000000000000000\n";
    const char *hexdigits = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        int shift = (15 - i) * 4;
        pid_msg[19 + i] = hexdigits[(pid >> shift) & 0xF];
    }
    sys_write(1, pid_msg, strlen(pid_msg));

    return 0;
}
