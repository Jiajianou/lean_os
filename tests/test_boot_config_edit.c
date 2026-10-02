#include <string.h>

#include "check.h"

#include "../kernel/boot/boot_config_edit.c"

#define AREA 256

static void make_area(char *area, const char *content) {
    memset(area, '\n', AREA);
    memcpy(area, content, strlen(content));
}

static int contains(const char *area, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= AREA; i++) {
        if (memcmp(area + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char CONFIG[] = "# lean_os boot options - read by the loader\n"
                             "video=native\n"
                             "log=4309696+49152\n";

TEST(boot_config_edit, a_named_line_is_replaced_where_it_was) {
    char area[AREA];
    make_area(area, CONFIG);
    int length = boot_config_set(area, AREA, "video", "1920x1080");
    CHECK(length > 0);
    CHECK(memcmp(area, "# lean_os boot options - read by the loader\nvideo=1920x1080\nlog=4309696+49152\n",
                 (size_t)length) == 0);
    CHECK(!contains(area, "native"));
}

TEST(boot_config_edit, the_area_keeps_its_size_and_ends_in_padding) {
    char area[AREA + 1];
    area[AREA] = 'Z';
    make_area(area, CONFIG);
    int length = boot_config_set(area, AREA, "video", "1920x1080");
    for (int i = length; i < AREA; i++) {
        CHECK_EQ(area[i], '\n');
    }
    CHECK_EQ(area[AREA], 'Z');
    length = boot_config_set(area, AREA, "video", "native");
    CHECK(memcmp(area, CONFIG, strlen(CONFIG)) == 0);
    CHECK_EQ(length, (int)strlen(CONFIG));
}

TEST(boot_config_edit, a_missing_line_is_appended_after_the_content) {
    char area[AREA];
    make_area(area, "# lean_os boot options\ncpus=4\n");
    int length = boot_config_set(area, AREA, "video", "1280x800");
    CHECK_EQ(length, (int)strlen("# lean_os boot options\ncpus=4\nvideo=1280x800\n"));
    CHECK(memcmp(area, "# lean_os boot options\ncpus=4\nvideo=1280x800\n", (size_t)length) == 0);
}

TEST(boot_config_edit, a_key_inside_a_comment_or_a_longer_key_is_not_the_key) {
    char area[AREA];
    make_area(area, "# lean_os boot options\n# video=old\nvideos=2\n  Video = 800x600\n");
    int length = boot_config_set(area, AREA, "video", "native");
    CHECK(length > 0);
    CHECK(contains(area, "# video=old\n"));
    CHECK(contains(area, "videos=2\n"));
    CHECK(contains(area, "video=native\n"));
    CHECK(!contains(area, "800x600"));
}

TEST(boot_config_edit, a_null_value_removes_the_line) {
    char area[AREA];
    make_area(area, CONFIG);
    int length = boot_config_set(area, AREA, "video", 0);
    CHECK_EQ(length, (int)strlen("# lean_os boot options - read by the loader\nlog=4309696+49152\n"));
    CHECK(!contains(area, "video"));
    CHECK(contains(area, "log=4309696+49152\n"));
}

TEST(boot_config_edit, an_area_that_is_not_ours_is_refused_untouched) {
    char area[AREA];
    make_area(area, "LEAN_OS LOG AREA 1\nnext=0\n");
    char before[AREA];
    memcpy(before, area, AREA);
    CHECK_EQ(boot_config_set(area, AREA, "video", "native"), -1);
    CHECK(memcmp(area, before, AREA) == 0);
}

TEST(boot_config_edit, a_line_that_would_not_fit_is_refused_untouched) {
    char area[64];
    memset(area, '\n', sizeof(area));
    memcpy(area, "# lean_os boot options\nvideo=native\n", 36);
    char before[64];
    memcpy(before, area, sizeof(area));
    CHECK_EQ(boot_config_set(area, sizeof(area), "interrupts", "a-value-much-too-long-for-the-room"), -1);
    CHECK(memcmp(area, before, sizeof(area)) == 0);
    CHECK(boot_config_set(area, sizeof(area), "video", "1x1") > 0);
}

TEST(boot_config_edit, content_without_a_final_newline_gets_one_before_the_new_line) {
    char area[AREA];
    memset(area, '\0', AREA);
    memcpy(area, "# lean_os boot options\ncpus=2", 29);
    int length = boot_config_set(area, AREA, "video", "native");
    CHECK(memcmp(area, "# lean_os boot options\ncpus=2\nvideo=native\n", (size_t)length) == 0);
}
