/* user_space/lib/settings_file.h
 *
 * M47: the desktop's three settings, on disk. Until this milestone this
 * OS remembered nothing between boots - including the wallpaper M44 had
 * just added a picker for.
 *
 * Plain `key=value` text, deliberately. leanfs is flat, this project has
 * no config format, and inventing a binary one to carry three integers
 * would be more code than parsing three lines - and unreadable with
 * `cat`, which is the only debugging tool this OS has for a file.
 *
 * Shared between the two processes that touch it rather than parsed twice:
 * settings.c writes it when a control is clicked, and compositor.c reads
 * it once at startup, before the first client connects. Two hand-written
 * parsers that had to agree on the same three key names is exactly the
 * kind of thing that drifts.
 */
#pragma once

#include "paths.h" /* system_api/include/paths.h - PATH_SETTINGS, M53 */
#include "wm.h" /* system_api/include/wm.h - wm_settings_request_t, the same three fields the live protocol carries */

/* M53: /etc/settings.conf. It sat at the top level next to the
 * compositor until then, because a flat filesystem had nowhere else to
 * put it - and that is exactly why the launcher used to offer to run it. */
#define SETTINGS_FILE_NAME PATH_SETTINGS

/* Reads and parses SETTINGS_FILE_NAME into *out. Returns 1 if every one
 * of the three keys was present and parsed; returns 0 - with *out left
 * exactly as the caller had it - if the file is missing, unreadable, or
 * malformed in any way.
 *
 * "All three or nothing" rather than per-key merging: a half-parsed file
 * would hand the compositor one real color and two zeros, which is a
 * black desktop that looks like a bug in the compositor rather than a
 * damaged file. The caller pre-fills *out with its own compiled-in
 * defaults and simply keeps them on a 0. */
int settings_file_load(wm_settings_request_t *out);

/* Writes the three settings as text. Returns 0, or -1 if the write
 * failed (SYS_writefile's own failure - a full disk or no free inode). */
int settings_file_save(const wm_settings_request_t *in);
