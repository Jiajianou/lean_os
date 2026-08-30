/* user_space/lib/recent.h - M74
 *
 * The list of files somebody opened recently, shared by every program
 * that opens one. Kept in /etc/recent.conf as one absolute path per line,
 * newest first.
 *
 * "The single highest-value-per-line feature on any desktop", which is
 * the milestone's own wording and is why this file is thirty lines rather
 * than a subsystem: what makes recents useful is that *every* way of
 * opening a file records it, not that any one of them records it well.
 * So this is a library rather than a service - the editor, the file
 * manager and the launcher each call it directly, and there is no process
 * to be running for it to work.
 *
 * Plain text, like settings.conf and session.conf, for the same reason:
 * it can be read with `cat` and deleted with `rm` when it has remembered
 * something you would rather it had not.
 */
#pragma once

#include "paths.h"

#define RECENT_PATH  PATH_ETC_DIR "recent.conf"
#define RECENT_MAX   8

/* Moves `path` to the front of the list, writing the file. A path that is
 * already there is moved rather than duplicated; the oldest falls off the
 * end. Silently does nothing for an empty or over-long path, and for a
 * caller without CAP_FS_WRITE - a program that could not record a recent
 * file should still open it. */
void recent_add(const char *path);

/* Fills `out` with up to `max` paths, newest first, skipping any that no
 * longer exist - a list that offers a file that was deleted is worse than
 * a shorter one. Returns how many were written. */
int recent_load(char out[][PATH_MAX_LEN], int max);
