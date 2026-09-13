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
