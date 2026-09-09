/* kernel/proc/pkgcaps.c - M111. See pkgcaps.h for the design. */
#include "pkgcaps.h"

#include "caps.h" /* system_api/include/caps.h */
#include "../drivers/klog.h"
#include "../fs/vfs.h"

/* The registry, held whole. A static buffer rather than a heap
 * allocation for the reason every long-lived kernel table here is
 * static: this is read on a spawn, and a spawn that can fail because the
 * heap is busy is a spawn that fails at the worst moment. 32 KiB of
 * .bss, and the ceiling is checked rather than assumed. */
static char registry[PKG_REGISTRY_MAX];
static int  registry_len;
static int  registry_loaded;   /* 0 = not loaded, 1 = loaded, -1 = failed */
static int  registry_entries;

void pkg_registry_invalidate(void) {
    registry_loaded = 0;
    registry_len = 0;
    registry_entries = 0;
}

/* Count the entries as a side effect of loading, so that
 * pkg_registry_count() does not need a second parse and the boot
 * self-test's number comes from the same code the lookup uses. */
static void count_entries(void) {
    registry_entries = 0;
    int i = 0;
    while (i < registry_len) {
        int start = i;
        while (i < registry_len && registry[i] != '\n') {
            i++;
        }
        int end = i;
        if (i < registry_len) {
            i++;
        }
        while (start < end && (registry[start] == ' ' || registry[start] == '\t')) {
            start++;
        }
        if (start < end && registry[start] != '#') {
            registry_entries++;
        }
    }
}

static void load(void) {
    if (registry_loaded) {
        return;
    }
    /* Absent is not an error: a machine with nothing installed has no
     * registry, and every lookup on it correctly finds nothing. Only a
     * registry that exists and cannot be used is worth a log line. */
    if (!vfs_exists(PKG_REGISTRY_PATH)) {
        registry_len = 0;
        registry_entries = 0;
        registry_loaded = 1;
        return;
    }
    int64_t n = vfs_read(PKG_REGISTRY_PATH, registry, sizeof(registry));
    if (n < 0 || n >= (int64_t)sizeof(registry)) {
        /* Fails closed, and says so. A registry at exactly the buffer
         * size is refused too: vfs_read cannot distinguish "this is the
         * whole file" from "this is as much as fits", and guessing wrong
         * would silently drop whatever is past the cut - which is the
         * half of the file the last package installed is in. */
        klog_puts("[pkg] " PKG_REGISTRY_PATH " could not be read whole - "
                  "every package will run with no capabilities until it can\n");
        registry_len = 0;
        registry_entries = 0;
        registry_loaded = -1;
        return;
    }
    registry_len = (int)n;
    count_entries();
    registry_loaded = 1;
}

int pkg_registry_count(void) {
    load();
    return registry_loaded == 1 ? registry_entries : -1;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint32_t pkg_caps_for_path(const char *path) {
    if (!path || !path_is_under_pkg(path)) {
        /* Not this function's question. The caller decides what a
         * non-package path gets; answering anything here would make two
         * places responsible for one rule. */
        return CAP_PKG_UNLISTED;
    }
    load();
    if (registry_loaded != 1) {
        return CAP_PKG_UNLISTED;
    }

    int i = 0;
    while (i < registry_len) {
        int start = i;
        while (i < registry_len && registry[i] != '\n') {
            i++;
        }
        int end = i;
        if (i < registry_len) {
            i++;
        }
        if (end > start && registry[end - 1] == '\r') {
            end--;
        }
        while (start < end && (registry[start] == ' ' || registry[start] == '\t')) {
            start++;
        }
        if (start >= end || registry[start] == '#') {
            continue;
        }

        /* "<hex mask> <absolute path>" */
        uint32_t mask = 0;
        int digits = 0;
        int p = start;
        while (p < end) {
            int v = hex_value(registry[p]);
            if (v < 0) {
                break;
            }
            /* A mask longer than eight digits is a malformed line rather
             * than a big number: refuse the line instead of wrapping. */
            if (digits >= 8) {
                digits = -1;
                break;
            }
            mask = (mask << 4) | (uint32_t)v;
            digits++;
            p++;
        }
        if (digits <= 0) {
            continue; /* not a line this understands - skipped, not fatal */
        }
        if (p >= end || (registry[p] != ' ' && registry[p] != '\t')) {
            continue;
        }
        while (p < end && (registry[p] == ' ' || registry[p] == '\t')) {
            p++;
        }
        if (p >= end || registry[p] != '/') {
            continue; /* the path has to be absolute to be comparable */
        }

        /* Exact match on the whole rest of the line. Not a prefix: a
         * registry entry for /pkg/grep/3.11/bin/grep must not authorize
         * /pkg/grep/3.11/bin/grep-something-else. */
        int q = p;
        const char *want = path;
        while (q < end && *want && registry[q] == *want) {
            q++;
            want++;
        }
        if (q == end && *want == '\0') {
            /* The second of the two intersections. The first is in `os`,
             * where it produces a message; this one is why that message
             * being absent does not matter. */
            return mask & (uint32_t)CAP_PKG_MAX;
        }
    }
    return CAP_PKG_UNLISTED;
}

uint32_t caps_for_spawn_path(const char *path) {
    if (path && path_is_under_pkg(path)) {
        return pkg_caps_for_path(path);
    }
    return caps_for_program(path ? path : "");
}
