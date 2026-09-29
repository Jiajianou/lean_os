#pragma once

#include <stdint.h>

#define BOOT_VIDEO_PREFER_1024_768 0
#define BOOT_VIDEO_LARGEST         1
#define BOOT_VIDEO_EXACT           2
#define BOOT_VIDEO_FIRMWARE        3

#define BOOT_INTERRUPTS_UNSET  0
#define BOOT_INTERRUPTS_PIC    1
#define BOOT_INTERRUPTS_IOAPIC 2

#define BOOT_OFFERED_MODES_MAX 16

#define BOOT_OPTIONS_MAGIC 0x4C4E4F5054424FULL

typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint32_t video_selection;
    uint32_t video_width;
    uint32_t video_height;
    uint32_t interrupts;
    uint32_t cpu_limit;
    uint32_t unknown_keys;
    uint32_t chosen_width;
    uint32_t chosen_height;
    uint32_t offered_count;
    uint32_t offered[BOOT_OFFERED_MODES_MAX][2];
    uint32_t log_lba;
    uint32_t log_sectors;
    uint32_t display_scale;
} boot_options_t;

void boot_options_defaults(boot_options_t *options);

void boot_options_parse(const char *text, uint32_t length, boot_options_t *options);

int boot_options_video_score(const boot_options_t *options, uint32_t width, uint32_t height);

void boot_options_set_active(const boot_options_t *handoff);

const boot_options_t *boot_options_active(void);

int boot_options_use_ioapic(const boot_options_t *options, int firmware_config_present,
                            int firmware_config_asked_for_ioapic);
