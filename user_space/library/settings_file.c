#include "settings_file.h"

#include <stddef.h>

#include "desktop_palette.h"
#include "string_utilities.h"
#include "syscall_wrappers.h"
#include "wallpaper.h"

#define SETTINGS_BUFFER 768

typedef enum {
    KEY_BACKGROUND = 0,
    KEY_ACCENT,
    KEY_WALLPAPER,
    KEY_ANIMATIONS,
    KEY_VOLUME,
    KEY_DISPLAY_WIDTH,
    KEY_DISPLAY_HEIGHT,
    KEY_POINTER_SPEED,
    KEY_NATURAL_SCROLLING,
    KEY_SWAP_BUTTONS,
    KEY_CLOCK_24_HOUR,
    KEY_UTC_OFFSET,
    KEY_RESTORE_WINDOWS,
    KEY_COUNT,
} settings_key_t;

static const char *const KEY_NAMES[KEY_COUNT] = {
    "bg", "accent", "wallpaper", "animations", "volume", "display_w", "display_h",
    "pointer_speed", "natural_scrolling", "swap_buttons", "clock_24_hour", "utc_offset",
    "restore_windows",
};

typedef struct {
    uint32_t value[KEY_COUNT];
    uint8_t have[KEY_COUNT];
} settings_all_t;

static int parse_uint(const char *s, uint32_t *out) {
    uint32_t value = 0;
    int digits = 0;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') {
            d = *s - '0';
        } else if (base == 16 && *s >= 'a' && *s <= 'f') {
            d = *s - 'a' + 10;
        } else if (base == 16 && *s >= 'A' && *s <= 'F') {
            d = *s - 'A' + 10;
        } else {
            return 0;
        }
        value = value * (uint32_t)base + (uint32_t)d;
        digits++;
    }
    if (digits == 0) {
        return 0;
    }
    *out = value;
    return 1;
}

static void append_key(char *buffer, int *length, const char *key, uint32_t value) {
    for (int i = 0; key[i]; i++) {
        buffer[(*length)++] = key[i];
    }
    buffer[(*length)++] = '=';
    buffer[(*length)++] = '0';
    buffer[(*length)++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t nibble = (value >> shift) & 0xF;
        buffer[(*length)++] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    buffer[(*length)++] = '\n';
}

static void parse_all(settings_all_t *out) {
    char buffer[SETTINGS_BUFFER];
    for (size_t i = 0; i < sizeof(*out); i++) {
        ((char *)out)[i] = 0;
    }
    long n = sys_readfile(SETTINGS_FILE_NAME, buffer, sizeof(buffer) - 1);
    if (n <= 0 || n >= (long)sizeof(buffer)) {
        return;
    }
    buffer[n] = '\0';

    char *line = buffer;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') {
            end++;
        }
        int last = (*end == '\0');
        *end = '\0';

        char *eq = line;
        while (*eq && *eq != '=') {
            eq++;
        }
        if (*eq == '=') {
            *eq = '\0';
            uint32_t value;
            if (parse_uint(eq + 1, &value)) {
                for (int key = 0; key < KEY_COUNT; key++) {
                    if (strcmp(line, KEY_NAMES[key]) == 0) {
                        out->value[key] = value;
                        out->have[key] = 1;
                        break;
                    }
                }
            }
        }
        if (last) {
            break;
        }
        line = end + 1;
    }
    if (!out->have[KEY_DISPLAY_WIDTH] || !out->have[KEY_DISPLAY_HEIGHT] ||
        out->value[KEY_DISPLAY_WIDTH] == 0 || out->value[KEY_DISPLAY_HEIGHT] == 0) {
        out->have[KEY_DISPLAY_WIDTH] = 0;
        out->have[KEY_DISPLAY_HEIGHT] = 0;
    }
}

static int write_all(const settings_all_t *all) {
    char buffer[SETTINGS_BUFFER];
    int length = 0;
    for (int key = 0; key < KEY_COUNT; key++) {
        if (all->have[key]) {
            append_key(buffer, &length, KEY_NAMES[key], all->value[key]);
        }
    }
    return sys_writefile(SETTINGS_FILE_NAME, buffer, (size_t)length) == 0 ? 0 : -1;
}

void settings_file_defaults(window_manager_settings_request_t *out) {
    out->volume = 70;
    out->animations = 1;
    out->bg_color = DESKTOP_PALETTE_DEFAULT_BACKGROUND;
    out->accent_color = DESKTOP_PALETTE_DEFAULT_ACCENT;
    out->wallpaper = WALLPAPER_GRADIENT;
    out->pointer_speed = WINDOW_MANAGER_POINTER_SPEED_DEFAULT;
    out->natural_scrolling = 0;
    out->swap_buttons = 0;
    out->clock_24_hour = 1;
    out->utc_offset_minutes = 0;
    out->restore_windows = 1;
}

int settings_file_load(window_manager_settings_request_t *out) {
    settings_all_t all;
    parse_all(&all);
    if (all.have[KEY_POINTER_SPEED]) {
        out->pointer_speed = all.value[KEY_POINTER_SPEED];
    }
    if (all.have[KEY_NATURAL_SCROLLING]) {
        out->natural_scrolling = all.value[KEY_NATURAL_SCROLLING] != 0;
    }
    if (all.have[KEY_SWAP_BUTTONS]) {
        out->swap_buttons = all.value[KEY_SWAP_BUTTONS] != 0;
    }
    if (all.have[KEY_CLOCK_24_HOUR]) {
        out->clock_24_hour = all.value[KEY_CLOCK_24_HOUR] != 0;
    }
    if (all.have[KEY_UTC_OFFSET]) {
        out->utc_offset_minutes = (int32_t)all.value[KEY_UTC_OFFSET];
    }
    if (all.have[KEY_RESTORE_WINDOWS]) {
        out->restore_windows = all.value[KEY_RESTORE_WINDOWS] != 0;
    }
    if (all.have[KEY_ANIMATIONS]) {
        out->animations = all.value[KEY_ANIMATIONS];
    }
    if (all.have[KEY_VOLUME]) {
        out->volume = all.value[KEY_VOLUME];
    }
    if (!all.have[KEY_BACKGROUND] || !all.have[KEY_ACCENT] || !all.have[KEY_WALLPAPER]) {
        return 0;
    }
    out->bg_color = all.value[KEY_BACKGROUND];
    out->accent_color = all.value[KEY_ACCENT];
    out->wallpaper = all.value[KEY_WALLPAPER];
    return 1;
}

int settings_file_load_display(uint32_t *w, uint32_t *h) {
    settings_all_t all;
    parse_all(&all);
    if (!all.have[KEY_DISPLAY_WIDTH]) {
        return 0;
    }
    *w = all.value[KEY_DISPLAY_WIDTH];
    *h = all.value[KEY_DISPLAY_HEIGHT];
    return 1;
}

int settings_file_save(const window_manager_settings_request_t *in) {
    settings_all_t all;
    parse_all(&all);
    const uint32_t values[] = {
        [KEY_BACKGROUND] = in->bg_color,
        [KEY_ACCENT] = in->accent_color,
        [KEY_WALLPAPER] = in->wallpaper,
        [KEY_ANIMATIONS] = in->animations,
        [KEY_VOLUME] = in->volume,
        [KEY_POINTER_SPEED] = in->pointer_speed,
        [KEY_NATURAL_SCROLLING] = in->natural_scrolling,
        [KEY_SWAP_BUTTONS] = in->swap_buttons,
        [KEY_CLOCK_24_HOUR] = in->clock_24_hour,
        [KEY_UTC_OFFSET] = (uint32_t)in->utc_offset_minutes,
        [KEY_RESTORE_WINDOWS] = in->restore_windows,
    };
    for (int key = 0; key < (int)(sizeof(values) / sizeof(values[0])); key++) {
        if (key == KEY_DISPLAY_WIDTH || key == KEY_DISPLAY_HEIGHT) {
            continue;
        }
        all.value[key] = values[key];
        all.have[key] = 1;
    }
    return write_all(&all);
}

int settings_file_save_display(uint32_t w, uint32_t h) {
    settings_all_t all;
    parse_all(&all);
    all.value[KEY_DISPLAY_WIDTH] = w;
    all.value[KEY_DISPLAY_HEIGHT] = h;
    all.have[KEY_DISPLAY_WIDTH] = 1;
    all.have[KEY_DISPLAY_HEIGHT] = 1;
    return write_all(&all);
}
