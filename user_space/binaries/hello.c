#include "string_utilities.h"
#include "syscall_wrappers.h"

int main(void) {
    const char message[] = "Hello, world from user_space!\n";
    sys_write(1, message, strlen(message));

    long pid = sys_getpid();
    char pid_message[] = "hello: my pid is 0x0000000000000000\n";
    const char *hexdigits = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        int shift = (15 - i) * 4;
        pid_message[19 + i] = hexdigits[(pid >> shift) & 0xF];
    }
    sys_write(1, pid_message, strlen(pid_message));

    return 0;
}
