#include "check.h"

#include "process/resource_limits.h"
#include "scheduler/scheduler.h"
#include "process.h"

/* The limits this machine reports, graded against the things that enforce
   them rather than against a copy of the same numbers. If MAX_TASKS moves and
   getrlimit(RLIMIT_NPROC) does not, this fails - which is the whole reason
   the table stopped being a list of constants in the C library. */

TEST(resource_limits, the_numbers_are_the_things_that_enforce_them) {
    os_rlimit_t limit;

    CHECK_EQ(resource_limit_for(OS_RLIMIT_NOFILE, &limit), 0);
    CHECK_EQ((int)limit.current, MAX_FILE_DESCRIPTORS);
    CHECK_EQ((int)limit.maximum, MAX_FILE_DESCRIPTORS);

    CHECK_EQ(resource_limit_for(OS_RLIMIT_NPROC, &limit), 0);
    CHECK_EQ((int)limit.current, MAX_TASKS);

    CHECK_EQ(resource_limit_for(OS_RLIMIT_STACK, &limit), 0);
    CHECK(limit.current == OS_MAIN_STACK_MAX_BYTES);
}

/* Zero rather than absent. A machine with two priority classes cannot lower
   a nice value at all, and saying so is what base/posix/can_lower_nice_to.cc
   reads; there is nowhere for a core dump to go either. */
TEST(resource_limits, what_this_machine_cannot_do_is_zero) {
    os_rlimit_t limit;
    const uint64_t none[] = {OS_RLIMIT_CORE, OS_RLIMIT_NICE, OS_RLIMIT_RTPRIO};
    for (unsigned i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        CHECK_EQ(resource_limit_for(none[i], &limit), 0);
        CHECK(limit.current == 0);
        CHECK(limit.maximum == 0);
    }
}

TEST(resource_limits, every_resource_the_abi_names_has_an_answer) {
    os_rlimit_t limit;
    for (uint64_t resource = 0; resource < OS_RLIM_COUNT; resource++) {
        CHECK_EQ(resource_limit_for(resource, &limit), 0);
        /* Nothing here is soft: there is no privilege to raise a limit with,
           so a current below a maximum would describe a machine that could
           be asked for more and cannot be. */
        CHECK(limit.current == limit.maximum);
    }
    CHECK_EQ(resource_limit_for(OS_RLIM_COUNT, &limit), -1);
    CHECK_EQ(resource_limit_for(0xFFFFFFFFULL, &limit), -1);
}

TEST(resource_limits, a_null_destination_is_refused_rather_than_written) {
    CHECK_EQ(resource_limit_for(OS_RLIMIT_NOFILE, 0), -1);
}
