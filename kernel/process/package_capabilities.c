#include "package_capabilities.h"

#include "caps.h"
#include "../drivers/kernel_log.h"
#include "../file_system/virtual_file_system.h"

static char registry[PKG_REGISTRY_MAX];
static int  registry_length;
static int  registry_loaded;
static int  registry_entries;

void pkg_registry_invalidate(void) {
    registry_loaded = 0;
    registry_length = 0;
    registry_entries = 0;
}

static void count_entries(void) {
    registry_entries = 0;
    int i = 0;
    while (i < registry_length) {
        int start = i;
        while (i < registry_length && registry[i] != '\n') {
            i++;
        }
        int end = i;
        if (i < registry_length) {
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
    if (!virtual_file_system_exists(PKG_REGISTRY_PATH)) {
        registry_length = 0;
        registry_entries = 0;
        registry_loaded = 1;
        return;
    }
    int64_t n = virtual_file_system_read(PKG_REGISTRY_PATH, registry, sizeof(registry));
    if (n < 0 || n >= (int64_t)sizeof(registry)) {
        kernel_log_puts("[pkg] " PKG_REGISTRY_PATH " could not be read whole - "
                  "every package will run with no capabilities until it can\n");
        registry_length = 0;
        registry_entries = 0;
        registry_loaded = -1;
        return;
    }
    registry_length = (int)n;
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

static uint32_t pkg_caps_for_path(const char *path) {
    if (!path || !path_is_under_pkg(path)) {
        return CAP_PKG_UNLISTED;
    }
    load();
    if (registry_loaded != 1) {
        return CAP_PKG_UNLISTED;
    }

    int i = 0;
    while (i < registry_length) {
        int start = i;
        while (i < registry_length && registry[i] != '\n') {
            i++;
        }
        int end = i;
        if (i < registry_length) {
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

        uint32_t mask = 0;
        int digits = 0;
        int p = start;
        while (p < end) {
            int v = hex_value(registry[p]);
            if (v < 0) {
                break;
            }
            if (digits >= 8) {
                digits = -1;
                break;
            }
            mask = (mask << 4) | (uint32_t)v;
            digits++;
            p++;
        }
        if (digits <= 0) {
            continue;
        }
        if (p >= end || (registry[p] != ' ' && registry[p] != '\t')) {
            continue;
        }
        while (p < end && (registry[p] == ' ' || registry[p] == '\t')) {
            p++;
        }
        if (p >= end || registry[p] != '/') {
            continue;
        }

        int q = p;
        const char *want = path;
        while (q < end && *want && registry[q] == *want) {
            q++;
            want++;
        }
        if (q == end && *want == '\0') {
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
