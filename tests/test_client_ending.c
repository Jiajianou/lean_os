#include "check.h"

#include "client_ending.h"

static long exited_with(int status) { return (long)((status & 0xFF) << 8); }
static long killed_by(int signal_number) { return (long)(signal_number & 0x7F); }

TEST(client_ending, a_program_still_running_keeps_its_window) {
    CHECK_EQ(client_ending(-OS_ERROR_AGAIN, 0), CLIENT_RUNNING);
    CHECK_EQ(client_ending(-OS_ERROR_AGAIN, 1), CLIENT_RUNNING);
}

TEST(client_ending, the_README_editor_quitting_from_its_own_menu_is_not_a_crash) {
    CHECK_EQ(client_ending(exited_with(0), 1), CLIENT_GONE);
    CHECK_NE(client_ending(exited_with(1), 1), CLIENT_CRASHED);
}

TEST(client_ending, a_program_that_exits_0_unasked_leaves_its_picture_behind) {
    CHECK_EQ(client_ending(exited_with(0), 0), CLIENT_KEEP_WINDOW);
}

TEST(client_ending, a_non_zero_exit_nobody_asked_for_is_a_failure_and_not_a_crash) {
    CHECK_EQ(client_ending(exited_with(1), 0), CLIENT_FAILED);
    CHECK_EQ(client_ending(exited_with(139), 0), CLIENT_FAILED);
}

TEST(client_ending, every_fault_is_a_crash) {
    int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTRAP};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        CHECK_EQ(client_ending(killed_by(faults[i]), 0), CLIENT_CRASHED);
    }
}

TEST(client_ending, a_signal_somebody_sent_on_purpose_is_not_a_crash) {
    CHECK_EQ(client_ending(killed_by(SIGTERM), 0), CLIENT_GONE);
    CHECK_EQ(client_ending(killed_by(SIGKILL), 0), CLIENT_GONE);
    CHECK_EQ(client_ending(killed_by(SIGINT), 0), CLIENT_GONE);
}

TEST(client_ending, a_close_the_compositor_asked_for_is_never_reported) {
    CHECK_EQ(client_ending(killed_by(SIGSEGV), 1), CLIENT_GONE);
    CHECK_EQ(client_ending(exited_with(2), 1), CLIENT_GONE);
}

TEST(client_ending, a_program_nothing_is_known_about_any_more_goes_quietly) {
    CHECK_EQ(client_ending(-OS_ERROR_NOENT, 0), CLIENT_GONE);
}
