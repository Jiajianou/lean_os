#include "clipboard.h"

#include <stdint.h>

#include "lib/libk.h"
#include "lib/spinlock.h"

static uint8_t clipboard_buf[CLIPBOARD_MAX];
static size_t clipboard_len;

/* ---- M67: clipboard_lock ---------------------------------------------
 *
 * What it protects: the one shared buffer and its length, together.
 *
 * Against whom: a reader arriving between the k_memcpy and the length
 * assignment in clipboard_set. Without the lock that reader gets the new
 * bytes with the old length - a copy that is half of one thing and half
 * of another, which is the worst of the three possible answers and the
 * only one that is not a valid clipboard. Interrupts off for
 * consistency with every other lock a dying task can be holding. */
static spinlock_t clipboard_lock;

void clipboard_set(const void *buf, size_t len) {
    if (len > CLIPBOARD_MAX) {
        len = CLIPBOARD_MAX;
    }
    uint64_t flags = spin_lock_irqsave(&clipboard_lock);
    k_memcpy(clipboard_buf, buf, len);
    clipboard_len = len;
    spin_unlock_irqrestore(&clipboard_lock, flags);
}

size_t clipboard_get(void *buf, size_t maxlen) {
    uint64_t flags = spin_lock_irqsave(&clipboard_lock);
    size_t real_len = clipboard_len;
    size_t to_copy = real_len < maxlen ? real_len : maxlen;
    k_memcpy(buf, clipboard_buf, to_copy);
    spin_unlock_irqrestore(&clipboard_lock, flags);
    return real_len;
}
