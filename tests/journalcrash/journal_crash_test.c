#include "check.h"
#include "drivers/block_device.h"
#include "file_system/leanfs.h"
#include "file_system/leanfs_format.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* M194. leanfs over the real block layer and its journal, on a RAM disk that
   can lose power in the middle of any sector of any write. A workload records
   the filesystem's state at every commit that completed; each run cuts the
   power at a different point, boots again, and requires the filesystem it
   finds to BE one of those states - the last commit that finished, or the one
   that was being written when the power went - and the mount check to find
   nothing to repair. Consistent is not enough on its own: a journal that lost
   a committed transaction would leave a consistent disk that is wrong. */

void fake_backend_reset(uint32_t sectors);
uint8_t *fake_backend_sector(uint32_t lba);
void fake_backend_power_cut_after_sectors(int64_t sectors);
int fake_backend_power_was_cut(void);
uint64_t fake_backend_sectors_written(void);
void kernel_log_capture_reset(void);
int kernel_log_capture_contains(const char *needle);

#define DISK_SECTORS (LEANFS_JOURNAL_START_LBA + LEANFS_JOURNAL_BLOCKS * LEANFS_SECTORS_PER_BLOCK + 64)
#define NAMES 10
#define MAX_FILE 160000
#define MAX_STATES 128

typedef struct {
    int exists[NAMES];
    uint32_t size[NAMES];
    uint32_t hash[NAMES];
} fs_state_t;

static fs_state_t states[MAX_STATES];
static int state_count;
static int committing_state;
static uint8_t file_buffer[MAX_FILE];

static void name_of(int i, char *out) {
    snprintf(out, 32, "/d/f%d", i);
}

static void capture(fs_state_t *out) {
    for (int i = 0; i < NAMES; i++) {
        char name[32];
        name_of(i, name);
        leanfs_stat_t st;
        out->exists[i] = leanfs_stat(name, &st) == 0;
        out->size[i] = 0;
        out->hash[i] = 0;
        if (out->exists[i]) {
            int64_t n = leanfs_read(name, file_buffer, sizeof(file_buffer));
            out->size[i] = n < 0 ? 0xFFFFFFFFu : (uint32_t)n;
            out->hash[i] = leanfs_fnv1a(LEANFS_FNV1A_INIT, file_buffer,
                                        n > 0 ? (size_t)n : 0);
        }
    }
}

static int same_state(const fs_state_t *a, const fs_state_t *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

static void boot(void) {
    block_device_journal_forget();
    block_device_init();
    kernel_log_capture_reset();
    leanfs_init();
}

static void fresh_disk(void) {
    fake_backend_power_cut_after_sectors(-1);
    fake_backend_reset(DISK_SECTORS);
    uint8_t *mbr = fake_backend_sector(0);
    memcpy(mbr + LEANFS_DISK_LABEL_OFFSET, "LEAN_OS1", LEANFS_DISK_LABEL_LENGTH);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    boot();
    leanfs_sync();
}

static void commit_point(int use_fsync) {
    leanfs_transaction_boundary();
    if (state_count >= MAX_STATES || fake_backend_power_was_cut()) {
        return;
    }
    committing_state = state_count;
    capture(&states[state_count]);
    int failed = use_fsync ? (leanfs_sync(), fake_backend_power_was_cut()) : block_device_commit() != 0;
    if (!failed && !fake_backend_power_was_cut()) {
        state_count++;
    }
}

static void fill_pattern(uint8_t *out, uint32_t length, uint32_t seed) {
    for (uint32_t i = 0; i < length; i++) {
        out[i] = (uint8_t)(seed * 131u + i * 7u + (i >> 9));
    }
}

static void workload(void) {
    static uint8_t data[MAX_FILE];
    leanfs_mkdir("/d");
    commit_point(0);
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < NAMES; i++) {
            char name[32];
            name_of(i, name);
            uint32_t length = (uint32_t)(4096 * (i + 1) + 1234 * round + (i % 3) * 70000);
            if (length > MAX_FILE) {
                length = MAX_FILE;
            }
            fill_pattern(data, length, (uint32_t)(round * 100 + i));
            switch ((i + round) % 5) {
            case 0:
                leanfs_write(name, data, length);
                break;
            case 1: {
                int h = leanfs_open(name, LEANFS_OPEN_CREATE);
                if (h >= 0) {
                    leanfs_handle_write(h, data, length / 2, 0);
                    leanfs_handle_write(h, data + length / 2, length - length / 2, length / 2);
                }
                break;
            }
            case 2: {
                char other[32];
                name_of((i + 1) % NAMES, other);
                leanfs_write(name, data, length);
                leanfs_rename(name, other);
                break;
            }
            case 3:
                leanfs_unlink(name);
                break;
            default:
                leanfs_write(name, data, length / 3);
                break;
            }
            leanfs_transaction_boundary();
            if (i % 3 == 2) {
                commit_point(i % 2);
            }
            if (i == 7) {
                commit_point(0);
                block_device_checkpoint();
            }
        }
        commit_point(1);
    }
}

TEST(journal_crash, a_run_with_no_power_cut_ends_where_it_was_left) {
    fresh_disk();
    state_count = 0;
    fake_backend_power_cut_after_sectors(1ll << 40);
    workload();
    REQUIRE(state_count > 10);
    fs_state_t last = states[state_count - 1];
    boot();
    fs_state_t found;
    capture(&found);
    CHECK(same_state(&found, &last));
    CHECK(!kernel_log_capture_contains("reclaimed, 1") && !kernel_log_capture_contains("were in use but marked free - bitmap"));
}

static void sweep(int cuts);

/* Eight cuts in the fast tier; the forty-eight-cut sweep that the mutations
   were graded against is the --slow one. */
TEST(journal_crash, eight_power_cuts_each_leave_a_state_that_was_committed) {
    sweep(8);
}

TEST(journal_crash, slow_forty_eight_power_cuts_each_leave_a_state_that_was_committed) {
    sweep(48);
}

static void sweep(int cuts) {
    fresh_disk();
    state_count = 0;
    fake_backend_power_cut_after_sectors(1ll << 40);
    workload();
    uint64_t total = fake_backend_sectors_written();
    REQUIRE(total > 1000);

    const int CUTS = cuts;
    int replays = 0;
    for (int c = 0; c < CUTS; c++) {
        /* Evenly across the run, and each one off the even grid by a prime
           number of sectors, so cuts land inside commands as well as between
           them. */
        int64_t cut = (int64_t)(total * (uint64_t)c / CUTS) + (c * 37) % 13;
        fresh_disk();
        state_count = 0;
        committing_state = -1;
        fake_backend_power_cut_after_sectors(cut);
        workload();
        int finished = state_count;
        int in_flight = committing_state;

        fake_backend_power_cut_after_sectors(-1);
        boot();
        if (kernel_log_capture_contains("replayed")) {
            replays++;
        }
        fs_state_t found;
        capture(&found);

        int matches = 0;
        if (finished > 0 && same_state(&found, &states[finished - 1])) {
            matches = 1;
        }
        if (in_flight >= finished && in_flight >= 0 && same_state(&found, &states[in_flight])) {
            matches = 1;
        }
        if (finished == 0) {
            fs_state_t empty;
            memset(&empty, 0, sizeof(empty));
            if (same_state(&found, &empty)) {
                matches = 1;
            }
        }
        if (!matches) {
            printf("    cut at sector %lld of %llu: the disk holds a state no commit left "
                   "(%d commits had finished)\n",
                   (long long)cut, (unsigned long long)total, finished);
        }
        CHECK(matches);
        CHECK(!kernel_log_capture_contains("were in use but marked free - bitmap"));
    }
    /* A sweep in which nothing was ever replayed would pass with replay
       broken. */
    CHECK(replays > 0);
}
