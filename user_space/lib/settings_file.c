#include "settings_file.h"

#include "str.h"
#include "syscall_wrappers.h"

/* Comfortably larger than the three lines this ever writes (about 50
 * bytes), and small enough to be a plain stack/static buffer in either of
 * the two processes that use it. A file bigger than this is truncated on
 * read, which parse_line then rejects as malformed - the correct outcome
 * for something that is not the file this wrote. */
#define SETTINGS_BUF 256

/* "0x1A1A2E" or "1717294" into *out. Returns 1 on a clean parse of at
 * least one digit with nothing but digits after it; 0 otherwise, which is
 * what makes a hand-edited typo fall back to defaults instead of
 * silently becoming some other color. */
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

/* Appends "key=0xVALUE\n" to buf at *len. No printf in this project (no
 * libc at all - see milestones.md's ground rules), so the hex digits are
 * emitted by hand; 8 of them, always, so the file is fixed-width and easy
 * to eyeball. */
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

int settings_file_load(wm_settings_request_t *out) {
    char buf[SETTINGS_BUF];
    long n = sys_readfile(SETTINGS_FILE_NAME, buf, sizeof(buf) - 1);
    if (n <= 0 || n >= (long)sizeof(buf)) {
        return 0;
    }
    buf[n] = '\0';

    uint32_t bg = 0, accent = 0, wallpaper = 0;
    int have_bg = 0, have_accent = 0, have_wallpaper = 0;

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
                    bg = value;
                    have_bg = 1;
                } else if (strcmp(line, "accent") == 0) {
                    accent = value;
                    have_accent = 1;
                } else if (strcmp(line, "wallpaper") == 0) {
                    wallpaper = value;
                    have_wallpaper = 1;
                }
            }
        }
        if (last) {
            break;
        }
        line = end + 1;
    }

    if (!have_bg || !have_accent || !have_wallpaper) {
        return 0;
    }
    out->bg_color = bg;
    out->accent_color = accent;
    out->wallpaper = wallpaper;
    return 1;
}

int settings_file_save(const wm_settings_request_t *in) {
    char buf[SETTINGS_BUF];
    int len = 0;
    append_key(buf, &len, "bg", in->bg_color);
    append_key(buf, &len, "accent", in->accent_color);
    append_key(buf, &len, "wallpaper", in->wallpaper);
    return sys_writefile(SETTINGS_FILE_NAME, buf, (size_t)len) == 0 ? 0 : -1;
}
