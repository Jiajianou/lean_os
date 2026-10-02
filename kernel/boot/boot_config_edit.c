#include "boot_config_edit.h"

/* M213. \EFI\BOOT\lean_os.cfg is the one place a choice can reach the boot
   loader, which picks the graphics mode before this kernel exists. The kernel
   has no FAT driver, so the image tool makes the file a fixed run of sectors
   padded with newlines and names them with config=; an edit rewrites a line
   inside that run and pads the rest, which leaves the file's size, its
   clusters and the FAT exactly as they were. */

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static uint32_t text_length(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int boot_config_is_ours(const char *text, uint32_t length) {
    const char *header = BOOT_CONFIG_HEADER;
    uint32_t n = text_length(header);
    if (length < n) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (text[i] != header[i]) {
            return 0;
        }
    }
    return 1;
}

/* Everything up to and including the last line that says something; the
   rest is the padding that keeps the file its size. */
uint32_t boot_config_content_length(const char *text, uint32_t length) {
    uint32_t end = length;
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\0' || is_space(text[end - 1]))) {
        end--;
    }
    if (end > 0 && end < length && text[end] == '\n') {
        end++;
    }
    return end;
}

static int line_has_key(const char *text, uint32_t start, uint32_t end, const char *key) {
    while (start < end && is_space(text[start])) {
        start++;
    }
    uint32_t i = start;
    while (*key && i < end) {
        if (lower(text[i]) != lower(*key)) {
            return 0;
        }
        i++;
        key++;
    }
    if (*key) {
        return 0;
    }
    while (i < end && is_space(text[i])) {
        i++;
    }
    return i < end && text[i] == '=';
}

/* Replaces the line naming key with key=value, or appends one; value 0
   removes the line. Returns -1, and changes nothing, when the area is not
   this project's or the result would not fit. */
int boot_config_set(char *text, uint32_t capacity, const char *key, const char *value) {
    if (!boot_config_is_ours(text, capacity)) {
        return -1;
    }
    uint32_t length = boot_config_content_length(text, capacity);
    uint32_t line_start = length;
    uint32_t line_end = length;
    for (uint32_t cursor = 0; cursor < length;) {
        uint32_t end = cursor;
        while (end < length && text[end] != '\n') {
            end++;
        }
        if (line_has_key(text, cursor, end, key)) {
            line_start = cursor;
            line_end = end < length ? end + 1 : end;
            break;
        }
        cursor = end + 1;
    }

    uint32_t key_length = text_length(key);
    uint32_t value_length = value ? text_length(value) : 0;
    uint32_t replacement = value ? key_length + 1 + value_length + 1 : 0;
    int appending = line_start == length;
    uint32_t separator = appending && length > 0 && text[length - 1] != '\n' ? 1 : 0;
    uint32_t tail = length - line_end;
    uint32_t new_length = line_start + separator + replacement + tail;
    if (new_length > capacity) {
        return -1;
    }

    if (line_start + separator + replacement > line_end) {
        uint32_t shift = line_start + separator + replacement - line_end;
        for (uint32_t i = tail; i > 0; i--) {
            text[line_end + shift + i - 1] = text[line_end + i - 1];
        }
    } else {
        uint32_t shift = line_end - (line_start + separator + replacement);
        for (uint32_t i = 0; i < tail; i++) {
            text[line_end - shift + i] = text[line_end + i];
        }
    }
    uint32_t at = line_start;
    if (separator) {
        text[at++] = '\n';
    }
    if (value) {
        for (uint32_t i = 0; i < key_length; i++) {
            text[at++] = key[i];
        }
        text[at++] = '=';
        for (uint32_t i = 0; i < value_length; i++) {
            text[at++] = value[i];
        }
        text[at++] = '\n';
    }
    for (uint32_t i = new_length; i < capacity; i++) {
        text[i] = '\n';
    }
    return (int)new_length;
}
