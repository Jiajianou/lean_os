#include <errno.h>

static __thread int lean_errno;

int *__errno_location(void) {
    return &lean_errno;
}
