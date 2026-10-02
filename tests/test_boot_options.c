#include "check.h"

#include "boot/boot_options.h"

#include <string.h>

static void parse(const char *text, boot_options_t *options) {
    boot_options_defaults(options);
    boot_options_parse(text, (uint32_t)strlen(text), options);
}

static int best_mode(const boot_options_t *options, const uint32_t (*modes)[2], int count,
                     uint32_t *width, uint32_t *height) {
    int best = -1;
    int best_score = 0;
    for (int i = 0; i < count; i++) {
        int score = boot_options_video_score(options, modes[i][0], modes[i][1]);
        if (score < 0) {
            continue;
        }
        if (best < 0 || score > best_score) {
            best = i;
            best_score = score;
        }
    }
    if (best < 0) {
        return 0;
    }
    *width = modes[best][0];
    *height = modes[best][1];
    return 1;
}

static const uint32_t thinkpad_modes[][2] = {
    {640, 480}, {800, 600}, {1024, 768}, {1280, 1024}, {1920, 1200},
};

static const uint32_t qemu_modes[][2] = {
    {640, 480}, {800, 600}, {1024, 768},
};

TEST(boot_options, an_absent_file_leaves_the_defaults_the_image_was_built_with) {
    boot_options_t options;
    boot_options_defaults(&options);
    CHECK_EQ(options.video_selection, (uint32_t)BOOT_VIDEO_PREFER_1024_768);
    CHECK_EQ(options.interrupts, (uint32_t)BOOT_INTERRUPTS_UNSET);
    CHECK_EQ(options.cpu_limit, 0u);
    CHECK_EQ(options.unknown_keys, 0u);
}

TEST(boot_options, the_default_picks_1024_768_over_a_panel_twice_its_size) {
    boot_options_t options;
    boot_options_defaults(&options);
    uint32_t width = 0, height = 0;
    CHECK_EQ(best_mode(&options, thinkpad_modes, 5, &width, &height), 1);
    CHECK_EQ(width, 1024u);
    CHECK_EQ(height, 768u);
}

TEST(boot_options, video_native_takes_the_panel_the_machine_actually_has) {
    boot_options_t options;
    parse("video=native\n", &options);
    CHECK_EQ(options.video_selection, (uint32_t)BOOT_VIDEO_LARGEST);
    uint32_t width = 0, height = 0;
    CHECK_EQ(best_mode(&options, thinkpad_modes, 5, &width, &height), 1);
    CHECK_EQ(width, 1920u);
    CHECK_EQ(height, 1200u);
}

TEST(boot_options, video_native_on_a_machine_with_nothing_bigger_still_picks_1024_768) {
    boot_options_t options;
    parse("video=native", &options);
    uint32_t width = 0, height = 0;
    CHECK_EQ(best_mode(&options, qemu_modes, 3, &width, &height), 1);
    CHECK_EQ(width, 1024u);
    CHECK_EQ(height, 768u);
}

TEST(boot_options, an_exact_mode_is_taken_and_a_machine_without_it_gets_nothing) {
    boot_options_t options;
    parse("video=1280x1024\n", &options);
    CHECK_EQ(options.video_selection, (uint32_t)BOOT_VIDEO_EXACT);
    CHECK_EQ(options.video_width, 1280u);
    CHECK_EQ(options.video_height, 1024u);

    uint32_t width = 0, height = 0;
    CHECK_EQ(best_mode(&options, thinkpad_modes, 5, &width, &height), 1);
    CHECK_EQ(width, 1280u);

    CHECK_EQ(best_mode(&options, qemu_modes, 3, &width, &height), 0);
}

TEST(boot_options, the_interrupt_controller_can_be_named_either_way) {
    boot_options_t options;
    parse("interrupts=ioapic\n", &options);
    CHECK_EQ(options.interrupts, (uint32_t)BOOT_INTERRUPTS_IOAPIC);
    parse("interrupts=PIC\n", &options);
    CHECK_EQ(options.interrupts, (uint32_t)BOOT_INTERRUPTS_PIC);
    CHECK_EQ(options.unknown_keys, 0u);
}

TEST(boot_options, comments_blank_lines_crlf_and_spaces_around_the_equals) {
    boot_options_t options;
    parse("# a machine's own settings\r\n"
          "\r\n"
          "   video = native   # the panel\r\n"
          "\tinterrupts=ioapic\r\n"
          "cpus = 8\r\n",
          &options);
    CHECK_EQ(options.video_selection, (uint32_t)BOOT_VIDEO_LARGEST);
    CHECK_EQ(options.interrupts, (uint32_t)BOOT_INTERRUPTS_IOAPIC);
    CHECK_EQ(options.cpu_limit, 8u);
    CHECK_EQ(options.unknown_keys, 0u);
}

TEST(boot_options, a_line_it_does_not_understand_is_counted_and_changes_nothing) {
    boot_options_t options;
    parse("wallpaper=blue\nvideo=native\nvideo=1024\ninterrupts=quantum\ncpus=x\nnonsense\n", &options);
    CHECK_EQ(options.video_selection, (uint32_t)BOOT_VIDEO_LARGEST);
    CHECK_EQ(options.interrupts, (uint32_t)BOOT_INTERRUPTS_UNSET);
    CHECK_EQ(options.cpu_limit, 0u);
    CHECK_EQ(options.unknown_keys, 5u);
}

TEST(boot_options, a_file_with_no_final_newline_still_has_its_last_line_read) {
    boot_options_t options;
    parse("cpus=4", &options);
    CHECK_EQ(options.cpu_limit, 4u);
    CHECK_EQ(options.unknown_keys, 0u);
}

TEST(boot_options, garbage_bytes_are_survivable_rather_than_fatal) {
    boot_options_t options;
    boot_options_defaults(&options);
    char noise[257];
    for (int i = 0; i < 256; i++) {
        noise[i] = (char)i;
    }
    noise[256] = 0;
    boot_options_parse(noise, 256, &options);
    CHECK_EQ(options.video_selection, (uint32_t)BOOT_VIDEO_PREFER_1024_768);
    CHECK_EQ(options.interrupts, (uint32_t)BOOT_INTERRUPTS_UNSET);
}

TEST(boot_options, a_zero_sized_mode_is_never_chosen) {
    boot_options_t options;
    boot_options_defaults(&options);
    CHECK(boot_options_video_score(&options, 0, 768) < 0);
    CHECK(boot_options_video_score(&options, 1024, 0) < 0);
}

TEST(boot_options, a_loader_that_hands_nothing_over_leaves_the_built_in_defaults) {
    boot_options_t stale;
    parse("interrupts=ioapic\ncpus=4\n", &stale);
    boot_options_set_active(&stale);
    CHECK_EQ(boot_options_active()->interrupts, (uint32_t)BOOT_INTERRUPTS_IOAPIC);

    boot_options_set_active(NULL);
    CHECK_EQ(boot_options_active()->interrupts, (uint32_t)BOOT_INTERRUPTS_UNSET);
    CHECK_EQ(boot_options_active()->cpu_limit, 0u);
}

TEST(boot_options, a_handoff_with_the_wrong_magic_is_refused_rather_than_believed) {
    boot_options_t forged;
    parse("cpus=64\n", &forged);
    forged.magic = 0;
    boot_options_set_active(&forged);
    CHECK_EQ(boot_options_active()->cpu_limit, 0u);

    forged.magic = BOOT_OPTIONS_MAGIC;
    boot_options_set_active(&forged);
    CHECK_EQ(boot_options_active()->cpu_limit, 64u);
    boot_options_set_active(NULL);
}

TEST(boot_options, qemu_keeps_the_8259_and_a_machine_with_no_fw_cfg_takes_the_io_apic) {
    boot_options_t options;
    boot_options_defaults(&options);

    CHECK_EQ(boot_options_use_ioapic(&options, 1, 0), 0);
    CHECK_EQ(boot_options_use_ioapic(&options, 1, 1), 1);
    CHECK_EQ(boot_options_use_ioapic(&options, 0, 0), 1);
}

TEST(boot_options, the_config_file_overrides_both_of_those) {
    boot_options_t options;
    parse("interrupts=pic\n", &options);
    CHECK_EQ(boot_options_use_ioapic(&options, 0, 0), 0);
    CHECK_EQ(boot_options_use_ioapic(&options, 1, 1), 0);

    parse("interrupts=ioapic\n", &options);
    CHECK_EQ(boot_options_use_ioapic(&options, 1, 0), 1);
}

TEST(boot_options, video_firmware_names_no_mode_of_its_own) {
    boot_options_t options;
    parse("video=firmware\n", &options);
    CHECK_EQ(options.video_selection, (uint32_t)BOOT_VIDEO_FIRMWARE);
    CHECK_EQ(options.unknown_keys, 0u);

    uint32_t width = 0, height = 0;
    CHECK_EQ(best_mode(&options, thinkpad_modes, 5, &width, &height), 0);
}

TEST(boot_options, the_desktop_scale_is_auto_unless_a_line_pins_it_to_one_or_two) {
    boot_options_t options;
    parse("video=native\n", &options);
    CHECK_EQ(options.display_scale, 0u);
    parse("scale=2\n", &options);
    CHECK_EQ(options.display_scale, 2u);
    CHECK_EQ(options.unknown_keys, 0u);
    parse("scale = 1\n", &options);
    CHECK_EQ(options.display_scale, 1u);
    parse("scale=2\nscale=auto\n", &options);
    CHECK_EQ(options.display_scale, 0u);
    CHECK_EQ(options.unknown_keys, 0u);
}

TEST(boot_options, a_scale_it_cannot_draw_is_refused_and_counted) {
    boot_options_t options;
    parse("scale=3\nscale=0\nscale=big\nscale=\n", &options);
    CHECK_EQ(options.display_scale, 0u);
    CHECK_EQ(options.unknown_keys, 4u);
}

TEST(boot_options, the_scale_crosses_the_handoff_and_a_forged_one_does_not) {
    boot_options_t handoff;
    parse("scale=1\n", &handoff);
    boot_options_set_active(&handoff);
    CHECK_EQ(boot_options_active()->display_scale, 1u);
    handoff.display_scale = 7;
    boot_options_set_active(&handoff);
    CHECK_EQ(boot_options_active()->display_scale, 0u);
}

TEST(boot_options, config_names_the_file_s_own_sectors_and_crosses_the_handoff) {
    boot_options_t options;
    parse("video=native\nconfig=4309700+4\n", &options);
    CHECK_EQ(options.config_lba, 4309700u);
    CHECK_EQ(options.config_sectors, 4u);
    CHECK_EQ(options.unknown_keys, 0u);
    boot_options_set_active(&options);
    CHECK_EQ(boot_options_active()->config_lba, 4309700u);
    CHECK_EQ(boot_options_active()->config_sectors, 4u);
    options.config_sectors = 40;
    boot_options_set_active(&options);
    CHECK_EQ(boot_options_active()->config_sectors, 0u);
}

TEST(boot_options, a_config_extent_that_is_not_one_is_refused_and_counted) {
    boot_options_t options;
    parse("config=0+4\nconfig=12\nconfig=12+0\nconfig=12+9\nconfig=12+4x\n", &options);
    CHECK_EQ(options.config_lba, 0u);
    CHECK_EQ(options.config_sectors, 0u);
    CHECK_EQ(options.unknown_keys, 5u);
}
