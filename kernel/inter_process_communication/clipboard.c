#include "clipboard.h"

#include <stdint.h>

#include "library/kernel_library.h"
#include "library/spinlock.h"

static uint8_t clipboard_buffer[CLIPBOARD_MAX];
static size_t clipboard_length;

static spinlock_t clipboard_lock;

void clipboard_set(const void *buf, size_t len) {
    if (len > CLIPBOARD_MAX) {
        len = CLIPBOARD_MAX;
    }
    uint64_t flags = spin_lock_irqsave(&clipboard_lock);
    k_memcpy(clipboard_buffer, buf, len);
    clipboard_length = len;
    spin_unlock_irqrestore(&clipboard_lock, flags);
}

size_t clipboard_get(void *buf, size_t maxlen) {
    uint64_t flags = spin_lock_irqsave(&clipboard_lock);
    size_t real_length = clipboard_length;
    size_t to_copy = real_length < maxlen ? real_length : maxlen;
    k_memcpy(buf, clipboard_buffer, to_copy);
    spin_unlock_irqrestore(&clipboard_lock, flags);
    return real_length;
}
