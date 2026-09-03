/* tests/test_sched.c - Q13
 *
 * The scheduler, off the machine.
 *
 * 2,157 lines, the most concurrency-sensitive code in this tree, and
 * until now zero host tests. Its bugs have historically presented as
 * "about one boot in ten hangs", which is the worst failure signature
 * there is and the one a deterministic test rules out completely.
 *
 * ---- what this tier can and cannot grade -------------------------------
 *
 * Stated up front, because a test file that is vague about this is worse
 * than none.
 *
 * It CAN grade every decision: who runs next, what a state transition
 * means, which task a signal lands on, whether a slot came back, what a
 * process group is. All of that is a pure function of a task table and
 * runs in microseconds.
 *
 * It CANNOT grade the switch. `context_switch` is x86 assembly that
 * swaps stacks, and tests/fakes/fake_arch.c replaces it with a counter -
 * so after `schedule()` picks a task, `current_task[cpu]` names it and
 * control returns to the caller anyway, because there is no second stack
 * to go to. The boot self-tests are what prove the switch; this file
 * proves the choice.
 *
 * ---- why the table is grown rather than reset --------------------------
 *
 * `sched_init` is called once for the whole binary and each test cleans
 * up its own tasks by reaping them, which is exactly what the machine
 * does. Zeroing the table between tests would need a reset function that
 * exists only for tests - and it would also hide the thing that reusing
 * slots exercises for free: M54's slot recycling, and the generation
 * counter that stops a recycled slot from answering to the dead task's
 * pid.
 */
#include "check.h"
#include "fakes/fakes.h"

#include "arch/x86_64/cpu.h" /* MAX_CPUS, MSR_FS_BASE - the two the switch touches */
#include "lib/spinlock.h"
#include "sched/sched.h"
#include "signal.h" /* system_api/include/signal.h */

#include <string.h>

/* Q13_QUANTUM is file-static in kernel/sched/sched.c, deliberately -
 * it is a scheduling constant rather than an interface. Repeated here, with
 * the assertion below that the two agree: a test that drove the scheduler
 * with the wrong quantum would silently measure a different fairness
 * property, which is the exact failure this tier is supposed to make
 * impossible. */
#define Q13_QUANTUM 2

/* ---- the harness ------------------------------------------------------
 *
 * Four helpers, shared with tests/test_pty.c (whose terminal signals a
 * real process group now). Deliberately not in a header: they are three
 * lines each and a header would make them look like an API.
 */

void q13_boot(void);
task_t *q13_spawn(const char *name);
void q13_kill(task_t *t);

static void q13_body(void *arg) {
    /* Never runs. A task in this tier is a table row: fake_arch's
     * context_switch does not transfer control, so no entry point is
     * ever entered. Said here rather than left as a puzzle. */
    (void)arg;
}

void q13_boot(void) {
    static int booted;
    if (booted) {
        return;
    }
    booted = 1;
    sched_init();
    fake_arch_set_cpu(0);
}

task_t *q13_spawn(const char *name) {
    return task_spawn(name, q13_body, (void *)0);
}

void q13_kill(task_t *t) {
    if (!t) {
        return;
    }
    /* Off the CPU first, and this is not bookkeeping - it is M106's rule
     * being obeyed by the test rather than asserted by it.
     * sched_reap_slot refuses to hand a kernel stack back while any CPU
     * is still standing on it, and on the machine the corpse walks
     * itself off through task_exit_with_code's own schedule(). Here the
     * test has to do that walk, because nothing else will. */
    fake_arch_set_cpu(0);
    if (sched_current() == t) {
        t->state = TASK_TERMINATED;
        fake_arch_stand_on(t->kernel_stack_top ? t->kernel_stack_top - 64 : 0);
        schedule();
    }
    /* Straight to TERMINATED rather than through task_exit_with_code,
     * which is noreturn and would take the test process with it: it ends
     * by calling schedule() and never coming back, and on this tier
     * "never coming back" means the harness never comes back either.
     * What task_exit_with_code does that matters here - releasing the fd
     * table and the address space - has its own test below, driven
     * through the paths that are not noreturn. */
    t->state = TASK_TERMINATED;
    sched_reap_slot(t);
}

/* Drive one timer tick on `cpu`, having first said where that CPU is
 * standing. M106's check panics unless the CPU is inside the outgoing
 * task's kernel stack, and on a host there is no honest default - see
 * fake_arch.c. One line, at every call, so the claim is explicit. */
static void q13_tick(int cpu) {
    fake_arch_set_cpu(cpu);
    task_t *cur = sched_current();
    fake_arch_stand_on(cur && cur->kernel_stack_top
                           ? cur->kernel_stack_top - 64
                           : 0);
    scheduler_tick_cpu(cpu);
}

/* ---- the table this file compiles against is the kernel's ------------- */

TEST(sched, the_constants_are_the_kernels_own) {
    /* tests/fakes shadows two headers for this build. If MAX_CPUS or
     * MAX_TASKS ever drifted between a shadow and the real thing, every
     * test in this file would be testing a different scheduler and all
     * of them would pass. This is the assertion that makes that drift
     * fail instead. */
    CHECK_EQ(MAX_TASKS, 128);
    CHECK_EQ(MAX_CPUS, 8);
    CHECK(MAX_FDS >= 128);
    /* And the quantum, which is file-static in sched.c and copied above.
     * Asserted by driving it: a single runnable task other than the one
     * on the CPU has to wait exactly Q13_QUANTUM ticks for its turn. */
    q13_boot();
    task_t *a = q13_spawn("q-a");
    task_t *b = q13_spawn("q-b");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    int placed = 0;
    for (int i = 0; i < 200 && !placed; i++) {
        q13_tick(0);
        placed = (sched_current() == a);
    }
    REQUIRE(placed);
    int ticks = 0;
    while (sched_current() == a && ticks < 64) {
        q13_tick(0);
        ticks++;
    }
    CHECK_EQ(ticks, Q13_QUANTUM);
    q13_kill(a);
    q13_kill(b);
}

/* ---- round-robin fairness, as a property ------------------------------ */

TEST(sched, every_runnable_task_gets_a_turn) {
    q13_boot();
    /* Six is enough to see rotation and small enough that the
     * distribution is readable when it fails. The property is asserted
     * at four counts below; this one is about the shape. */
    task_t *t[6];
    for (int i = 0; i < 6; i++) {
        t[i] = q13_spawn("rr");
        REQUIRE(t[i] != NULL);
    }

    int seen[6];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < 6 * Q13_QUANTUM * 4; i++) {
        q13_tick(0);
        task_t *cur = sched_current();
        for (int j = 0; j < 6; j++) {
            if (cur == t[j]) {
                seen[j]++;
            }
        }
    }
    for (int j = 0; j < 6; j++) {
        CHECK(seen[j] > 0);
    }
    for (int j = 0; j < 6; j++) {
        q13_kill(t[j]);
    }
}

TEST(sched, slow_shares_within_one_quantum_at_every_task_count) {
    q13_boot();
    /* The property M56 and M69 both leaned on and neither ever checked:
     * N runnable tasks over M quanta each get within one quantum of M/N.
     * Checked at every N from 1 to 32 rather than at one convenient
     * number - a round-robin that skips the last slot, or that favours
     * the task it started from, is right for some N and wrong for
     * others, and a single N is exactly the test that would miss it. */
    for (int n = 1; n <= 32; n++) {
        task_t *t[32];
        for (int i = 0; i < n; i++) {
            t[i] = q13_spawn("fair");
            REQUIRE(t[i] != NULL);
        }
        int quanta = 12;
        int seen[32];
        memset(seen, 0, sizeof(seen));
        for (int i = 0; i < n * quanta * Q13_QUANTUM; i++) {
            q13_tick(0);
            task_t *cur = sched_current();
            for (int j = 0; j < n; j++) {
                if (cur == t[j]) {
                    seen[j]++;
                }
            }
        }
        /* Compared against each OTHER rather than against a computed
         * share, and the reason is worth a line: the table also holds
         * task 0 - the boot task, which is `kernel` on the machine and
         * is runnable like anything else - so the absolute share
         * depends on a task this test did not create. What the
         * milestone actually asks is that no runnable task is starved
         * relative to its peers, and that is a comparison between the n
         * of them. */
        int lo = seen[0], hi = seen[0];
        for (int j = 1; j < n; j++) {
            if (seen[j] < lo) {
                lo = seen[j];
            }
            if (seen[j] > hi) {
                hi = seen[j];
            }
        }
        if (lo <= 0 || hi - lo > Q13_QUANTUM * 2) {
            test_fail(__FILE__, __LINE__,
                      "n=%d: the busiest task ran %d ticks and the quietest %d,"
                      " a spread of %d - more than one quantum apart",
                      n, hi, lo, hi - lo);
        }
        for (int j = 0; j < n; j++) {
            q13_kill(t[j]);
        }
    }
}

/* ---- the states, and what pick_next does with each --------------------- */

TEST(sched, a_blocked_task_is_not_runnable_and_a_wake_makes_it_so) {
    q13_boot();
    task_t *a = q13_spawn("a");
    task_t *b = q13_spawn("b");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);

    b->state = TASK_BLOCKED;
    int saw_b = 0;
    for (int i = 0; i < 40 * Q13_QUANTUM; i++) {
        q13_tick(0);
        if (sched_current() == b) {
            saw_b = 1;
        }
    }
    CHECK_EQ(saw_b, 0);

    /* And the wake is what undoes it - not a tick, not time. */
    sched_wake_task(b);
    CHECK_EQ(b->state, TASK_READY);
    for (int i = 0; i < 40 * Q13_QUANTUM && !saw_b; i++) {
        q13_tick(0);
        if (sched_current() == b) {
            saw_b = 1;
        }
    }
    CHECK_EQ(saw_b, 1);

    q13_kill(a);
    q13_kill(b);
}

TEST(sched, a_stopped_task_is_not_woken_by_anything_but_a_resume) {
    q13_boot();
    task_t *a = q13_spawn("a");
    task_t *s = q13_spawn("stopped");
    REQUIRE(a != NULL);
    REQUIRE(s != NULL);

    sched_raise_signal(s, SIGTSTP);
    CHECK_EQ(s->pending_stop, SIGTSTP);
    /* The stop is taken at a tick that finds it current - which is the
     * checkpoint M85's first attempt relied on and its second found was
     * not enough on its own. Here it is, working, in the one case it
     * covers: a task that is actually running. */
    for (int i = 0; i < 60 * Q13_QUANTUM && s->state != TASK_STOPPED; i++) {
        q13_tick(0);
    }
    CHECK_EQ(s->state, TASK_STOPPED);
    CHECK_EQ(s->pending_stop, 0);

    /* THE property, and the reason TASK_STOPPED is a state rather than a
     * flag on TASK_BLOCKED: every one of the wakes in this kernel has to
     * leave it alone, and sched_wake_task is the one they all go
     * through. */
    sched_wake_task(s);
    sched_wake_all(SCHED_POLL_CHAN);
    CHECK_EQ(s->state, TASK_STOPPED);
    for (int i = 0; i < 40 * Q13_QUANTUM; i++) {
        q13_tick(0);
        CHECK_NE((long long)(uintptr_t)sched_current(), (long long)(uintptr_t)s);
    }

    sched_resume_stopped(s);
    CHECK_EQ(s->state, TASK_READY);

    q13_kill(a);
    q13_kill(s);
}

TEST(sched, a_stopped_task_can_still_be_killed) {
    q13_boot();
    task_t *a = q13_spawn("a");
    task_t *s = q13_spawn("stopped");
    REQUIRE(a != NULL);
    REQUIRE(s != NULL);

    s->state = TASK_STOPPED;
    /* Nothing schedules a stopped task, so a SIGKILL that only set a
     * pending signal would be a kill that never happens - the one thing
     * SIGKILL is not allowed to be. sched_raise_signal has to resume it
     * first, and this is the assertion that says so. */
    sched_raise_signal(s, SIGKILL);
    CHECK_EQ(s->state, TASK_READY);
    CHECK_EQ(s->pending_signal, SIGKILL);

    s->pending_signal = 0;
    q13_kill(a);
    q13_kill(s);
}

TEST(sched, sigcont_resumes_a_task_that_ignores_it) {
    q13_boot();
    task_t *s = q13_spawn("ignorer");
    REQUIRE(s != NULL);
    s->sig_handler[SIGCONT] = SIG_IGN_ADDR;
    s->state = TASK_STOPPED;
    /* POSIX: the continue happens whatever the disposition, and only the
     * optional handler is subject to it. Otherwise a program could make
     * itself permanently unstoppable in the other direction - suspended
     * with no way back. */
    sched_raise_signal(s, SIGCONT);
    CHECK_EQ(s->state, TASK_READY);
    s->sig_handler[SIGCONT] = SIG_DFL_ADDR;
    q13_kill(s);
}

TEST(sched, a_pending_stop_is_cleared_by_sigcont_before_it_is_ever_taken) {
    q13_boot();
    task_t *a = q13_spawn("a");
    task_t *s = q13_spawn("racer");
    REQUIRE(a != NULL);
    REQUIRE(s != NULL);

    sched_raise_signal(s, SIGTSTP);
    CHECK_EQ(s->pending_stop, SIGTSTP);
    /* ^Z then `fg` faster than a tick. The stop must not land after the
     * continue, or the job comes back and immediately suspends itself -
     * which is a hang a user reads as "fg is broken". */
    sched_raise_signal(s, SIGCONT);
    CHECK_EQ(s->pending_stop, 0);
    for (int i = 0; i < 40 * Q13_QUANTUM; i++) {
        q13_tick(0);
        CHECK_NE(s->state, TASK_STOPPED);
    }
    q13_kill(a);
    q13_kill(s);
}

/* ---- signals: dispositions, and where each one is recorded ------------- */

TEST(sched, the_disposition_decides_which_field_a_signal_lands_in) {
    q13_boot();
    task_t *t = q13_spawn("dispositions");
    REQUIRE(t != NULL);

    /* Three fields, and which one a signal reaches is the whole of the
     * signal model: pending_signal means "this will die at the next
     * checkpoint", pending_stop means "this will suspend", sig_pending
     * means "this has a handler to run". A signal in the wrong one is a
     * program that dies instead of handling, or handles instead of
     * dying. */
    sched_raise_signal(t, SIGTERM); /* catchable, default death */
    CHECK_EQ(t->pending_signal, SIGTERM);
    CHECK_EQ(t->sig_pending, 0u);
    t->pending_signal = 0;

    sched_raise_signal(t, SIGCHLD); /* default: ignore */
    CHECK_EQ(t->pending_signal, 0);
    CHECK_EQ(t->sig_pending, 0u);

    t->sig_handler[SIGTERM] = 0x1234; /* a handler, so it is caught */
    sched_raise_signal(t, SIGTERM);
    CHECK_EQ(t->pending_signal, 0);
    CHECK((t->sig_pending & (1u << SIGTERM)) != 0);
    t->sig_handler[SIGTERM] = SIG_DFL_ADDR;
    t->sig_pending = 0;

    t->sig_handler[SIGTERM] = SIG_IGN_ADDR;
    sched_raise_signal(t, SIGTERM);
    CHECK_EQ(t->pending_signal, 0);
    CHECK_EQ(t->sig_pending, 0u);
    t->sig_handler[SIGTERM] = SIG_DFL_ADDR;

    /* SIGKILL ignores every one of those decisions, which is what
     * "uncatchable" has to mean or it means nothing. */
    t->sig_handler[SIGKILL] = 0x1234;
    sched_raise_signal(t, SIGKILL);
    CHECK_EQ(t->pending_signal, SIGKILL);
    CHECK_EQ(t->sig_pending, 0u);
    t->pending_signal = 0;
    t->sig_handler[SIGKILL] = SIG_DFL_ADDR;

    q13_kill(t);
}

TEST(sched, a_signal_number_outside_the_table_is_refused) {
    q13_boot();
    task_t *t = q13_spawn("bounds");
    REQUIRE(t != NULL);
    /* SYS_kill's argument comes from user space. An off-by-one here
     * writes past sig_handler[], which is inside task_t and therefore
     * inside the next task's fields. */
    sched_raise_signal(t, 0);
    sched_raise_signal(t, -1);
    sched_raise_signal(t, SIG_MAX + 1);
    sched_raise_signal(t, 100000);
    CHECK_EQ(t->pending_signal, 0);
    CHECK_EQ(t->pending_stop, 0);
    CHECK_EQ(t->sig_pending, 0u);
    q13_kill(t);
}

TEST(sched, a_dead_task_absorbs_a_signal_rather_than_taking_it) {
    q13_boot();
    task_t *t = q13_spawn("corpse");
    REQUIRE(t != NULL);
    t->state = TASK_TERMINATED;
    /* A parent that signals a child which has just exited is ordinary
     * and must not resurrect it - a TERMINATED task made READY again is
     * a task the scheduler will run with no stack. */
    sched_raise_signal(t, SIGKILL);
    sched_raise_signal(t, SIGTSTP);
    CHECK_EQ(t->state, TASK_TERMINATED);
    CHECK_EQ(t->pending_signal, 0);
    CHECK_EQ(t->pending_stop, 0);
    sched_reap_slot(t);
}

/* ---- process groups and sessions -------------------------------------- */

TEST(sched, a_group_signal_reaches_every_member_and_nobody_else) {
    q13_boot();
    task_t *in1 = q13_spawn("in1");
    task_t *in2 = q13_spawn("in2");
    task_t *out = q13_spawn("out");
    REQUIRE(in1 != NULL);
    REQUIRE(in2 != NULL);
    REQUIRE(out != NULL);

    int pg = in1->id;
    in1->pgid = pg;
    in2->pgid = pg;
    out->pgid = out->id;

    sched_raise_signal_group(pg, SIGINT);
    CHECK_EQ(in1->pending_signal, SIGINT);
    CHECK_EQ(in2->pending_signal, SIGINT);
    CHECK_EQ(out->pending_signal, 0);

    /* And a group nobody is in signals nobody rather than everybody,
     * which is the failure a "no match" fallback would produce. */
    in1->pending_signal = 0;
    in2->pending_signal = 0;
    sched_raise_signal_group(0x7ffffff, SIGINT);
    CHECK_EQ(in1->pending_signal, 0);
    CHECK_EQ(in2->pending_signal, 0);
    CHECK_EQ(out->pending_signal, 0);

    q13_kill(in1);
    q13_kill(in2);
    q13_kill(out);
}

/* ---- the lifecycle, at its edges --------------------------------------- */

TEST(sched, a_slot_comes_back_and_the_recycled_pid_is_a_different_pid) {
    q13_boot();
    task_t *first = q13_spawn("first");
    REQUIRE(first != NULL);
    int old_pid = first->id;
    int slot = PID_SLOT(old_pid);
    q13_kill(first);

    task_t *second = q13_spawn("second");
    REQUIRE(second != NULL);
    /* The slot is reused - that is M54 - and the pid is NOT, which is
     * the half that matters: a stale pid held by anybody must not name
     * the new task, or a `kill` aimed at a process that exited an hour
     * ago lands on whatever is in its chair now. */
    CHECK_EQ(PID_SLOT(second->id), slot);
    CHECK_NE(second->id, old_pid);
    CHECK_EQ(sched_task_by_id(old_pid), NULL);
    CHECK_EQ(sched_task_by_id(second->id), second);
    q13_kill(second);
}

TEST(sched, a_recycled_slot_inherits_nothing_from_the_task_that_had_it) {
    q13_boot();
    task_t *t = q13_spawn("leaver");
    REQUIRE(t != NULL);
    t->pgid = 4242;
    t->sid = 4242;
    t->pending_signal = SIGTERM;
    t->pending_stop = SIGTSTP;
    t->fs_base = 0xDEADBEEF;
    t->sig_handler[SIGINT] = 0x1234;
    t->sig_blocked = 0xFFu;
    t->sig_pending = 0xFFu;
    int slot = PID_SLOT(t->id);
    q13_kill(t);

    /* Asked of the NEXT task in that row rather than of the empty row
     * itself - sched_task_by_slot answers NULL for a free slot, which is
     * correct and is not the question. The question is what a program
     * that lands here inherits, and the answer has to be nothing.
     *
     * fs_base is on this list because M95 found it there: a recycled
     * slot kept the previous task's thread pointer and the new program
     * read the dead one's TLS. */
    task_t *reused = q13_spawn("newcomer");
    REQUIRE(reused != NULL);
    CHECK_EQ(PID_SLOT(reused->id), slot);
    CHECK_EQ(reused->fs_base, 0);
    CHECK_EQ(reused->pending_signal, 0);
    CHECK_EQ(reused->pending_stop, 0);
    CHECK_EQ(reused->sig_pending, 0u);
    CHECK_EQ(reused->sig_blocked, 0u);
    CHECK_EQ(reused->sig_handler[SIGINT], SIG_DFL_ADDR);
    CHECK_EQ(reused->sid, 0);
    CHECK(strcmp(reused->cwd, "/") == 0);
    q13_kill(reused);
}

TEST(sched, the_task_table_fills_and_recovers) {
    q13_boot();
    /* MAX_TASKS is a fixed ceiling and the interesting question is what
     * happens at it: a spawn that returns NULL is something SYS_spawn
     * can report, and a spawn that returns the 129th row is memory
     * corruption. */
    task_t *held[MAX_TASKS];
    int n = 0;
    while (n < MAX_TASKS) {
        task_t *t = q13_spawn("filler");
        if (!t) {
            break;
        }
        held[n++] = t;
    }
    CHECK(n > 0);
    CHECK_EQ(q13_spawn("one too many"), NULL);
    for (int i = 0; i < n; i++) {
        q13_kill(held[i]);
    }
    /* And the table is not permanently full afterwards, which is the
     * half a "does it refuse" test on its own would miss. */
    task_t *again = q13_spawn("after");
    CHECK(again != NULL);
    q13_kill(again);
}

TEST(sched, a_kernel_stack_goes_back_when_a_task_is_reaped) {
    q13_boot();
    uint64_t before = fake_pmm_outstanding();
    task_t *t[8];
    for (int i = 0; i < 8; i++) {
        t[i] = q13_spawn("stacky");
        REQUIRE(t[i] != NULL);
    }
    CHECK(fake_pmm_outstanding() > before);
    for (int i = 0; i < 8; i++) {
        q13_kill(t[i]);
    }
    /* The number M50 and M54 both had to measure on a booted machine.
     * Here it is exact and it is free. */
    CHECK_EQ(fake_pmm_outstanding(), before);
}

TEST(sched, a_spawn_that_cannot_get_a_stack_returns_null) {
    q13_boot();
    /* M102 found this check dead: it had been written against the
     * panicking allocator, so "no memory for a kernel stack" was a
     * halt rather than a refusal. Nothing had ever executed the
     * refusal, because reaching it on a booted machine means exhausting
     * physical memory. */
    uint64_t allocs = fake_pmm_total_allocs();
    fake_pmm_fail_after((int64_t)allocs);
    task_t *t = q13_spawn("starved");
    CHECK_EQ(t, NULL);
    fake_pmm_fail_after(-1);
    task_t *ok = q13_spawn("fed");
    CHECK(ok != NULL);
    q13_kill(ok);
}

/* ---- what a task hands back when it dies -------------------------------- */

TEST(sched, a_dying_task_gives_back_every_descriptor_it_held) {
    q13_boot();
    task_t *t = q13_spawn("holder");
    REQUIRE(t != NULL);
    fake_objects_reset();

    /* One of each kind the fd table can point at. The scheduler's whole
     * responsibility here is the refcount, and this is it as an
     * assertion rather than as an argument. */
    t->fds[3].type = FD_PIPE_READ;
    t->fds[3].pipe = (struct pipe *)0x1000;
    t->fds[4].type = FD_PIPE_WRITE;
    t->fds[4].pipe = (struct pipe *)0x1000;
    t->fds[5].type = FD_FILE;
    t->fds[5].file = (struct openfile *)0x2000;
    t->fds[6].type = FD_SOCKET;
    t->fds[6].sock = (struct socket *)0x3000;

    sched_release_fds(t);
    CHECK_EQ(fake_objects_pipe_read_refs(), -1);
    CHECK_EQ(fake_objects_pipe_write_refs(), -1);
    CHECK_EQ(fake_objects_file_refs(), -1);
    CHECK_EQ(fake_objects_socket_refs(), -1);
    /* And the slots are empty, so a second release cannot drop them
     * twice - which is the bug that makes a pipe's refcount go negative
     * and its buffer come back to a task that no longer exists. */
    CHECK_EQ(t->fds[3].type, FD_NONE);
    CHECK_EQ(t->fds[6].type, FD_NONE);
    sched_release_fds(t);
    CHECK_EQ(fake_objects_pipe_read_refs(), -1);
    CHECK_EQ(fake_objects_socket_refs(), -1);

    q13_kill(t);
}

/* ---- fork, which is where M85's second attempt found a real bug -------- */

TEST(sched, a_fork_inherits_what_it_must_and_nothing_it_must_not) {
    q13_boot();
    task_t *parent = q13_spawn("parent");
    REQUIRE(parent != NULL);

    parent->pgid = 77;
    parent->sid = 88;
    parent->caps = 0x5;
    parent->fs_base = 0xFEEDFACE;
    parent->sig_handler[SIGINT] = 0x9999;
    parent->sig_blocked = 0x40u;
    parent->sig_pending = 0x80u;
    parent->pending_signal = SIGTERM;
    parent->user_ticks = 1234;
    parent->sys_ticks = 5678;
    memcpy(parent->cwd, "/somewhere", sizeof("/somewhere"));

    /* task_fork reads `current_task[cpu]` as the parent, so the parent
     * has to be current. That is how it is called on the machine - from
     * inside SYS_fork - and making the test say so is cheaper than a
     * seam that lets it lie. */
    task_t *saved = sched_current();
    (void)saved;
    isr_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.rax = 0xAAAA;

    /* Put the parent on this CPU by letting the scheduler pick it. */
    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        placed = (sched_current() == parent);
    }
    REQUIRE(placed);

    task_t *child = task_fork(parent->pml4_phys, &regs);
    REQUIRE(child != NULL);

    CHECK_EQ(child->pgid, 77);
    CHECK_EQ(child->sid, 88);
    CHECK_EQ(child->caps, 0x5);
    CHECK_EQ(child->sig_handler[SIGINT], 0x9999);
    CHECK_EQ(child->sig_blocked, 0x40u);
    CHECK(strcmp(child->cwd, "/somewhere") == 0);
    CHECK_EQ(child->parent_id, parent->id);

    /* THE regression. task_fork copied the fd table, the signal
     * handlers, the FPU state, the mmap regions and the working
     * directory - and left fs_base at zero. Every `errno = ...` in this
     * libc is a store through %fs, so the first FAILING call a forked
     * child made was a page fault at address 0, and a child that only
     * ever succeeded never touched it. Found by M85's pty fixture, four
     * milestones after M96 put TLS in. */
    CHECK_EQ(child->fs_base, 0xFEEDFACE);

    /* And the three that must NOT be inherited. */
    CHECK_EQ(child->sig_pending, 0u);   /* sent to the parent before the child existed */
    CHECK_EQ(child->pending_signal, 0);
    CHECK_EQ(child->user_ticks, 0u);    /* M88: a child has not run */
    CHECK_EQ(child->sys_ticks, 0u);
    CHECK_EQ(child->is_thread, 0);
    CHECK_EQ(child->tgid, child->id);   /* a fork produces a process, never a thread */

    q13_kill(child);
    parent->pending_signal = 0;
    q13_kill(parent);
}

/* ---- the invariant M106 added, and the check that it fires ------------- */

TEST(sched, switching_away_from_a_stack_this_cpu_is_not_on_is_a_panic) {
    q13_boot();
    task_t *a = q13_spawn("a");
    task_t *b = q13_spawn("b");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);

    /* Put `a` on the CPU, then claim to be standing somewhere that is
     * not its stack. On the machine this is a scheduler bug that shows
     * up three switches later as a garbage rip in low memory; here it is
     * the panic M106 added, executed. */
    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        placed = (sched_current() == a);
    }
    REQUIRE(placed);

    fake_arch_set_cpu(0);
    fake_arch_stand_on(0x10);
    /* schedule() directly rather than a tick: a tick only switches once
     * every quantum, so a single one is as likely to return without
     * reaching the check as to reach it - and a panic test that
     * sometimes passes because nothing ran is worse than no test. */
    CHECK_PANIC(schedule(), "not on the stack");
    /* The panic longjmp'd out of schedule() with sched_lock held. The
     * runner drops every lock between tests; the rest of THIS test needs
     * them dropped now, and saying so is better than a test that fails
     * later with "recursive acquire" and no explanation. */
    fake_spinlock_release_all();

    /* And with an honest stack pointer it does not fire, which is the
     * half that stops this from passing against a check that always
     * panics. */
    for (int i = 0; i < 4 * Q13_QUANTUM; i++) {
        q13_tick(0);
    }

    q13_kill(a);
    q13_kill(b);
}

/* ---- M98: the wake-while-still-current window -------------------------- */

TEST(sched, a_woken_task_still_current_on_another_cpu_is_not_picked) {
    q13_boot();
    task_t *w = q13_spawn("w");
    task_t *f = q13_spawn("f");
    REQUIRE(w != NULL);
    REQUIRE(f != NULL);

    /* Bring cpu1's idle identity up the way smp.c's ap_main does - this
     * harness had never ticked a second CPU before this test, and a
     * tick on a CPU with no current task is not a case the machine has. */
    static int cpu1_up;
    if (!cpu1_up) {
        cpu1_up = 1;
        fake_arch_set_cpu(1);
        sched_init_ap(1);
    }

    /* Put `w` on cpu1. */
    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(1);
        fake_arch_set_cpu(1);
        placed = (sched_current() == w);
    }
    REQUIRE(placed);

    /* The window this test exists for: `w` marks itself BLOCKED on the
     * way into a wait but cpu1 has not reached schedule() - it is still
     * current there, still on its own kernel stack - and a wake from
     * another core makes it READY again. On the machine this is
     * futex_wait racing futex_wake; [m97]'s std::thread fixture hit it
     * on four cores and two CPUs ended up on one stack. */
    w->state = TASK_BLOCKED;
    sched_wake_task(w);
    CHECK_EQ(w->state, TASK_READY);

    /* cpu0 must refuse it for as long as cpu1 stands on it. */
    int stolen = 0;
    for (int i = 0; i < 40 * Q13_QUANTUM; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        if (sched_current() == w) {
            stolen = 1;
        }
    }
    CHECK_EQ(stolen, 0);

    /* cpu1 reaches its schedule() and switches away; only then is `w`
     * anybody else's to run. Ticked to a quantum boundary, not once - a
     * single tick mid-quantum returns without switching. */
    w->state = TASK_BLOCKED;
    for (int i = 0; i < 4 * Q13_QUANTUM; i++) {
        q13_tick(1);
        fake_arch_set_cpu(1);
        if (sched_current() != w) {
            break;
        }
    }
    fake_arch_set_cpu(1);
    REQUIRE(sched_current() != w);
    sched_wake_task(w);
    int picked = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !picked; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        picked = (sched_current() == w);
    }
    CHECK_EQ(picked, 1);

    /* Walk cpu0 off `w` before reaping - M106's rule, as q13_kill's own
     * comment explains. */
    for (int i = 0; i < 200 * Q13_QUANTUM; i++) {
        fake_arch_set_cpu(0);
        if (sched_current() != w) {
            break;
        }
        w->state = TASK_READY;
        q13_tick(0);
    }
    /* And walk cpu1 off whatever it picked up (it has `f` by now) -
     * q13_kill only walks cpu0, and a reap refuses a task any CPU still
     * stands on, which is M106's rule doing its job in the harness. */
    for (int i = 0; i < 4 * Q13_QUANTUM; i++) {
        fake_arch_set_cpu(1);
        task_t *cur = sched_current();
        if (!cur || cur->is_idle) {
            break;
        }
        cur->state = TASK_BLOCKED;
        q13_tick(1);
    }
    q13_kill(w);
    f->state = TASK_READY; /* unblock what the walk-off blocked */
    q13_kill(f);
}

/* ---- the thread pointer, loaded on the way in -------------------------- */

TEST(sched, a_switch_loads_the_incoming_tasks_thread_pointer) {
    q13_boot();
    task_t *a = q13_spawn("tls-a");
    task_t *b = q13_spawn("tls-b");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    a->fs_base = 0xAAAA0000;
    b->fs_base = 0xBBBB0000;

    /* M96: `%fs:0` is where every access to a `__thread` variable goes,
     * and a task resumed with the previous task's FS base reads the
     * previous task's copy. Written on every switch rather than only
     * when it differs - and this asserts the value that was written, not
     * that a write happened. */
    for (int i = 0; i < 400 * Q13_QUANTUM; i++) {
        q13_tick(0);
        task_t *cur = sched_current();
        if (cur == a) {
            CHECK_EQ(fake_cpu_last_msr(MSR_FS_BASE), 0xAAAA0000u);
        } else if (cur == b) {
            CHECK_EQ(fake_cpu_last_msr(MSR_FS_BASE), 0xBBBB0000u);
        }
    }

    q13_kill(a);
    q13_kill(b);
}

/* ---- Q9: the lock order, and a checker that can fail ------------------
 *
 * `heap.c` documents its lock order in a comment and `spinlock.h`
 * documents the interrupt rule in three paragraphs, and a comment cannot
 * fail. tests/fakes/fake_spinlock.c records every (outer, inner) pair it
 * sees and panics on the reverse; these two tests are what stop that
 * from being a mechanism nothing exercises.
 */

TEST(sched, taking_two_locks_in_both_orders_is_caught) {
    /* The failure M56 cost "about one boot in ten" to find, made into an
     * assertion.

     * Two locks of this test's own rather than the kernel's, and that is
     * a limitation worth stating plainly rather than hiding: **no kernel
     * unit in this tier currently nests one lock inside another.** The
     * first version of this pair asserted that it did, and failed - the
     * scheduler takes its lock without calling into the heap or the
     * filesystem under it, and the pmm and vmm here are fakes with no
     * locks at all. So what is proved is that the detector works, not
     * that the kernel's order is checked; it is armed for the day a path
     * does nest, and that day it needs no new code.
     *
     * Provoking a real inversion would mean writing one into the kernel,
     * and a test that needs the bug present to prove the detector works
     * is a test nobody can run twice. */
    static spinlock_t a, b;
    a.locked = 0;
    b.locked = 0;

    int pairs_before = fake_spinlock_order_pairs();
    spin_lock(&a);
    spin_lock(&b);
    spin_unlock(&b);
    spin_unlock(&a);
    /* The order was learned. Without this the check below could pass
     * against a detector that had recorded nothing and was comparing an
     * empty table. */
    CHECK(fake_spinlock_order_pairs() > pairs_before);

    /* Now the other way round, which is the inversion. */
    spin_lock(&b);
    CHECK_PANIC(spin_lock(&a), "lock order inversion");
    fake_spinlock_release_all();
}
