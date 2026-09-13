#include "settings_file.h"

#include "str.h"
#include "syscall_wrappers.h"

#define SETTINGS_BUF 256

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

static void append_key(char *buf, int *len, const char *key, uint32_t value) {
    for (int i = 0; key[i]; i++) {
        buf[(*len)++] = key[i];
    }
    buf[(*len)++] = '=';
    buf[(*len)++] = '0';
    buf[(*len)++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t nibble = (value >> shift) & 0xF;
        buf[(*len)++] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    buf[(*len)++] = '\n';
}

typedef struct {
    uint32_t bg, accent, wallpaper, display_w, display_h, animations, volume;
    int have_bg, have_accent, have_wallpaper, have_display, have_animations, have_volume;
} settings_all_t;

static void parse_all(settings_all_t *out) {
    char buf[SETTINGS_BUF];
    for (int i = 0; i < (int)sizeof(*out); i++) {
        ((char *)out)[i] = 0;
    }
    long n = sys_readfile(SETTINGS_FILE_NAME, buf, sizeof(buf) - 1);
    if (n <= 0 || n >= (long)sizeof(buf)) {
        return;
    }
    buf[n] = '\0';

    int have_w = 0, have_h = 0;
    char *line = buf;
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
                if (strcmp(line, "bg") == 0) {
                    out->bg = value;
                    out->have_bg = 1;
                } else if (strcmp(line, "accent") == 0) {
                    out->accent = value;
                    out->have_accent = 1;
                } else if (strcmp(line, "wallpaper") == 0) {
                    out->wallpaper = value;
                    out->have_wallpaper = 1;
                } else if (strcmp(line, "animations") == 0) {
                    out->animations = value;
                    out->have_animations = 1;
                } else if (strcmp(line, "volume") == 0) {
                    out->volume = value;
                    out->have_volume = 1;
                } else if (strcmp(line, "display_w") == 0) {
                    out->display_w = value;
                    have_w = 1;
                } else if (strcmp(line, "display_h") == 0) {
                    out->display_h = value;
                    have_h = 1;
                }
            }
        }
        if (last) {
            break;
        }
        line = end + 1;
    }
    out->have_display = have_w && have_h && out->display_w > 0 && out->display_h > 0;
}

static int write_all(const settings_all_t *all) {
    char buf[SETTINGS_BUF];
    int len = 0;
    append_key(buf, &len, "bg", all->bg);
    append_key(buf, &len, "accent", all->accent);
    append_key(buf, &len, "wallpaper", all->wallpaper);
    append_key(buf, &len, "animations", all->animations);
    append_key(buf, &len, "volume", all->volume);
    if (all->have_display) {
        append_key(buf, &len, "display_w", all->display_w);
        append_key(buf, &len, "display_h", all->display_h);
    }
    return sys_writefile(SETTINGS_FILE_NAME, buf, (size_t)len) == 0 ? 0 : -1;
}

int settings_file_load(wm_settings_request_t *out) {
    settings_all_t all;
    parse_all(&all);
    if (!all.have_bg || !all.have_accent || !all.have_wallpaper) {
        return 0;
    }
    out->bg_color = all.bg;
    out->accent_color = all.accent;
    out->wallpaper = all.wallpaper;
    if (all.have_animations) {
        out->animations = all.animations;
    }
    if (all.have_volume) {
        out->volume = all.volume;
    }
    return 1;
}

int settings_file_load_display(uint32_t *w, uint32_t *h) {
    settings_all_t all;
    parse_all(&all);
    if (!all.have_display) {
        return 0;
    }
    *w = all.display_w;
    *h = all.display_h;
    return 1;
}

int settings_file_save(const wm_settings_request_t *in) {
    settings_all_t all;
    parse_all(&all);
    all.bg = in->bg_color;
    all.accent = in->accent_color;
    all.wallpaper = in->wallpaper;
    all.animations = in->animations;
    all.have_animations = 1;
    all.volume = in->volume;
    all.have_volume = 1;
    return write_all(&all);
}

int settings_file_save_display(uint32_t w, uint32_t h) {
    settings_all_t all;
    parse_all(&all);
    all.display_w = w;
    all.display_h = h;
    all.have_display = 1;
    return write_all(&all);
}
