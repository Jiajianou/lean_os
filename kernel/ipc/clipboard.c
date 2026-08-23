#include "clipboard.h"

#include <stdint.h>

#include "lib/libk.h"

static uint8_t clipboard_buf[CLIPBOARD_MAX];
static size_t clipboard_len;

void clipboard_set(const void *buf, size_t len) {
    if (len > CLIPBOARD_MAX) {
        len = CLIPBOARD_MAX;
    }
    k_memcpy(clipboard_buf, buf, len);
    clipboard_len = len;
}

size_t clipboard_get(void *buf, size_t maxlen) {
    size_t to_copy = clipboard_len < maxlen ? clipboard_len : maxlen;
    k_memcpy(buf, clipboard_buf, to_copy);
    return clipboard_len;
}
