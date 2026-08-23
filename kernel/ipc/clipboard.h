/* kernel/ipc/clipboard.h
 *
 * M32: a single, kernel-owned, global clipboard - one fixed buffer, no
 * per-process scoping, no format negotiation (plain bytes only). Same
 * "simplest thing that lets two unrelated processes share one piece of
 * state" reasoning kernel/ipc/shm.h's own header comment already gives
 * for shm ids being a flat global namespace: nothing this project ships
 * needs more than one clipboard, or needs to guess what an unrelated
 * process last put in it beyond "whatever bytes are there now."
 */
#pragma once

#include <stddef.h>

#define CLIPBOARD_MAX 256 /* generous for a terminal input line (gui_terminal.c's LINE_MAX is well under this) - the only writer today */

/* Copies `len` bytes (truncated to CLIPBOARD_MAX, not an error) into the
 * clipboard, replacing whatever was there. */
void clipboard_set(const void *buf, size_t len);

/* Copies up to `maxlen` bytes of the clipboard's current contents into
 * `buf` and returns how many bytes are actually stored (which may exceed
 * maxlen, mirroring leanfs_read's own "real size, caller's responsibility
 * to size its buffer" contract) - 0 if the clipboard has never been set. */
size_t clipboard_get(void *buf, size_t maxlen);
