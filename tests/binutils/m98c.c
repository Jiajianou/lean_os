#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char *buffer = malloc(64);
    if (!buffer) {
        printf("FAIL: malloc\n");
        return 1;
    }
    strcpy(buffer, "gcc built this program on this machine");
    printf("%s (%.1f%% self-hosted)\n", buffer, 99.5);
    free(buffer);
    return 0;
}
