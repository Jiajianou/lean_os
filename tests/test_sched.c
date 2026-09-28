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
    scheduler_init();
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
    if (scheduler_current() == t) {
        t->state = TASK_TERMINATED;
        fake_arch_stand_on(t->kernel_stack_top ? t->kernel_stack_top - 64 : 0);
        schedule();
    }
    t->state = TASK_TERMINATED;
    scheduler_reap_slot(t);
}

static void q13_tick(int cpu) {
    fake_arch_set_cpu(cpu);
    task_t *current = scheduler_current();
    fake_arch_stand_on(current && current->kernel_stack_top
                           ? current->kernel_stack_top - 64
                           : 0);
    scheduler_tick_cpu(cpu);
}

TEST(scheduler, the_constants_are_the_kernels_own) {
    CHECK_EQ(MAX_TASKS, 256);
    CHECK_EQ(MAX_CPUS, 16);
    CHECK(MAX_FILE_DESCRIPTORS >= 1024);
    q13_boot();
    task_t *a = q13_spawn("q-a");
    task_t *b = q13_spawn("q-b");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    int placed = 0;
    for (int i = 0; i < 200 && !placed; i++) {
        q13_tick(0);
        placed = (scheduler_current() == a);
    }
    REQUIRE(placed);
    int ticks = 0;
    while (scheduler_current() == a && ticks < 64) {
        q13_tick(0);
        ticks++;
    }
    CHECK_EQ(ticks, Q13_QUANTUM);
    q13_kill(a);
    q13_kill(b);
}

TEST(scheduler, every_runnable_task_gets_a_turn) {
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
        task_t *current = scheduler_current();
        for (int j = 0; j < 6; j++) {
            if (current == t[j]) {
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

TEST(scheduler, slow_shares_within_one_quantum_at_every_task_count) {
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
            task_t *current = scheduler_current();
            for (int j = 0; j < n; j++) {
                if (current == t[j]) {
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

TEST(scheduler, a_blocked_task_is_not_runnable_and_a_wake_makes_it_so) {
    q13_boot();
    task_t *a = q13_spawn("a");
    task_t *b = q13_spawn("b");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);

    b->state = TASK_BLOCKED;
    int saw_b = 0;
    for (int i = 0; i < 40 * Q13_QUANTUM; i++) {
        q13_tick(0);
        if (scheduler_current() == b) {
            saw_b = 1;
        }
    }
    CHECK_EQ(saw_b, 0);

    scheduler_wake_task(b);
    CHECK_EQ(b->state, TASK_READY);
    for (int i = 0; i < 40 * Q13_QUANTUM && !saw_b; i++) {
        q13_tick(0);
        if (scheduler_current() == b) {
            saw_b = 1;
        }
    }
    CHECK_EQ(saw_b, 1);

    q13_kill(a);
    q13_kill(b);
}

TEST(scheduler, a_stopped_task_is_not_woken_by_anything_but_a_resume) {
    q13_boot();
    task_t *a = q13_spawn("a");
    task_t *s = q13_spawn("stopped");
    REQUIRE(a != NULL);
    REQUIRE(s != NULL);

    scheduler_raise_signal(s, SIGTSTP);
    CHECK_EQ(s->pending_stop, SIGTSTP);
    for (int i = 0; i < 60 * Q13_QUANTUM && s->state != TASK_STOPPED; i++) {
        q13_tick(0);
    }
    CHECK_EQ(s->state, TASK_STOPPED);
    CHECK_EQ(s->pending_stop, 0);

    scheduler_wake_task(s);
    scheduler_wake_all(SCHEDULER_POLL_CHAN);
    CHECK_EQ(s->state, TASK_STOPPED);
    for (int i = 0; i < 40 * Q13_QUANTUM; i++) {
        q13_tick(0);
        CHECK_NE((long long)(uintptr_t)scheduler_current(), (long long)(uintptr_t)s);
    }

    scheduler_resume_stopped(s);
    CHECK_EQ(s->state, TASK_READY);

    q13_kill(a);
    q13_kill(s);
}

TEST(scheduler, a_stopped_task_can_still_be_killed) {
    q13_boot();
    task_t *a = q13_spawn("a");
    task_t *s = q13_spawn("stopped");
    REQUIRE(a != NULL);
    REQUIRE(s != NULL);

    s->state = TASK_STOPPED;
    scheduler_raise_signal(s, SIGKILL);
    CHECK_EQ(s->state, TASK_READY);
    CHECK_EQ(s->pending_signal, SIGKILL);

    s->pending_signal = 0;
    q13_kill(a);
    q13_kill(s);
}

TEST(scheduler, sigcont_resumes_a_task_that_ignores_it) {
    q13_boot();
    task_t *s = q13_spawn("ignorer");
    REQUIRE(s != NULL);
    s->sig_handler[SIGCONT] = SIG_IGN_ADDR;
    s->state = TASK_STOPPED;
    scheduler_raise_signal(s, SIGCONT);
    CHECK_EQ(s->state, TASK_READY);
    s->sig_handler[SIGCONT] = SIG_DFL_ADDR;
    q13_kill(s);
}

TEST(scheduler, a_pending_stop_is_cleared_by_sigcont_before_it_is_ever_taken) {
    q13_boot();
    task_t *a = q13_spawn("a");
    task_t *s = q13_spawn("racer");
    REQUIRE(a != NULL);
    REQUIRE(s != NULL);

    scheduler_raise_signal(s, SIGTSTP);
    CHECK_EQ(s->pending_stop, SIGTSTP);
    scheduler_raise_signal(s, SIGCONT);
    CHECK_EQ(s->pending_stop, 0);
    for (int i = 0; i < 40 * Q13_QUANTUM; i++) {
        q13_tick(0);
        CHECK_NE(s->state, TASK_STOPPED);
    }
    q13_kill(a);
    q13_kill(s);
}

TEST(scheduler, the_disposition_decides_which_field_a_signal_lands_in) {
    q13_boot();
    task_t *t = q13_spawn("dispositions");
    REQUIRE(t != NULL);

    scheduler_raise_signal(t, SIGTERM);
    CHECK_EQ(t->pending_signal, SIGTERM);
    CHECK_EQ(t->sig_pending, 0u);
    t->pending_signal = 0;

    scheduler_raise_signal(t, SIGCHLD);
    CHECK_EQ(t->pending_signal, 0);
    CHECK_EQ(t->sig_pending, 0u);

    t->sig_handler[SIGTERM] = 0x1234;
    scheduler_raise_signal(t, SIGTERM);
    CHECK_EQ(t->pending_signal, 0);
    CHECK((t->sig_pending & (1u << SIGTERM)) != 0);
    t->sig_handler[SIGTERM] = SIG_DFL_ADDR;
    t->sig_pending = 0;

    t->sig_handler[SIGTERM] = SIG_IGN_ADDR;
    scheduler_raise_signal(t, SIGTERM);
    CHECK_EQ(t->pending_signal, 0);
    CHECK_EQ(t->sig_pending, 0u);
    t->sig_handler[SIGTERM] = SIG_DFL_ADDR;

    t->sig_handler[SIGKILL] = 0x1234;
    scheduler_raise_signal(t, SIGKILL);
    CHECK_EQ(t->pending_signal, SIGKILL);
    CHECK_EQ(t->sig_pending, 0u);
    t->pending_signal = 0;
    t->sig_handler[SIGKILL] = SIG_DFL_ADDR;

    q13_kill(t);
}

TEST(scheduler, a_signal_number_outside_the_table_is_refused) {
    q13_boot();
    task_t *t = q13_spawn("bounds");
    REQUIRE(t != NULL);
    scheduler_raise_signal(t, 0);
    scheduler_raise_signal(t, -1);
    scheduler_raise_signal(t, SIG_MAX + 1);
    scheduler_raise_signal(t, 100000);
    CHECK_EQ(t->pending_signal, 0);
    CHECK_EQ(t->pending_stop, 0);
    CHECK_EQ(t->sig_pending, 0u);
    q13_kill(t);
}

TEST(scheduler, a_dead_task_absorbs_a_signal_rather_than_taking_it) {
    q13_boot();
    task_t *t = q13_spawn("corpse");
    REQUIRE(t != NULL);
    t->state = TASK_TERMINATED;
    scheduler_raise_signal(t, SIGKILL);
    scheduler_raise_signal(t, SIGTSTP);
    CHECK_EQ(t->state, TASK_TERMINATED);
    CHECK_EQ(t->pending_signal, 0);
    CHECK_EQ(t->pending_stop, 0);
    scheduler_reap_slot(t);
}

TEST(scheduler, a_group_signal_reaches_every_member_and_nobody_else) {
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

    scheduler_raise_signal_group(pg, SIGINT);
    CHECK_EQ(in1->pending_signal, SIGINT);
    CHECK_EQ(in2->pending_signal, SIGINT);
    CHECK_EQ(out->pending_signal, 0);

    in1->pending_signal = 0;
    in2->pending_signal = 0;
    scheduler_raise_signal_group(0x7ffffff, SIGINT);
    CHECK_EQ(in1->pending_signal, 0);
    CHECK_EQ(in2->pending_signal, 0);
    CHECK_EQ(out->pending_signal, 0);

    q13_kill(in1);
    q13_kill(in2);
    q13_kill(out);
}

/* M172: whether a copy-on-write break has to tell the other cores. A frame
   that moved is invisible to a thread on another core holding a read-only
   translation of the old one, so the fault path asks this before it spends
   a shootdown - and a task that is gone, or one in another address space,
   holds no translation worth invalidating. */
TEST(scheduler, an_address_space_is_shared_only_while_another_live_task_is_in_it) {
    q13_boot();
    task_t *a = q13_spawn("as-a");
    task_t *b = q13_spawn("as-b");
    task_t *c = q13_spawn("as-c");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    REQUIRE(c != NULL);
    a->pml4_phys = 0x7A000;
    b->pml4_phys = 0x7B000;
    c->pml4_phys = 0x7C000;
    CHECK_EQ(scheduler_address_space_is_shared(a), 0);
    CHECK_EQ(scheduler_address_space_is_shared((task_t *)0), 0);
    b->pml4_phys = a->pml4_phys;
    CHECK_EQ(scheduler_address_space_is_shared(a), 1);
    CHECK_EQ(scheduler_address_space_is_shared(b), 1);
    CHECK_EQ(scheduler_address_space_is_shared(c), 0);
    b->state = TASK_TERMINATED;
    CHECK_EQ(scheduler_address_space_is_shared(a), 0);
    b->state = TASK_READY;
    b->pml4_phys = 0x7B000;
    q13_kill(c);
    q13_kill(b);
    q13_kill(a);
}

TEST(scheduler, a_slot_comes_back_and_the_recycled_pid_is_a_different_pid) {
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
    CHECK_EQ(scheduler_task_by_id(old_pid), NULL);
    CHECK_EQ(scheduler_task_by_id(second->id), second);
    q13_kill(second);
}

TEST(scheduler, a_recycled_slot_inherits_nothing_from_the_task_that_had_it) {
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

/* M168. A thread-group leader's slot is not reset while its threads are
   still running in the address space it owns.

   Everything scheduler_reap_slot does belongs to the ADDRESS SPACE rather
   than to the task: it frees the mapping table, forgets the memfd references
   that table holds, and hands the slot to the next task with a new
   generation - after which scheduler_vm_owner() cannot find the owner at all
   and every surviving thread is running in a space with no record of what is
   mapped in it. The teardown then frees the frames of a shared mapping a
   second time.

   And it is not the leader that does this to itself: its PARENT reaps it.
   Chromium's browser reaps a child whose main thread returned first, which
   is every child it has. */
TEST(scheduler, a_leaders_slot_is_held_while_its_threads_are_still_running) {
    q13_boot();
    task_t *leader = q13_spawn("leader");
    task_t *thread = q13_spawn("thread");
    REQUIRE(leader != NULL);
    REQUIRE(thread != NULL);

    /* An address space of their own: a task still on the kernel's page table
       has no user space for anybody to be left running in. */
    const uint64_t shared_space = 0x2000;
    leader->pml4_phys = shared_space;
    thread->pml4_phys = shared_space;
    thread->tgid = leader->tgid;
    thread->is_thread = 1;

    mmap_region_t *const table = leader->mmaps;
    const int generation_before = leader->generation;

    leader->state = TASK_TERMINATED;
    scheduler_reap_slot(leader);

    /* Held: still terminated, still the owner, still holding the table the
       running thread reaches through scheduler_vm_owner(). */
    CHECK_EQ(leader->state, TASK_TERMINATED);
    CHECK_EQ(leader->generation, generation_before);
    CHECK_EQ(leader->is_thread, 0);
    CHECK(leader->mmaps == table);
    CHECK(leader->pml4_phys == shared_space);

    /* And released the moment the last thread using the space is reaped -
       by the same call, so nothing has to come back for it later. */
    thread->state = TASK_TERMINATED;
    thread->pml4_phys = 0;
    scheduler_reap_slot(thread);

    CHECK_EQ(thread->state, TASK_FREE);
    CHECK_EQ(leader->state, TASK_FREE);
    CHECK(leader->mmaps == NULL);
}

TEST(scheduler, a_leader_with_nobody_left_is_reaped_at_once) {
    q13_boot();
    task_t *leader = q13_spawn("leader");
    REQUIRE(leader != NULL);

    /* The ordinary case, and the one every single-threaded program takes:
       the last task out has already torn its address space down and is on
       the kernel's page table by the time anybody reaps it. */
    leader->state = TASK_TERMINATED;
    scheduler_reap_slot(leader);
    CHECK_EQ(leader->state, TASK_FREE);
}

TEST(scheduler, a_thread_is_reaped_even_though_its_leader_is_still_running) {
    q13_boot();
    task_t *leader = q13_spawn("leader");
    task_t *thread = q13_spawn("thread");
    REQUIRE(leader != NULL);
    REQUIRE(thread != NULL);
    const uint64_t shared_space = 0x3000;
    leader->pml4_phys = shared_space;
    thread->pml4_phys = shared_space;
    thread->tgid = leader->tgid;
    thread->is_thread = 1;

    /* The rule is about the OWNER. A thread holds nothing the others need,
       so holding its slot would be a leak rather than a safeguard - and a
       browser makes thousands of them. */
    thread->state = TASK_TERMINATED;
    scheduler_reap_slot(thread);
    CHECK_EQ(thread->state, TASK_FREE);
    CHECK_EQ(leader->state, TASK_READY);

    leader->pml4_phys = 0;
    q13_kill(leader);
}

TEST(scheduler, a_reaped_childs_peak_resident_set_reaches_its_parent) {
    q13_boot();
    task_t *parent = q13_spawn("parent");
    task_t *child = q13_spawn("child");
    REQUIRE(parent != NULL);
    REQUIRE(child != NULL);
    child->parent_id = parent->id;
    child->max_rss_pages = 4096;

    child->state = TASK_TERMINATED;
    scheduler_reap_slot(child);

    CHECK_EQ(parent->child_max_rss_pages, 4096u);
    CHECK_EQ(parent->max_rss_pages, 0u);
    q13_kill(parent);
}

TEST(scheduler, a_parent_keeps_the_largest_childs_peak_not_the_last_one) {
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
        scheduler_reap_slot(c);
    }
    CHECK_EQ(parent->child_max_rss_pages, 9000u);

    task_t *mid = q13_spawn("mid");
    REQUIRE(mid != NULL);
    mid->parent_id = parent->id;
    mid->child_max_rss_pages = 20000;
    mid->state = TASK_TERMINATED;
    scheduler_reap_slot(mid);
    CHECK_EQ(parent->child_max_rss_pages, 20000u);
    q13_kill(parent);
}

TEST(scheduler, a_joined_threads_peak_is_the_processs_own_not_a_childs) {
    q13_boot();
    task_t *process = q13_spawn("proc");
    REQUIRE(process != NULL);
    task_t *thread = q13_spawn("thread");
    REQUIRE(thread != NULL);
    thread->parent_id = process->id;
    thread->is_thread = 1;
    thread->tgid = process->tgid;
    thread->max_rss_pages = 7777;

    thread->state = TASK_TERMINATED;
    scheduler_reap_slot(thread);

    CHECK_EQ(process->max_rss_pages, 7777u);
    CHECK_EQ(process->child_max_rss_pages, 0u);
    q13_kill(process);
}

TEST(scheduler, a_recycled_slot_reports_no_peak_from_the_task_before_it) {
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

TEST(scheduler, the_task_table_fills_and_recovers) {
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

TEST(scheduler, a_kernel_stack_goes_back_when_a_task_is_reaped) {
    q13_boot();
    uint64_t before = fake_physical_memory_outstanding();
    task_t *t[8];
    for (int i = 0; i < 8; i++) {
        t[i] = q13_spawn("stacky");
        REQUIRE(t[i] != NULL);
    }
    CHECK(fake_physical_memory_outstanding() > before);
    for (int i = 0; i < 8; i++) {
        q13_kill(t[i]);
    }
    CHECK_EQ(fake_physical_memory_outstanding(), before);
}

TEST(scheduler, a_spawn_that_cannot_get_a_stack_returns_null) {
    q13_boot();
    uint64_t allocs = fake_physical_memory_total_allocs();
    fake_physical_memory_fail_after((int64_t)allocs);
    task_t *t = q13_spawn("starved");
    CHECK_EQ(t, NULL);
    fake_physical_memory_fail_after(-1);
    task_t *ok = q13_spawn("fed");
    CHECK(ok != NULL);
    q13_kill(ok);
}

TEST(scheduler, a_dying_task_gives_back_every_descriptor_it_held) {
    q13_boot();
    task_t *t = q13_spawn("holder");
    REQUIRE(t != NULL);
    REQUIRE(t->descriptor_table != NULL);
    fake_objects_reset();

    t->descriptor_table->slots[3].type = FILE_DESCRIPTOR_PIPE_READ;
    t->descriptor_table->slots[3].pipe = (struct pipe *)0x1000;
    t->descriptor_table->slots[4].type = FILE_DESCRIPTOR_PIPE_WRITE;
    t->descriptor_table->slots[4].pipe = (struct pipe *)0x1000;
    t->descriptor_table->slots[5].type = FILE_DESCRIPTOR_FILE;
    t->descriptor_table->slots[5].file = (struct open_file *)0x2000;
    t->descriptor_table->slots[6].type = FILE_DESCRIPTOR_SOCKET;
    t->descriptor_table->slots[6].sock = (struct socket *)0x3000;

    scheduler_release_file_descriptors(t);
    CHECK_EQ(fake_objects_pipe_read_refs(), -1);
    CHECK_EQ(fake_objects_pipe_write_refs(), -1);
    CHECK_EQ(fake_objects_file_refs(), -1);
    CHECK_EQ(fake_objects_socket_refs(), -1);
    /* The table went with the last reference to it, so there is no slot
       left to inspect - which is the point. Releasing again must be quiet
       rather than a second round of closes on freed memory. */
    CHECK_EQ(t->descriptor_table, NULL);
    scheduler_release_file_descriptors(t);
    CHECK_EQ(fake_objects_pipe_read_refs(), -1);
    CHECK_EQ(fake_objects_socket_refs(), -1);

    q13_kill(t);
}

/* M146. The half a booted machine cannot show: that the descriptors close
   ONCE, when the last task sharing the table is gone, and not when the
   first of them exits. On the machine a thread exiting early and a thread
   exiting late look the same from outside; here the reference count is
   readable. */
TEST(scheduler, a_thread_shares_its_process_descriptor_table) {
    q13_boot();
    task_t *leader = q13_spawn("leader");
    REQUIRE(leader != NULL);
    task_t *worker = task_spawn_thread("worker", leader, q13_body, (void *)0);
    REQUIRE(worker != NULL);

    /* Not a copy - the same table. */
    CHECK_EQ(worker->descriptor_table, leader->descriptor_table);
    CHECK_EQ(leader->descriptor_table->references, 2);

    fake_objects_reset();
    leader->descriptor_table->slots[7].type = FILE_DESCRIPTOR_SOCKET;
    leader->descriptor_table->slots[7].sock = (struct socket *)0x4000;

    /* A descriptor opened after the thread started is visible to it, which
       a copy taken at spawn time could not be. */
    CHECK_EQ(worker->descriptor_table->slots[7].type, FILE_DESCRIPTOR_SOCKET);

    /* The thread exits first. Its siblings still hold the table, so nothing
       closes. */
    scheduler_release_file_descriptors(worker);
    CHECK_EQ(worker->descriptor_table, NULL);
    CHECK_EQ(fake_objects_socket_refs(), 0);
    REQUIRE(leader->descriptor_table != NULL);
    CHECK_EQ(leader->descriptor_table->references, 1);
    CHECK_EQ(leader->descriptor_table->slots[7].type, FILE_DESCRIPTOR_SOCKET);

    /* And now the last one does. */
    scheduler_release_file_descriptors(leader);
    CHECK_EQ(fake_objects_socket_refs(), -1);
    CHECK_EQ(leader->descriptor_table, NULL);

    q13_kill(worker);
    q13_kill(leader);
}

/* A close-on-exec descriptor survives a thread and does NOT survive a
   spawn, which is the distinction task_spawn_common could not make before
   M146 - it ran one loop for both and cleared the flag either way. */
TEST(scheduler, close_on_exec_means_exec_and_not_thread_creation) {
    q13_boot();
    task_t *leader = q13_spawn("cloexec-leader");
    REQUIRE(leader != NULL);
    fake_objects_reset();
    leader->descriptor_table->slots[8].type = FILE_DESCRIPTOR_SOCKET;
    leader->descriptor_table->slots[8].sock = (struct socket *)0x5000;
    leader->descriptor_table->slots[8].cloexec = 1;

    task_t *worker = task_spawn_thread("cloexec-worker", leader, q13_body, (void *)0);
    REQUIRE(worker != NULL);
    CHECK_EQ(worker->descriptor_table->slots[8].type, FILE_DESCRIPTOR_SOCKET);
    CHECK_EQ(worker->descriptor_table->slots[8].cloexec, 1);

    scheduler_release_file_descriptors(worker);
    q13_kill(worker);

    /* A spawn is the exec those flags are about, so the slot is gone there
       and the table is a different one. */
    task_t *child = q13_spawn("cloexec-child");
    REQUIRE(child != NULL);
    CHECK(child->descriptor_table != leader->descriptor_table);
    CHECK_EQ(child->descriptor_table->references, 1);
    CHECK_EQ(child->descriptor_table->slots[8].type, FILE_DESCRIPTOR_NONE);
    CHECK_EQ(child->descriptor_table->slots[8].cloexec, 0);

    scheduler_release_file_descriptors(child);
    scheduler_release_file_descriptors(leader);
    q13_kill(child);
    q13_kill(leader);
}

TEST(scheduler, a_dying_task_gives_back_every_record_lock_it_held) {
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

    scheduler_release_file_descriptors(t);
    CHECK_EQ(flock_count(), before + 1);
    os_flock_t who;
    CHECK_EQ(flock_test(11, 0, OS_FLOCK_WR, 0, 10, &who), 0);
    CHECK_EQ(flock_test(11, 0, OS_FLOCK_WR, 50, 10, &who), 1);
    CHECK_EQ(who.pid, u->id);

    scheduler_release_file_descriptors(t);
    CHECK_EQ(flock_count(), before + 1);

    flock_release_pid(u->id);
    CHECK_EQ(flock_count(), before);
    q13_kill(t);
    q13_kill(u);
}

TEST(scheduler, a_fork_inherits_what_it_must_and_nothing_it_must_not) {
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

    task_t *saved = scheduler_current();
    (void)saved;
    isr_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.rax = 0xAAAA;

    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        placed = (scheduler_current() == parent);
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

TEST(scheduler, switching_away_from_a_stack_this_cpu_is_not_on_is_a_panic) {
    q13_boot();
    task_t *a = q13_spawn("a");
    task_t *b = q13_spawn("b");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);

    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        placed = (scheduler_current() == a);
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

TEST(scheduler, a_woken_task_still_current_on_another_cpu_is_not_picked) {
    q13_boot();
    task_t *w = q13_spawn("w");
    task_t *f = q13_spawn("f");
    REQUIRE(w != NULL);
    REQUIRE(f != NULL);

    static int cpu1_up;
    if (!cpu1_up) {
        cpu1_up = 1;
        fake_arch_set_cpu(1);
        scheduler_init_ap(1);
    }

    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(1);
        fake_arch_set_cpu(1);
        placed = (scheduler_current() == w);
    }
    REQUIRE(placed);

    w->state = TASK_BLOCKED;
    scheduler_wake_task(w);
    CHECK_EQ(w->state, TASK_READY);

    int stolen = 0;
    for (int i = 0; i < 40 * Q13_QUANTUM; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        if (scheduler_current() == w) {
            stolen = 1;
        }
    }
    CHECK_EQ(stolen, 0);

    w->state = TASK_BLOCKED;
    for (int i = 0; i < 4 * Q13_QUANTUM; i++) {
        q13_tick(1);
        fake_arch_set_cpu(1);
        if (scheduler_current() != w) {
            break;
        }
    }
    fake_arch_set_cpu(1);
    REQUIRE(scheduler_current() != w);
    scheduler_wake_task(w);
    int picked = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !picked; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        picked = (scheduler_current() == w);
    }
    CHECK_EQ(picked, 1);

    for (int i = 0; i < 200 * Q13_QUANTUM; i++) {
        fake_arch_set_cpu(0);
        if (scheduler_current() != w) {
            break;
        }
        w->state = TASK_READY;
        q13_tick(0);
    }
    for (int i = 0; i < 4 * Q13_QUANTUM; i++) {
        fake_arch_set_cpu(1);
        task_t *current = scheduler_current();
        if (!current || current->is_idle) {
            break;
        }
        current->state = TASK_BLOCKED;
        q13_tick(1);
    }
    q13_kill(w);
    f->state = TASK_READY;
    q13_kill(f);
}

TEST(scheduler, a_switch_loads_the_incoming_tasks_thread_pointer) {
    q13_boot();
    task_t *a = q13_spawn("tls-a");
    task_t *b = q13_spawn("tls-b");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    a->fs_base = 0xAAAA0000;
    b->fs_base = 0xBBBB0000;

    for (int i = 0; i < 400 * Q13_QUANTUM; i++) {
        q13_tick(0);
        task_t *current = scheduler_current();
        if (current == a) {
            CHECK_EQ(fake_cpu_last_msr(MSR_FS_BASE), 0xAAAA0000u);
        } else if (current == b) {
            CHECK_EQ(fake_cpu_last_msr(MSR_FS_BASE), 0xBBBB0000u);
        }
    }

    q13_kill(a);
    q13_kill(b);
}

TEST(scheduler, taking_two_locks_in_both_orders_is_caught) {

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

/* M175: a CPU's idle identity adopts the stack the AP was already standing on,
   so it records no stack of its own - and that is precisely what makes it
   unrunnable anywhere else. pick_next used to hand it to any CPU that ran out
   of work, because current_on_some_cpu only refuses a task somebody is running
   RIGHT NOW, not one whose stack belongs to another core. The CPU that took it
   then executed on another core's boot stack, and the damage surfaced much
   later, somewhere else, as a real task standing on an address in the kernel
   heap. Reaching it needs the taking CPU to have nothing of its own left to
   run, because pick_next prefers the incumbent to any idle. */
TEST(scheduler, an_idle_cpu_does_not_take_another_cpus_idle_identity) {
    q13_boot();

    static int cpu3_up;
    static task_t *cpu3_idle;
    if (!cpu3_up) {
        cpu3_up = 1;
        fake_arch_set_cpu(3);
        scheduler_init_ap(3);
        cpu3_idle = scheduler_current();
    }
    REQUIRE(cpu3_idle != NULL);
    REQUIRE(cpu3_idle->kernel_stack_top == 0);

    task_t *keeper = q13_spawn("m175keep");
    task_t *filler = q13_spawn("m175fill");
    REQUIRE(keeper != NULL);
    REQUIRE(filler != NULL);

    int left = 0;
    for (int i = 0; i < 400 * Q13_QUANTUM && !left; i++) {
        q13_tick(3);
        fake_arch_set_cpu(3);
        left = (scheduler_current() != cpu3_idle);
    }
    REQUIRE(left);
    REQUIRE(cpu3_idle->state == TASK_READY);

    fake_arch_set_cpu(3);
    task_t *on_three = scheduler_current();
    REQUIRE(on_three != NULL);
    REQUIRE(on_three != cpu3_idle);

    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (!t || t == cpu3_idle || t == on_three) {
            continue;
        }
        if (t->state == TASK_RUNNING || t->state == TASK_READY) {
            t->state = TASK_BLOCKED;
        }
    }

    int took_it = 0;
    for (int i = 0; i < 40 * Q13_QUANTUM && !took_it; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        took_it = (scheduler_current() == cpu3_idle);
    }
    CHECK_EQ(took_it, 0);

    /* And the other direction, which is the half a blanket refusal would
       break: cpu 3 must still be able to fall back to its OWN identity. */
    on_three->state = TASK_BLOCKED;
    for (int i = 0; i < 400 * Q13_QUANTUM; i++) {
        fake_arch_set_cpu(3);
        if (scheduler_current() == cpu3_idle) {
            break;
        }
        q13_tick(3);
    }
    fake_arch_set_cpu(3);
    CHECK_EQ(scheduler_current() == cpu3_idle, 1);

    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (t && t->state == TASK_BLOCKED) {
            t->state = TASK_READY;
        }
    }
    q13_kill(keeper);
    q13_kill(filler);
}

/* M178: there has to be an idle task for every CPU, and this is where that
   stops being a coincidence.

   A task that has marked itself TERMINATED calls schedule() and must never
   come back - task_exit_with_code panics if it does, and the panic is right,
   because the stack it would return onto is about to be freed. schedule()
   returns when pick_next hands back the task it was given, and for a
   terminated `from` the only way out of pick_next is an idle task. So a CPU
   that runs out of idle tasks turns an ordinary exit into a panic.

   Each AP can always fall back to the idle identity scheduler_init_ap gave it.
   The BOOT cpu has none - scheduler_init_ap runs only for application
   processors - so the migratable ones are all cpu 0 has, and there must be
   enough that every other CPU can be sitting on one and cpu 0 still finds a
   free one. That means one per CPU.

   It was two, from before this kernel had an SMP bringup at all: smp_init runs
   five hundred lines after the call, so the count could not have been right by
   anything but luck. Before M175 cpu 0 could take an AP's identity when it ran
   out, which is the stack-sharing bug M175 removed - removing it was right and
   it left cpu 0 with nothing to fall back on. */
TEST(scheduler, there_is_an_idle_task_for_every_cpu_to_fall_back_to) {
    q13_boot();
    scheduler_spawn_idle_tasks();

    int migratable = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (!t || t->state == TASK_FREE) {
            continue;
        }
        /* An AP's own identity is is_idle too, and is no use to another CPU -
           it is bound to the stack that CPU is standing on. Only the ones that
           allocated a stack of their own can be fallen back to from anywhere. */
        if (t->is_idle && t->home_cpu < 0) {
            migratable++;
        }
    }
    CHECK(migratable >= MAX_CPUS);
}

/* And the behaviour that count exists for: a CPU whose only remaining work has
   terminated leaves it, rather than being handed it back. */
TEST(scheduler, a_terminated_task_is_not_handed_back_to_its_own_cpu) {
    q13_boot();
    scheduler_spawn_idle_tasks();

    task_t *dying = q13_spawn("m178dying");
    REQUIRE(dying != NULL);

    int placed = 0;
    for (int i = 0; i < 400 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        placed = (scheduler_current() == dying);
    }
    REQUIRE(placed);

    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (!t || t == dying || t->is_idle) {
            continue;
        }
        if (t->state == TASK_RUNNING || t->state == TASK_READY) {
            t->state = TASK_BLOCKED;
        }
    }

    dying->state = TASK_TERMINATED;
    fake_arch_set_cpu(0);
    fake_arch_stand_on(dying->kernel_stack_top - 64);
    schedule();

    fake_arch_set_cpu(0);
    CHECK(scheduler_current() != dying);

    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (t && t->state == TASK_BLOCKED) {
            t->state = TASK_READY;
        }
    }
    scheduler_reap_slot(dying);
}

/* M182: the other end of M106's question. That one fires when a CPU is
   standing somewhere the task it thinks it is running does not cover, which is
   where the damage is SEEN - the bad stack pointer it complains about was put
   into that task by an earlier context_switch saving it, so by the time it
   fires the first event is long gone and the only thing named is the victim.

   This one looks at a task's saved stack pointer BEFORE jumping to it. A task
   whose rsp is not inside its own stack has never run there, so the corruption
   is already in the task rather than in what the CPU is doing, and the dump
   names whichever task's stack the bad pointer DOES fall in - which is the
   difference between two cores having shared a stack and a stack having been
   freed and handed out again. */
TEST(scheduler, a_saved_stack_pointer_outside_its_own_task_is_caught_before_it_runs) {
    q13_boot();
    scheduler_spawn_idle_tasks();

    task_t *runner = q13_spawn("m182run");
    task_t *victim = q13_spawn("m182victim");
    REQUIRE(runner != NULL);
    REQUIRE(victim != NULL);

    int placed = 0;
    for (int i = 0; i < 400 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        placed = (scheduler_current() == runner);
    }
    REQUIRE(placed);

    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (!t || t == runner || t == victim) {
            continue;
        }
        if (t->state == TASK_RUNNING || t->state == TASK_READY) {
            t->state = TASK_BLOCKED;
        }
    }
    victim->state = TASK_READY;

    /* Inside runner's stack rather than nowhere at all, so what is being
       caught is "this is not YOUR stack" and not "this is not a stack". */
    uint64_t borrowed = runner->kernel_stack_top - 64;
    int borrowed_is_the_victims = borrowed >= (uint64_t)(uintptr_t)victim->stack_base &&
                                  borrowed < victim->kernel_stack_top;
    REQUIRE(!borrowed_is_the_victims);
    victim->rsp = borrowed;

    fake_arch_set_cpu(0);
    fake_arch_stand_on(runner->kernel_stack_top - 64);
    CHECK_PANIC(schedule(), "not in its own stack");
    fake_spinlock_release_all();

    victim->rsp = victim->kernel_stack_top - 64;
    victim->state = TASK_BLOCKED;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (t && t->state == TASK_BLOCKED) {
            t->state = TASK_READY;
        }
    }
    q13_kill(runner);
    q13_kill(victim);
}

/* M192. A task that reads which processor it is on with interrupts still
   enabled can be moved by the next tick and go on using the old answer: it
   then treats the other processor's running task as its own, saves its stack
   pointer into it and points that processor's tss.rsp0 at the next task's
   stack. That was the four-core bug M172 to M182 chased. No entry point may
   ask the question while the answer can still change. */
TEST(scheduler, no_entry_point_asks_which_cpu_it_is_on_while_it_can_still_be_moved) {
    q13_boot();
    task_t *a = q13_spawn("migrant-a");
    task_t *b = q13_spawn("migrant-b");
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        placed = (scheduler_current() == a);
    }
    REQUIRE(placed);
    fake_arch_set_cpu(0);
    fake_arch_stand_on(a->kernel_stack_top - 64);

    fake_arch_reset_unguarded_cpu_reads();
    CHECK(scheduler_current() == a);
    CHECK_EQ(fake_arch_unguarded_cpu_reads(), 0);

    fake_arch_reset_unguarded_cpu_reads();
    (void)scheduler_signal_pending();
    CHECK_EQ(fake_arch_unguarded_cpu_reads(), 0);

    fake_arch_reset_unguarded_cpu_reads();
    schedule();
    CHECK_EQ(fake_arch_unguarded_cpu_reads(), 0);

    fake_arch_set_cpu(0);
    task_t *now = scheduler_current();
    fake_arch_stand_on(now->kernel_stack_top ? now->kernel_stack_top - 64 : 0);
    static spinlock_t held;
    uint64_t flags = spin_lock_irqsave(&held);
    fake_arch_reset_unguarded_cpu_reads();
    scheduler_block_on(&held, 0, &held, &flags);
    CHECK_EQ(fake_arch_unguarded_cpu_reads(), 0);
    spin_unlock_irqrestore(&held, flags);

    fake_arch_reset_unguarded_cpu_reads();
    q13_kill(a);
    q13_kill(b);
}
