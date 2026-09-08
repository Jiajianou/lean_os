/* tests/test_flock.c - M100: the record-lock table, off the machine.
 *
 * kernel/fs/flock.c is range arithmetic over a fixed table: does this
 * request overlap that lock, which of a process's own locks does a new
 * one replace, what is left when the middle is unlocked, when do two
 * touching locks become one. Every one of those is a boundary question,
 * and boundary questions are what a boot self-test cannot reach - sqlite
 * on the machine takes three locks and releases them, and would pass
 * against a table that got every split wrong.
 *
 * What is asserted is POSIX's model, not this implementation's habits:
 * two readers coexist, a writer excludes, a process never conflicts
 * with itself, an open-ended lock reaches every later byte, a refused
 * request changes nothing, and the close rule releases exactly what it
 * should and nothing else.
 */
#include "check.h"

#include "fs/flock.h"

/* The table is file-static and has no reset, like the pty table - so
 * every test starts by releasing the pids it will use and ends the same
 * way, which also makes "release actually frees the entry" a property
 * every test depends on. */
static void clean(void) {
    for (int pid = 1; pid <= 8; pid++) {
        flock_release_pid(pid);
    }
    CHECK_EQ(flock_count(), 0);
}

static int held_by(uint32_t ino, int asker, int type, uint64_t start, uint64_t len) {
    os_flock_t out;
    if (!flock_test(ino, asker, type, start, len, &out)) {
        return 0;
    }
    return out.pid;
}

TEST(flock, two_readers_share_and_a_writer_excludes) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 0, 100), 0);
    CHECK_EQ(flock_set(7, 2, OS_FLOCK_RD, 50, 100), 0);
    CHECK_EQ(flock_count(), 2);
    /* A writer over the shared bytes is refused by whichever reader the
     * table finds first - what matters is that it is a reader, not pid 3. */
    CHECK_EQ(flock_set(7, 3, OS_FLOCK_WR, 60, 10), FLOCK_CONFLICT);
    CHECK(held_by(7, 3, OS_FLOCK_WR, 60, 10) != 0);
    /* A third reader is welcome, and a writer outside both ranges is too. */
    CHECK_EQ(flock_set(7, 3, OS_FLOCK_RD, 0, 150), 0);
    CHECK_EQ(flock_set(7, 4, OS_FLOCK_WR, 150, 10), 0);
    CHECK_EQ(flock_count(), 4);
    /* Now a reader over the writer's bytes is refused, and F_GETLK says
     * by whom and over what. */
    os_flock_t who;
    CHECK_EQ(flock_test(7, 1, OS_FLOCK_RD, 155, 1, &who), 1);
    CHECK_EQ(who.pid, 4);
    CHECK_EQ(who.type, OS_FLOCK_WR);
    CHECK_EQ(who.start, 150);
    CHECK_EQ(who.len, 10);
    CHECK_EQ(who.whence, 0);
    clean();
}

TEST(flock, no_conflict_is_reported_as_unlocked_with_nothing_in_it) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 0, 100), 0);
    /* F_GETLK's answer when nothing is in the way is F_UNLCK - and the
     * rest of the struct is zeroed rather than left holding whatever the
     * caller passed in, because a program that prints l_pid on "unlocked"
     * would otherwise print its own question back. Filled with noise
     * first so that "unchanged" cannot pass for "zeroed". */
    os_flock_t out;
    out.type = 99;
    out.whence = 99;
    out.pid = 99;
    out.start = 99;
    out.len = 99;
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_RD, 0, 100, &out), 0); /* a reader beside a reader */
    CHECK_EQ(out.type, OS_FLOCK_UNLCK);
    CHECK_EQ(out.whence, 0);
    CHECK_EQ(out.pid, 0);
    CHECK_EQ(out.start, 0);
    CHECK_EQ(out.len, 0);
    CHECK_EQ(flock_test(7, 1, OS_FLOCK_WR, 0, 100, &out), 0); /* the holder itself */
    CHECK_EQ(out.type, OS_FLOCK_UNLCK);
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 100, 1, &out), 0);  /* the byte after */
    CHECK_EQ(out.type, OS_FLOCK_UNLCK);
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 99, 1, &out), 1);   /* the last byte in */
    CHECK_EQ(out.type, OS_FLOCK_RD);
    clean();
}

TEST(flock, unlocking_exactly_the_locked_range_leaves_nothing) {
    clean();
    /* The two boundaries at once: an unlock whose start IS the lock's
     * start and whose end IS the lock's end must free the entry, not
     * trim it to a zero-length lock that still counts and still answers
     * F_GETLK. Then each boundary alone. */
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 10, 90), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 10, 90), 0);
    CHECK_EQ(flock_count(), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 10, 90), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 10, 50), 0);  /* front, exactly at the start */
    CHECK_EQ(flock_count(), 1);
    os_flock_t who;
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 0, 0, &who), 1);
    CHECK_EQ(who.start, 60);
    CHECK_EQ(who.len, 40);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 80, 20), 0);  /* back, exactly at the end */
    CHECK_EQ(flock_count(), 1);
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 0, 0, &who), 1);
    CHECK_EQ(who.start, 60);
    CHECK_EQ(who.len, 20);
    /* An open-ended lock unlocked open-endedly from its own start. */
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 500, 0), 0);
    CHECK_EQ(flock_count(), 2);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 500, 0), 0);
    CHECK_EQ(flock_count(), 1);
    clean();
}

TEST(flock, a_process_never_conflicts_with_itself_and_upgrades_in_place) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 0, 100), 0);
    /* The same process asking for a write lock over part of its own read
     * lock is an upgrade, not a conflict - and afterwards the table holds
     * the read lock's two remnants and the write lock, three entries. */
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 40, 20), 0);
    CHECK_EQ(flock_count(), 3);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 40, 1), 1);  /* the write lock excludes a reader */
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 39, 1), 0);  /* the remnant before it is still shared */
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 60, 1), 0);  /* and the one after */
    CHECK_EQ(held_by(7, 2, OS_FLOCK_WR, 99, 1), 1);  /* but a writer is not */
    /* And a different file is a different world. */
    CHECK_EQ(held_by(8, 2, OS_FLOCK_WR, 40, 20), 0);
    clean();
}

TEST(flock, unlocking_the_middle_splits_and_unlocking_the_edges_trims) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 0, 100), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 40, 20), 0);
    CHECK_EQ(flock_count(), 2);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_WR, 39, 1), 1);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_WR, 40, 20), 0); /* the hole is free */
    CHECK_EQ(held_by(7, 2, OS_FLOCK_WR, 60, 1), 1);
    /* Trim the front of the first piece and the back of the second. */
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 0, 10), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 90, 10), 0);
    CHECK_EQ(flock_count(), 2);
    os_flock_t who;
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 0, 100, &who), 1);
    CHECK_EQ(who.start, 10);
    CHECK_EQ(who.len, 30);
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 60, 100, &who), 1);
    CHECK_EQ(who.start, 60);
    CHECK_EQ(who.len, 30);
    /* Unlocking a range that covers both drops both. */
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 0, 0), 0);
    CHECK_EQ(flock_count(), 0);
    /* Unlocking what is not locked is not an error - POSIX says so, and
     * sqlite's unlock path relies on it. */
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_UNLCK, 0, 0), 0);
    clean();
}

TEST(flock, len_zero_means_to_end_of_file) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 1000, 0), 0);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 999, 1), 0);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 1000, 1), 1);
    CHECK_EQ(held_by(7, 2, OS_FLOCK_RD, 1u << 30, 1), 1);
    /* An open-ended request against it, and its report of the holder's
     * range comes back open-ended too. */
    os_flock_t who;
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_RD, 5000, 0, &who), 1);
    CHECK_EQ(who.start, 1000);
    CHECK_EQ(who.len, 0);
    /* sqlite's own pattern: a shared lock on one byte and a write lock
     * from there to the end, the second taken by the same process. */
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
    /* Not even a partial grant of the bytes the holder did not have. */
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
    CHECK_EQ(flock_count(), 2); /* [0,20) and [30,40) */
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_RD, 20, 10), 0);
    CHECK_EQ(flock_count(), 1); /* the gap closed both sides: [0,40) */
    os_flock_t who;
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_WR, 25, 1, &who), 1);
    CHECK_EQ(who.start, 0);
    CHECK_EQ(who.len, 40);
    /* A different TYPE touching does not merge: it is a different lock. */
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 40, 10), 0);
    CHECK_EQ(flock_count(), 2);
    /* And an open-ended lock absorbs a touching one before it. */
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 50, 0), 0);
    CHECK_EQ(flock_count(), 2); /* [0,40) read, [40,EOF) write */
    CHECK_EQ(flock_test(7, 2, OS_FLOCK_RD, 45, 1, &who), 1);
    CHECK_EQ(who.start, 40);
    CHECK_EQ(who.len, 0);
    clean();
}

TEST(flock, the_close_rule_releases_one_file_and_exit_releases_all) {
    clean();
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 0, 10), 0);
    CHECK_EQ(flock_set(7, 1, OS_FLOCK_WR, 20, 10), 0);
    CHECK_EQ(flock_set(8, 1, OS_FLOCK_WR, 0, 10), 0);
    CHECK_EQ(flock_set(7, 2, OS_FLOCK_WR, 50, 10), 0);
    CHECK_EQ(flock_count(), 4);
    /* pid 1 closes a descriptor for inode 7: both of its locks there go,
     * its lock on inode 8 stays, pid 2's stays. */
    CHECK_EQ(flock_release_file(7, 1), 2);
    CHECK_EQ(flock_count(), 2);
    CHECK_EQ(held_by(7, 3, OS_FLOCK_WR, 0, 30), 0);
    CHECK_EQ(held_by(8, 3, OS_FLOCK_WR, 0, 1), 1);
    CHECK_EQ(held_by(7, 3, OS_FLOCK_WR, 50, 1), 2);
    /* Closing a file it holds nothing on releases nothing, and says so. */
    CHECK_EQ(flock_release_file(7, 1), 0);
    /* Exit. */
    CHECK_EQ(flock_release_pid(1), 1);
    CHECK_EQ(flock_release_pid(2), 1);
    CHECK_EQ(flock_count(), 0);
}

TEST(flock, a_full_table_refuses_without_losing_what_it_holds) {
    clean();
    /* Disjoint one-byte write locks from one pid, two bytes apart so
     * nothing merges, until the table is full. */
    /* Bounded at one past the table, not `while it succeeds`: under a
     * mutant that never refuses, an unbounded loop is a 90-second
     * timeout the harness counts as a kill, and a bounded one is an
     * assertion that says which line. (The first version of this file
     * was unbounded, and its timed-out mutants left orphaned test
     * binaries spinning on the host for forty minutes - long enough to
     * fail a graded boot that was running at the time.) */
    int n = 0;
    while (n <= FLOCK_MAX && flock_set(9, 1, OS_FLOCK_WR, (uint64_t)n * 2, 1) == 0) {
        n++;
    }
    CHECK_EQ(n, FLOCK_MAX);
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_set(9, 2, OS_FLOCK_WR, 1000000, 1), FLOCK_FULL);
    /* A split needs a free entry too: carving the middle out of a lock
     * would make two of it. Refused, and the lock is intact. */
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 0, 0), 0); /* all gone... */
    CHECK_EQ(flock_count(), 0);
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_WR, 0, 100), 0);
    n = 1;
    while (n <= FLOCK_MAX && flock_set(9, 1, OS_FLOCK_WR, 1000 + (uint64_t)n * 2, 1) == 0) {
        n++;
    }
    CHECK_EQ(n, FLOCK_MAX); /* one lock already held, so one fewer fits */
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 40, 20), FLOCK_FULL);
    os_flock_t who;
    CHECK_EQ(flock_test(9, 2, OS_FLOCK_RD, 40, 20, &who), 1);
    CHECK_EQ(who.start, 0);
    CHECK_EQ(who.len, 100);
    /* Trimming an edge needs no entry, and works even now - the front,
     * the back ending exactly where the lock ends, and a range that
     * starts exactly at the lock's start. Each of those is one
     * comparison away from being mistaken for a split. */
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 0, 10), 0);
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 90, 10), 0);
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 10, 10), 0);
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_test(9, 2, OS_FLOCK_RD, 0, 100, &who), 1);
    CHECK_EQ(who.start, 20);
    CHECK_EQ(who.len, 70);
    /* And one that reaches past the end is a trim of the back too -
     * stopping short of the one-byte locks that fill the rest of the
     * table from offset 1002, which are not the subject. */
    CHECK_EQ(flock_set(9, 1, OS_FLOCK_UNLCK, 50, 500), 0);
    CHECK_EQ(flock_count(), FLOCK_MAX);
    CHECK_EQ(flock_test(9, 2, OS_FLOCK_RD, 0, 100, &who), 1);
    CHECK_EQ(who.start, 20);
    CHECK_EQ(who.len, 30);
    /* And once something is released the table works again. */
    CHECK_EQ(flock_release_pid(1), FLOCK_MAX);
    CHECK_EQ(flock_set(9, 2, OS_FLOCK_WR, 1000000, 1), 0);
    clean();
}
