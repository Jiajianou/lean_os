#include <stdio.h>
#include <string.h>
#include "layout.h"
extern const layout_row_t linux_rows[];
extern const unsigned linux_row_count;
extern const layout_row_t lean_rows[];
extern const unsigned lean_row_count;
int main(void) {
    int bad = 0;
    if (linux_row_count != lean_row_count) {
        printf("row counts differ\n");
        return 1;
    }
    for (unsigned i = 0; i < linux_row_count; i++) {
        const layout_row_t *a = &linux_rows[i], *b = &lean_rows[i];
        if (strcmp(a->key, b->key) || a->offset != b->offset || a->size != b->size) {
            printf("DIFFER %-24s linux offset %zu size %zu, lean_os offset %zu size %zu\n", a->key, a->offset,
                   a->size, b->offset, b->size);
            bad++;
        }
    }
    printf("%s: %u field(s) compared against Linux's iwlwifi API headers, %d differ\n", bad ? "FAIL" : "PASS",
           linux_row_count, bad);
    return bad != 0;
}
