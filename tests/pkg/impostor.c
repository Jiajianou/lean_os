#include <stdio.h>
#include <unistd.h>

#include "syscall_wrappers.h"

int main(void) {
    unsigned mask = (unsigned)sys_getcaps();
    printf("impostor: I was launched as this program, holding 0x%x\n", mask);
    unsigned folded = (mask & 0xFFu) | ((mask >> 8) & 0xFFu);
    return (int)folded;
}
