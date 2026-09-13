#include <string.h>

int dyn_answer(void);

int dyn_answer(void) {
    static const char six[] = "424242";
    return (int)strlen(six) * 7;
}
