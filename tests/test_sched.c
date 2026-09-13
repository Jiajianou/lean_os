#include "check.h"
#include "fakes/fakes.h"

#include "architecture/x86_64/cpu.h"
#include "library/spinlock.h"
#include "file_system/flock.h"
#include "scheduler/scheduler.h"
#include "signal.h"

#include <string.h>

#define Q13_QUANTUM 2

void q13_boot(void);
task_t *q13_spawn(const char *name);
void q13_kill(task_t *t);

static void q13_body(void *arg) {
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
    fake_arch_set_cpu(0);
    if (sched_current() == t) {
        t->state = TASK_TERMINATED;
        fake_arch_stand_on(t->kernel_stack_top ? t->kernel_stack_top - 64 : 0);
        schedule();
    }
    t->state = TASK_TERMINATED;
    sched_reap_slot(t);
}

static void q13_tick(int cpu) {
    fake_arch_set_cpu(cpu);
    task_t *cur = sched_current();
    fake_arch_stand_on(cur && cur->kernel_stack_top
                           ? cur->kernel_stack_top - 64
                           : 0);
    scheduler_tick_cpu(cpu);
}

TEST(sched, the_constants_are_the_kernels_own) {
    CHECK_EQ(MAX_TASKS, 128);
    CHECK_EQ(MAX_CPUS, 8);
    CHECK(MAX_FDS >= 128);
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

TEST(sched, every_runnable_task_gets_a_turn) {
    q13_boot();
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
    for (int i = 0; i < 60 * Q13_QUANTUM && s->state != TASK_STOPPED; i++) {
        q13_tick(0);
    }
    CHECK_EQ(s->state, TASK_STOPPED);
    CHECK_EQ(s->pending_stop, 0);

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
    sched_raise_signal(s, SIGCONT);
    CHECK_EQ(s->pending_stop, 0);
    for (int i = 0; i < 40 * Q13_QUANTUM; i++) {
        q13_tick(0);
        CHECK_NE(s->state, TASK_STOPPED);
    }
    q13_kill(a);
    q13_kill(s);
}

TEST(sched, the_disposition_decides_which_field_a_signal_lands_in) {
    q13_boot();
    task_t *t = q13_spawn("dispositions");
    REQUIRE(t != NULL);

    sched_raise_signal(t, SIGTERM);
    CHECK_EQ(t->pending_signal, SIGTERM);
    CHECK_EQ(t->sig_pending, 0u);
    t->pending_signal = 0;

    sched_raise_signal(t, SIGCHLD);
    CHECK_EQ(t->pending_signal, 0);
    CHECK_EQ(t->sig_pending, 0u);

    t->sig_handler[SIGTERM] = 0x1234;
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
    sched_raise_signal(t, SIGKILL);
    sched_raise_signal(t, SIGTSTP);
    CHECK_EQ(t->state, TASK_TERMINATED);
    CHECK_EQ(t->pending_signal, 0);
    CHECK_EQ(t->pending_stop, 0);
    sched_reap_slot(t);
}

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

TEST(sched, a_slot_comes_back_and_the_recycled_pid_is_a_different_pid) {
    q13_boot();
    task_t *first = q13_spawn("first");
    REQUIRE(first != NULL);
    int old_pid = first->id;
    int slot = PID_SLOT(old_pid);
    q13_kill(first);

    task_t *second = q13_spawn("second");
    REQUIRE(second != NULL);
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

TEST(sched, a_reaped_childs_peak_resident_set_reaches_its_parent) {
    q13_boot();
    task_t *parent = q13_spawn("parent");
    task_t *child = q13_spawn("child");
    REQUIRE(parent != NULL);
    REQUIRE(child != NULL);
    child->parent_id = parent->id;
    child->max_rss_pages = 4096;

    child->state = TASK_TERMINATED;
    sched_reap_slot(child);

    CHECK_EQ(parent->child_max_rss_pages, 4096u);
    CHECK_EQ(parent->max_rss_pages, 0u);
    q13_kill(parent);
}

TEST(sched, a_parent_keeps_the_largest_childs_peak_not_the_last_one) {
    q13_boot();
    task_t *parent = q13_spawn("parent");
    REQUIRE(parent != NULL);

    const uint64_t peaks[] = {1000, 9000, 300};
    for (int i = 0; i < 3; i++) {
        task_t *c = q13_spawn("child");
        REQUIRE(c != NULL);
        c->parent_id = parent->id;
        c->max_rss_pages = peaks[i];
        c->state = TASK_TERMINATED;
        sched_reap_slot(c);
    }
    CHECK_EQ(parent->child_max_rss_pages, 9000u);

    task_t *mid = q13_spawn("mid");
    REQUIRE(mid != NULL);
    mid->parent_id = parent->id;
    mid->child_max_rss_pages = 20000;
    mid->state = TASK_TERMINATED;
    sched_reap_slot(mid);
    CHECK_EQ(parent->child_max_rss_pages, 20000u);
    q13_kill(parent);
}

TEST(sched, a_joined_threads_peak_is_the_processs_own_not_a_childs) {
    q13_boot();
    task_t *proc = q13_spawn("proc");
    REQUIRE(proc != NULL);
    task_t *thread = q13_spawn("thread");
    REQUIRE(thread != NULL);
    thread->parent_id = proc->id;
    thread->is_thread = 1;
    thread->tgid = proc->tgid;
    thread->max_rss_pages = 7777;

    thread->state = TASK_TERMINATED;
    sched_reap_slot(thread);

    CHECK_EQ(proc->max_rss_pages, 7777u);
    CHECK_EQ(proc->child_max_rss_pages, 0u);
    q13_kill(proc);
}

TEST(sched, a_recycled_slot_reports_no_peak_from_the_task_before_it) {
    q13_boot();
    task_t *t = q13_spawn("big");
    REQUIRE(t != NULL);
    t->max_rss_pages = 500000;
    t->child_max_rss_pages = 500000;
    int slot = PID_SLOT(t->id);
    q13_kill(t);

    task_t *reused = q13_spawn("small");
    REQUIRE(reused != NULL);
    CHECK_EQ(PID_SLOT(reused->id), slot);
    CHECK_EQ(reused->max_rss_pages, 0u);
    CHECK_EQ(reused->child_max_rss_pages, 0u);
    q13_kill(reused);
}

TEST(sched, the_task_table_fills_and_recovers) {
    q13_boot();
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
    CHECK_EQ(fake_pmm_outstanding(), before);
}

TEST(sched, a_spawn_that_cannot_get_a_stack_returns_null) {
    q13_boot();
    uint64_t allocs = fake_pmm_total_allocs();
    fake_pmm_fail_after((int64_t)allocs);
    task_t *t = q13_spawn("starved");
    CHECK_EQ(t, NULL);
    fake_pmm_fail_after(-1);
    task_t *ok = q13_spawn("fed");
    CHECK(ok != NULL);
    q13_kill(ok);
}

TEST(sched, a_dying_task_gives_back_every_descriptor_it_held) {
    q13_boot();
    task_t *t = q13_spawn("holder");
    REQUIRE(t != NULL);
    fake_objects_reset();

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
    CHECK_EQ(t->fds[3].type, FD_NONE);
    CHECK_EQ(t->fds[6].type, FD_NONE);
    sched_release_fds(t);
    CHECK_EQ(fake_objects_pipe_read_refs(), -1);
    CHECK_EQ(fake_objects_socket_refs(), -1);

    q13_kill(t);
}

TEST(sched, a_dying_task_gives_back_every_record_lock_it_held) {
    q13_boot();
    task_t *t = q13_spawn("locker");
    task_t *u = q13_spawn("bystander");
    REQUIRE(t != NULL);
    REQUIRE(u != NULL);
    flock_release_pid(t->id);
    flock_release_pid(u->id);
    int before = flock_count();

    CHECK_EQ(flock_set(11, t->id, OS_FLOCK_WR, 0, 10), 0);
    CHECK_EQ(flock_set(12, t->id, OS_FLOCK_RD, 0, 0), 0);
    CHECK_EQ(flock_set(11, u->id, OS_FLOCK_WR, 50, 10), 0);
    CHECK_EQ(flock_count(), before + 3);

    sched_release_fds(t);
    CHECK_EQ(flock_count(), before + 1);
    os_flock_t who;
    CHECK_EQ(flock_test(11, 0, OS_FLOCK_WR, 0, 10, &who), 0);
    CHECK_EQ(flock_test(11, 0, OS_FLOCK_WR, 50, 10, &who), 1);
    CHECK_EQ(who.pid, u->id);

    sched_release_fds(t);
    CHECK_EQ(flock_count(), before + 1);

    flock_release_pid(u->id);
    CHECK_EQ(flock_count(), before);
    q13_kill(t);
    q13_kill(u);
}

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

    task_t *saved = sched_current();
    (void)saved;
    isr_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.rax = 0xAAAA;

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

    CHECK_EQ(child->fs_base, 0xFEEDFACE);

    CHECK_EQ(child->sig_pending, 0u);
    CHECK_EQ(child->pending_signal, 0);
    CHECK_EQ(child->user_ticks, 0u);
    CHECK_EQ(child->sys_ticks, 0u);
    CHECK_EQ(child->is_thread, 0);
    CHECK_EQ(child->tgid, child->id);

    q13_kill(child);
    parent->pending_signal = 0;
    q13_kill(parent);
}

TEST(sched, switching_away_from_a_stack_this_cpu_is_not_on_is_a_panic) {
    q13_boot();
    task_t *a = q13_spawn("a");
    task_t *b = q13_spawn("b");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);

    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        placed = (sched_current() == a);
    }
    REQUIRE(placed);

    fake_arch_set_cpu(0);
    fake_arch_stand_on(0x10);
    CHECK_PANIC(schedule(), "not on the stack");
    fake_spinlock_release_all();

    for (int i = 0; i < 4 * Q13_QUANTUM; i++) {
        q13_tick(0);
    }

    q13_kill(a);
    q13_kill(b);
}

TEST(sched, a_woken_task_still_current_on_another_cpu_is_not_picked) {
    q13_boot();
    task_t *w = q13_spawn("w");
    task_t *f = q13_spawn("f");
    REQUIRE(w != NULL);
    REQUIRE(f != NULL);

    static int cpu1_up;
    if (!cpu1_up) {
        cpu1_up = 1;
        fake_arch_set_cpu(1);
        sched_init_ap(1);
    }

    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(1);
        fake_arch_set_cpu(1);
        placed = (sched_current() == w);
    }
    REQUIRE(placed);

    w->state = TASK_BLOCKED;
    sched_wake_task(w);
    CHECK_EQ(w->state, TASK_READY);

    int stolen = 0;
    for (int i = 0; i < 40 * Q13_QUANTUM; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        if (sched_current() == w) {
            stolen = 1;
        }
    }
    CHECK_EQ(stolen, 0);

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

    for (int i = 0; i < 200 * Q13_QUANTUM; i++) {
        fake_arch_set_cpu(0);
        if (sched_current() != w) {
            break;
        }
        w->state = TASK_READY;
        q13_tick(0);
    }
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
    f->state = TASK_READY;
    q13_kill(f);
}

TEST(sched, a_switch_loads_the_incoming_tasks_thread_pointer) {
    q13_boot();
    task_t *a = q13_spawn("tls-a");
    task_t *b = q13_spawn("tls-b");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    a->fs_base = 0xAAAA0000;
    b->fs_base = 0xBBBB0000;

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

TEST(sched, taking_two_locks_in_both_orders_is_caught) {

    static spinlock_t a, b;
    a.locked = 0;
    b.locked = 0;

    int pairs_before = fake_spinlock_order_pairs();
    spin_lock(&a);
    spin_lock(&b);
    spin_unlock(&b);
    spin_unlock(&a);
    CHECK(fake_spinlock_order_pairs() > pairs_before);

    spin_lock(&b);
    CHECK_PANIC(spin_lock(&a), "lock order inversion");
    fake_spinlock_release_all();
}
