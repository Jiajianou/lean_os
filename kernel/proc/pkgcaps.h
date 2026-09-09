/* kernel/proc/pkgcaps.h - M111: what an installed package may do.
 *
 * The kernel side of the package manager, and the smallest piece of it
 * that has to be in the kernel at all. Everything else about a package -
 * fetching, verifying, unpacking, recording - happens in /bin/os, in
 * user space, where it can be read and where a bug is a program that
 * fails. This is the part that cannot live there: the decision about
 * what capabilities a program spawned out of /pkg starts with.
 *
 * ---- why the registry is a file, and why that is not a hole ----------
 *
 * `os` writes /pkg/db/caps, one line per installed executable, and this
 * reads it. A file the kernel trusts, written by a program, sounds like
 * exactly the "fake check" M65 refuses to build - so here is why it is
 * not:
 *
 *   1. Nothing but `os` can write it. CAP_PKG_ADMIN gates every write
 *      whose path resolves under /pkg, in the kernel, at the same door
 *      CAP_FS_WRITE is checked. `os` is the only program in CAP_GRANTS
 *      that holds it.
 *   2. Whatever it says is intersected with CAP_PKG_MAX here. A registry
 *      that asked for the framebuffer would not get it even if it were
 *      somehow written, so the worst a compromised registry can do is
 *      hand a package the network.
 *   3. A registry that cannot be read, or does not fit, or does not
 *      parse, grants CAP_PKG_UNLISTED - which is zero. It fails closed,
 *      and the failure is logged.
 *
 * What it is NOT a defence against is this disk being edited by
 * something that is not this OS. There are no signatures here and no
 * secret to sign with; docs/packages.md says so in as many words rather
 * than leaving it implied.
 *
 * ---- the cache, and what invalidates it -------------------------------
 *
 * Read once, on the first spawn out of /pkg, and kept. Every successful
 * write under /pkg drops it - which the kernel can do reliably precisely
 * because it is already checking every such write for CAP_PKG_ADMIN. So
 * `os install` is visible to the next spawn without `os` having to say
 * anything, and without a spawn costing a disk read.
 */
#pragma once

#include <stdint.h>

/* The registry's path. Under /pkg, so the write gate covers it. */
#define PKG_REGISTRY_PATH "/pkg/db/caps"

/* Every line is "<hex mask> <absolute path>", and the path runs to the
 * end of the line - so a package path containing a space still works,
 * and the field that could contain one is last. '#' starts a comment. */

/* Bounded, and the bound is a refusal rather than a truncation: a
 * registry larger than this grants nothing at all, because a registry
 * read halfway is a registry that silently forgets packages. 32 KiB is
 * about 400 executables. */
#define PKG_REGISTRY_MAX 32768

/* What `path` should be launched with. `path` must be absolute and
 * normalized. Returns CAP_PKG_UNLISTED for anything the registry does
 * not name, and never returns a bit outside CAP_PKG_MAX. */
uint32_t pkg_caps_for_path(const char *path);

/* The capabilities a program loaded from `path` should be launched with:
 * the shipped grant table for anything outside /pkg, and the package
 * registry for anything inside it. This is the function every spawn and
 * exec in this kernel calls; caps_for_program() is no longer called
 * directly anywhere, on purpose, because a call site that used the name
 * table on a /pkg path would be the impersonation hole caps.h describes.
 *
 * `path` must be absolute and normalized. A bare name (which is what the
 * kernel's own boot-time spawns pass) is not under /pkg by definition
 * and reaches the name table, which is correct: those programs are the
 * ones the table is about. */
uint32_t caps_for_spawn_path(const char *path);

/* Drop the cached registry. Called from the syscall layer after any
 * successful write under /pkg. */
void pkg_registry_invalidate(void);

/* For the boot self-test and the host tests: how many entries the
 * registry currently holds, loading it if it is not loaded. -1 if it
 * could not be read. */
int pkg_registry_count(void);
