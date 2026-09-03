/* tests/binutils/m98mk - M98: a BUILD on the machine, not a compile.
 * Two translation units, an incremental rule graph, and make deciding
 * what is out of date by stat()ing leanfs - which is the part a single
 * `gcc file.c` cannot grade. */
#include <stdio.h>
int lib_answer(void);
int main(void) {
    printf("make built this on this machine: %d\n", lib_answer());
    return 0;
}
