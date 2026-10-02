#include "boot_options.h"

#define VIDEO_PREFERRED_SCORE (1 << 30)

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int token_equals(const char *text, uint32_t start, uint32_t end, const char *name) {
    uint32_t i = start;
    while (i < end && *name) {
        if (lower(text[i]) != *name) {
            return 0;
        }
        i++;
        name++;
    }
    return i == end && *name == 0;
}

static uint32_t parse_number(const char *text, uint32_t *cursor, uint32_t end) {
    uint32_t value = 0;
    while (*cursor < end && is_digit(text[*cursor])) {
        if (value > 100000000u) {
            return value;
        }
        value = value * 10u + (uint32_t)(text[*cursor] - '0');
        (*cursor)++;
    }
    return value;
}

void boot_options_defaults(boot_options_t *options) {
    options->magic = BOOT_OPTIONS_MAGIC;
    options->video_selection = BOOT_VIDEO_PREFER_1024_768;
    options->video_width = 0;
    options->video_height = 0;
    options->interrupts = BOOT_INTERRUPTS_UNSET;
    options->cpu_limit = 0;
    options->unknown_keys = 0;
    options->chosen_width = 0;
    options->chosen_height = 0;
    options->offered_count = 0;
    options->log_lba = 0;
    options->log_sectors = 0;
    options->display_scale = 0;
    options->config_lba = 0;
    options->config_sectors = 0;
}

static void apply_video(const char *text, uint32_t start, uint32_t end, boot_options_t *options) {
    if (token_equals(text, start, end, "firmware") || token_equals(text, start, end, "keep")) {
        options->video_selection = BOOT_VIDEO_FIRMWARE;
        options->video_width = 0;
        options->video_height = 0;
        return;
    }
    if (token_equals(text, start, end, "native") || token_equals(text, start, end, "largest")) {
        options->video_selection = BOOT_VIDEO_LARGEST;
        options->video_width = 0;
        options->video_height = 0;
        return;
    }
    uint32_t cursor = start;
    uint32_t width = parse_number(text, &cursor, end);
    if (cursor == start || cursor >= end || lower(text[cursor]) != 'x') {
        options->unknown_keys++;
        return;
    }
    cursor++;
    uint32_t height_start = cursor;
    uint32_t height = parse_number(text, &cursor, end);
    if (cursor == height_start || cursor != end || width == 0 || height == 0) {
        options->unknown_keys++;
        return;
    }
    options->video_selection = BOOT_VIDEO_EXACT;
    options->video_width = width;
    options->video_height = height;
}

static void apply_interrupts(const char *text, uint32_t start, uint32_t end, boot_options_t *options) {
    if (token_equals(text, start, end, "pic") || token_equals(text, start, end, "8259")) {
        options->interrupts = BOOT_INTERRUPTS_PIC;
        return;
    }
    if (token_equals(text, start, end, "ioapic") || token_equals(text, start, end, "apic")) {
        options->interrupts = BOOT_INTERRUPTS_IOAPIC;
        return;
    }
    options->unknown_keys++;
}

static void apply_cpus(const char *text, uint32_t start, uint32_t end, boot_options_t *options) {
    uint32_t cursor = start;
    uint32_t value = parse_number(text, &cursor, end);
    if (cursor != end || cursor == start || value == 0) {
        options->unknown_keys++;
        return;
    }
    options->cpu_limit = value;
}

/* scale=1 or scale=2 fixes how many physical pixels one of the desktop's is;
   scale=auto (and no line at all) lets the kernel decide from the panel. */
static void apply_scale(const char *text, uint32_t start, uint32_t end, boot_options_t *options) {
    if (token_equals(text, start, end, "auto")) {
        options->display_scale = 0;
        return;
    }
    uint32_t cursor = start;
    uint32_t value = parse_number(text, &cursor, end);
    if (cursor != end || cursor == start || value < 1 || value > 2) {
        options->unknown_keys++;
        return;
    }
    options->display_scale = value;
}

static int parse_extent(const char *text, uint32_t start, uint32_t end, uint32_t *lba, uint32_t *sectors) {
    uint32_t cursor = start;
    *lba = parse_number(text, &cursor, end);
    if (cursor == start || cursor >= end || text[cursor] != '+') {
        return 0;
    }
    cursor++;
    uint32_t sectors_start = cursor;
    *sectors = parse_number(text, &cursor, end);
    return cursor != sectors_start && cursor == end && *lba != 0;
}

/* M213: config=<lba>+<sectors> names this file's own sectors, so the kernel -
   which has no FAT driver - can rewrite a line of it in place. */
static void apply_config(const char *text, uint32_t start, uint32_t end, boot_options_t *options) {
    uint32_t lba = 0;
    uint32_t sectors = 0;
    if (!parse_extent(text, start, end, &lba, &sectors) || sectors == 0 || sectors > 8) {
        options->unknown_keys++;
        return;
    }
    options->config_lba = lba;
    options->config_sectors = sectors;
}

static void apply_log(const char *text, uint32_t start, uint32_t end, boot_options_t *options) {
    uint32_t cursor = start;
    uint32_t lba = parse_number(text, &cursor, end);
    if (cursor == start || cursor >= end || text[cursor] != '+') {
        options->unknown_keys++;
        return;
    }
    cursor++;
    uint32_t sectors_start = cursor;
    uint32_t sectors = parse_number(text, &cursor, end);
    if (cursor == sectors_start || cursor != end || lba == 0 || sectors < 2) {
        options->unknown_keys++;
        return;
    }
    options->log_lba = lba;
    options->log_sectors = sectors;
}

static void apply_pair(const char *text, uint32_t key_start, uint32_t key_end,
                       uint32_t value_start, uint32_t value_end, boot_options_t *options) {
    if (token_equals(text, key_start, key_end, "video")) {
        apply_video(text, value_start, value_end, options);
        return;
    }
    if (token_equals(text, key_start, key_end, "interrupts")) {
        apply_interrupts(text, value_start, value_end, options);
        return;
    }
    if (token_equals(text, key_start, key_end, "cpus")) {
        apply_cpus(text, value_start, value_end, options);
        return;
    }
    if (token_equals(text, key_start, key_end, "scale")) {
        apply_scale(text, value_start, value_end, options);
        return;
    }
    if (token_equals(text, key_start, key_end, "log")) {
        apply_log(text, value_start, value_end, options);
        return;
    }
    if (token_equals(text, key_start, key_end, "config")) {
        apply_config(text, value_start, value_end, options);
        return;
    }
    options->unknown_keys++;
}

void boot_options_parse(const char *text, uint32_t length, boot_options_t *options) {
    if (!text) {
        return;
    }
    uint32_t cursor = 0;
    while (cursor < length) {
        uint32_t line_end = cursor;
        while (line_end < length && text[line_end] != '\n') {
            line_end++;
        }

        uint32_t start = cursor;
        uint32_t end = line_end;
        while (start < end && is_space(text[start])) {
            start++;
        }
        while (end > start && is_space(text[end - 1])) {
            end--;
        }

        uint32_t comment = start;
        while (comment < end && text[comment] != '#') {
            comment++;
        }
        end = comment;
        while (end > start && is_space(text[end - 1])) {
            end--;
        }

        if (end > start) {
            uint32_t equals = start;
            while (equals < end && text[equals] != '=') {
                equals++;
            }
            if (equals == end) {
                options->unknown_keys++;
            } else {
                uint32_t key_end = equals;
                while (key_end > start && is_space(text[key_end - 1])) {
                    key_end--;
                }
                uint32_t value_start = equals + 1;
                while (value_start < end && is_space(text[value_start])) {
                    value_start++;
                }
                if (key_end == start || value_start == end) {
                    options->unknown_keys++;
                } else {
                    apply_pair(text, start, key_end, value_start, end, options);
                }
            }
        }

        cursor = line_end + 1;
    }
}

int boot_options_video_score(const boot_options_t *options, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return -1;
    }
    if (options->video_selection == BOOT_VIDEO_EXACT) {
        return (width == options->video_width && height == options->video_height) ? 1 : -1;
    }
    if (options->video_selection == BOOT_VIDEO_FIRMWARE) {
        return -1;
    }
    int area = (int)((width * height) & 0x3FFFFFFFu);
    if (options->video_selection == BOOT_VIDEO_PREFER_1024_768 && width == 1024 && height == 768) {
        return VIDEO_PREFERRED_SCORE;
    }
    return area;
}

int boot_options_use_ioapic(const boot_options_t *options, int firmware_config_present,
                            int firmware_config_asked_for_ioapic) {
    if (options->interrupts == BOOT_INTERRUPTS_IOAPIC) {
        return 1;
    }
    if (options->interrupts == BOOT_INTERRUPTS_PIC) {
        return 0;
    }
    if (firmware_config_present) {
        return firmware_config_asked_for_ioapic ? 1 : 0;
    }
    return 1;
}

static boot_options_t active_options;
static int active_options_ready;

void boot_options_set_active(const boot_options_t *handoff) {
    boot_options_defaults(&active_options);
    active_options_ready = 1;
    if (!handoff || handoff->magic != BOOT_OPTIONS_MAGIC) {
        return;
    }
    active_options.video_selection = handoff->video_selection;
    active_options.video_width = handoff->video_width;
    active_options.video_height = handoff->video_height;
    active_options.interrupts = handoff->interrupts;
    active_options.cpu_limit = handoff->cpu_limit;
    active_options.unknown_keys = handoff->unknown_keys;
    active_options.chosen_width = handoff->chosen_width;
    active_options.chosen_height = handoff->chosen_height;
    active_options.offered_count = handoff->offered_count;
    active_options.log_lba = handoff->log_lba;
    active_options.log_sectors = handoff->log_sectors;
    active_options.display_scale = handoff->display_scale <= 2 ? handoff->display_scale : 0;
    active_options.config_lba = handoff->config_lba;
    active_options.config_sectors = handoff->config_sectors <= 8 ? handoff->config_sectors : 0;
    if (active_options.offered_count > BOOT_OFFERED_MODES_MAX) {
        active_options.offered_count = BOOT_OFFERED_MODES_MAX;
    }
    for (uint32_t i = 0; i < active_options.offered_count; i++) {
        active_options.offered[i][0] = handoff->offered[i][0];
        active_options.offered[i][1] = handoff->offered[i][1];
    }
}

const boot_options_t *boot_options_active(void) {
    if (!active_options_ready) {
        boot_options_defaults(&active_options);
        active_options_ready = 1;
    }
    return &active_options;
}
