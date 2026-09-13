#include "check.h"

#include "file_system/flock.h"

static void clean(void) {
    for (int pid = 1; pid <= 8; pid++) {
        flock_release_pid(pid);
    }
    CHECK_EQ(flock_count(), 0);
}

static int held_by(uint32_t ino, int asker, int type, uint64_t start, uint64_t length) {
    os_flock_t out;
    if (!flock_test(ino, asker, type, start, length, &out)) {
        return 0;
    }
    return out.pid;
}

TEST(flock, two_readers_share_and_a_writer_excludes) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 0, 100), 0);
    CHECK_EQ(flock_set(7, 2, OS_FLOCK_RD, 50, 100), 0);
    CHECK_EQ(flock_count(), 2);
    CHECK_EQ(flock_set(7, 3, OS_FLOCK_WR, 60, 10), FLOCK_CONFLICT);
    CHECK(held_by(7, 3, OS_FLOCK_WR, 60, 10) != 0);
    CHECK_EQ(flock_set(7, 3, OS_FLOCK_RD, 0, 150), 0);
    CHECK_EQ(flock_set(7, 4, OS_FLOCK_WR, 150, 10), 0);
    CHECK_EQ(flock_count(), 4);
    os_flock_t who;
    CHECK_EQ(flock_test(7, 1, OS_FLOCK_RD, 155, 1, &who), 1);
    CHECK_EQ(who.pid, 4);
    CHECK_EQ(who.type, OS_FLOCK_WR);
    CHECK_EQ(who.start, 150);
    CHECK_EQ(who.length, 10);
    CHECK_EQ(who.whence, 0);
    clean();
}

TEST(flock, no_conflict_is_reported_as_unlocked_with_nothing_in_it) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 0, 100), 0);
    os_flock_t out;
    out.type = 99;
    out.whence = 99;
    out.pid = 99;
    out.start = 99;
    out.length = 99;
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_RD, 0, 100, &out), 0);
    CHECK_EQ(out.type, OS_FLOCK_UNLCK);
    CHECK_EQ(out.whence, 0);
    CHECK_EQ(out.pid, 0);
    CHECK_EQ(out.start, 0);
    CHECK_EQ(out.length, 0);
    CHECK_EQ(flock_test(7, 1, OS_FLOCK_WR, 0, 100, &out), 0);
    CHECK_EQ(out.type, OS_FLOCK_UNLCK);
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 100, 1, &out), 0);
    CHECK_EQ(out.type, OS_FLOCK_UNLCK);
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 99, 1, &out), 1);
    CHECK_EQ(out.type, OS_FLOCK_RD);
    clean();
}

TEST(flock, unlocking_exactly_the_locked_range_leaves_nothing) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 10, 90), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 10, 90), 0);
    CHECK_EQ(flock_count(), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 10, 90), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 10, 50), 0);
    CHECK_EQ(flock_count(), 1);
    os_flock_t who;
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 0, 0, &who), 1);
    CHECK_EQ(who.start, 60);
    CHECK_EQ(who.length, 40);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 80, 20), 0);
    CHECK_EQ(flock_count(), 1);
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 0, 0, &who), 1);
    CHECK_EQ(who.start, 60);
    CHECK_EQ(who.length, 20);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 500, 0), 0);
    CHECK_EQ(flock_count(), 2);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 500, 0), 0);
    CHECK_EQ(flock_count(), 1);
    clean();
}

TEST(flock, a_process_never_conflicts_with_itself_and_upgrades_in_place) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 0, 100), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 40, 20), 0);
    CHECK_EQ(flock_count(), 3);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 40, 1), 1);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 39, 1), 0);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 60, 1), 0);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_WR, 99, 1), 1);
    CHECK_EQ(held_by(8, 2, OS_FLOCK_WR, 40, 20), 0);
    clean();
}

TEST(flock, unlocking_the_middle_splits_and_unlocking_the_edges_trims) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 0, 100), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 40, 20), 0);
    CHECK_EQ(flock_count(), 2);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_WR, 39, 1), 1);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_WR, 40, 20), 0);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_WR, 60, 1), 1);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 0, 10), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 90, 10), 0);
    CHECK_EQ(flock_count(), 2);
    os_flock_t who;
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 0, 100, &who), 1);
    CHECK_EQ(who.start, 10);
    CHECK_EQ(who.length, 30);
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 60, 100, &who), 1);
    CHECK_EQ(who.start, 60);
    CHECK_EQ(who.length, 30);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 0, 0), 0);
    CHECK_EQ(flock_count(), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 0, 0), 0);
    clean();
}

TEST(flock, length_zero_means_to_end_of_file) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 1000, 0), 0);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 999, 1), 0);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 1000, 1), 1);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 1u << 30, 1), 1);
    os_flock_t who;
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_RD, 5000, 0, &who), 1);
    CHECK_EQ(who.start, 1000);
    CHECK_EQ(who.length, 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 1073741824, 1), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 1073741825, 0), 0);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 1073741824, 1), 0);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 1073741825, 1), 1);
    clean();
}

TEST(flock, a_refused_request_changes_nothing) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 0, 10), 0);
    CHECK_EQ(flock_set(7, 2, OS_FLOCK_RD, 0, 50), FLOCK_CONFLICT);
    CHECK_EQ(flock_count(), 1);
    CHECK_EQ(held_by(7, 3, OS_FLOCK_WR, 20, 1), 0);
    os_flock_t who;
    CHECK_EQ(flock_test(7, 3, OS_FLOCK_WR, 0, 1, &who), 1);
    CHECK_EQ(who.pid, 1);
    clean();
}

TEST(flock, touching_locks_of_one_type_become_one_entry) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 0, 10), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 10, 10), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 30, 10), 0);
    CHECK_EQ(flock_count(), 2);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 20, 10), 0);
    CHECK_EQ(flock_count(), 1);
    os_flock_t who;
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 25, 1, &who), 1);
    CHECK_EQ(who.start, 0);
    CHECK_EQ(who.length, 40);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 40, 10), 0);
    CHECK_EQ(flock_count(), 2);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 50, 0), 0);
    CHECK_EQ(flock_count(), 2);
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_RD, 45, 1, &who), 1);
    CHECK_EQ(who.start, 40);
    CHECK_EQ(who.length, 0);
    clean();
}

TEST(flock, the_close_rule_releases_one_file_and_exit_releases_all) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 0, 10), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 20, 10), 0);
    CHECK_EQ(flock_set(8, 1, OS_FLOCK_WR, 0, 10), 0);
    CHECK_EQ(flock_set(7, 2, OS_FLOCK_WR, 50, 10), 0);
    CHECK_EQ(flock_count(), 4);
    CHECK_EQ(flock_release_file(7, 1), 2);
    CHECK_EQ(flock_count(), 2);
    CHECK_EQ(held_by(7, 3, OS_FLOCK_WR, 0, 30), 0);
    CHECK_EQ(held_by(8, 3, OS_FLOCK_WR, 0, 1), 1);
    CHECK_EQ(held_by(7, 3, OS_FLOCK_WR, 50, 1), 2);
    CHECK_EQ(flock_release_file(7, 1), 0);
    CHECK_EQ(flock_release_pid(1), 1);
    CHECK_EQ(flock_release_pid(2), 1);
    CHECK_EQ(flock_count(), 0);
}

TEST(flock, a_full_table_refuses_without_losing_what_it_holds) {
    clean();
    int n = 0;
    while (n <= FLOCK_MAX && flock_set(9, 1, OS_FLOCK_WR, (uint64_t)n * 2, 1) == 0) {
        n++;
    }
    CHECK_EQ(n, FLOCK_MAX);
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_set(9, 2, OS_FLOCK_WR, 1000000, 1), FLOCK_FULL);
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 0, 0), 0);
    CHECK_EQ(flock_count(), 0);
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_WR, 0, 100), 0);
    n = 1;
    while (n <= FLOCK_MAX && flock_set(9, 1, OS_FLOCK_WR, 1000 + (uint64_t)n * 2, 1) == 0) {
        n++;
    }
    CHECK_EQ(n, FLOCK_MAX);
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 40, 20), FLOCK_FULL);
    os_flock_t who;
    CHECK_EQ(flock_test(9, 2, OS_FLOCK_RD, 40, 20, &who), 1);
    CHECK_EQ(who.start, 0);
    CHECK_EQ(who.length, 100);
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 0, 10), 0);
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 90, 10), 0);
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 10, 10), 0);
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_test(9, 2, OS_FLOCK_RD, 0, 100, &who), 1);
    CHECK_EQ(who.start, 20);
    CHECK_EQ(who.length, 70);
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 50, 500), 0);
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_test(9, 2, OS_FLOCK_RD, 0, 100, &who), 1);
    CHECK_EQ(who.start, 20);
    CHECK_EQ(who.length, 30);
    CHECK_EQ(flock_release_pid(1), FLOCK_MAX);
    CHECK_EQ(flock_set(9, 2, OS_FLOCK_WR, 1000000, 1), 0);
    clean();
}
