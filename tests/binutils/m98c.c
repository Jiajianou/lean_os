/* tests/binutils/m98c.c - M98
 *
 * Compiled ON lean_os by the [m98] boot self-test's second half:
 *     gcc /tests/m98c.c -o m98c && ./m98c
 * with no flag anywhere, which is the same bar tests/gcc/hello.c set
 * for the cross compiler in M94 - only here the compiler, assembler,
 * linker, headers, startup files and libc are all read from this
 * machine's own disk by this machine's own processes.
 *
 * Deliberately more than "hello": the preprocessor pulls three real
 * headers through the on-image include paths, malloc exercises the
 * libc out of /usr/lib/libc.a, and the printf goes through the same
 * format engine M98 spent a differential test on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char *buf = malloc(64);
    if (!buf) {
        printf("FAIL: malloc\n");
        return 1;
    }
    strcpy(buf, "gcc built this program on this machine");
    printf("%s (%.1f%% self-hosted)\n", buf, 99.5);
    free(buf);
    return 0;
}
