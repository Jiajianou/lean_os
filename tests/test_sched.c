#include "check.h"
#include "fakes/fakes.h"

#include "architecture/x86_64/cpu.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"
#include "drivers/pit.h"
#include "library/spinlock.h"
#include "file_system/flock.h"
#include "inter_process_communication/shared_memory.h"
#include "memory_management/heap.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"
#include "mman.h"
#include "scheduler/scheduler.h"
#include "signal.h"
#include "syscall.h"

#include <string.h>

#define Q13_QUANTUM 2

void q13_boot(void);
task_t *q13_spawn(const char *name);
void q13_kill(task_t *t);
task_t *q13_spawn_program(const char *name);

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

/* M225: a task standing for a PROGRAM - one in an address space of its own.
   q13_spawn's tasks run in the kernel's (the fakes' kernel page table is 0),
   which makes them kernel threads, and since M225 no signal a program sends
   to a group reaches a kernel thread in it. A test about what programs do to
   each other's groups needs programs. */
task_t *q13_spawn_program(const char *name) {
    static uint64_t next_space = 0x7000000;
    next_space += 0x1000;
    return task_spawn_in(name, next_space, q13_body, (void *)0, 0, 0);
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

/* M225 (process-lifetimes): a thread is made the way the kernel makes one -
   by a task running in its process (task_spawn_thread refuses anybody
   else) - so the test sits `of` in this processor's seat for the call and
   puts back whoever was there. */
task_t *scheduler_host_test_seat(int cpu, task_t *t);

static task_t *q13_thread_of(const char *name, task_t *of) {
    int cpu = smp_current_cpu();
    task_t *was = scheduler_host_test_seat(cpu, of);
    task_t *thread = task_spawn_thread(name, of, q13_body, (void *)0);
    scheduler_host_test_seat(cpu, was);
    return thread;
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
    CHECK_EQ(MAX_TASKS, 512);
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

TEST(scheduler, a_finished_thread_nobody_will_join_gives_its_slot_back) {
    q13_boot();
    task_t *leader = q13_spawn("pool");
    REQUIRE(leader != NULL);
    task_t *worker = q13_thread_of("worker", leader);
    task_t *joinable = q13_thread_of("joinable", leader);
    REQUIRE(worker != NULL);
    REQUIRE(joinable != NULL);
    int worker_id = worker->id;
    int joinable_id = joinable->id;
    CHECK_EQ(scheduler_detach_thread(leader, worker_id), 0);

    worker->state = TASK_TERMINATED;
    joinable->state = TASK_TERMINATED;
    scheduler_release_finished_tasks();

    CHECK(scheduler_task_by_id(worker_id) == NULL);
    CHECK(scheduler_task_by_id(joinable_id) == joinable);

    scheduler_reap_slot(joinable);
    q13_kill(leader);
}

TEST(scheduler, the_threads_of_a_process_that_died_are_nobodys_to_join) {
    q13_boot();
    int live_before = scheduler_live_task_count();
    task_t *leader = q13_spawn("gone");
    REQUIRE(leader != NULL);
    task_t *threads[5];
    for (int i = 0; i < 5; i++) {
        threads[i] = q13_thread_of("orphan", leader);
        REQUIRE(threads[i] != NULL);
    }
    for (int i = 0; i < 5; i++) {
        threads[i]->state = TASK_TERMINATED;
    }
    CHECK_EQ(scheduler_live_task_count(), live_before + 6);
    scheduler_release_finished_tasks();
    CHECK_EQ(scheduler_live_task_count(), live_before + 6);

    leader->state = TASK_TERMINATED;
    scheduler_release_finished_tasks();
    CHECK_EQ(scheduler_live_task_count(), live_before);
}

TEST(scheduler, only_a_thread_of_ones_own_process_can_be_detached) {
    q13_boot();
    task_t *mine = q13_spawn("mine");
    task_t *theirs = q13_spawn("theirs");
    REQUIRE(mine != NULL);
    REQUIRE(theirs != NULL);
    task_t *their_thread = q13_thread_of("their-thread", theirs);
    REQUIRE(their_thread != NULL);

    CHECK_EQ(scheduler_detach_thread(mine, their_thread->id), -1);
    CHECK_EQ(scheduler_detach_thread(mine, theirs->id), -1);
    CHECK_EQ(scheduler_detach_thread(theirs, their_thread->id), 0);
    CHECK_EQ(scheduler_detach_thread(theirs, 0x7FFFFF00), -1);

    their_thread->state = TASK_TERMINATED;
    scheduler_reap_slot(their_thread);
    q13_kill(theirs);
    q13_kill(mine);
}

TEST(scheduler, a_group_signal_reaches_every_member_and_nobody_else) {
    q13_boot();
    task_t *in1 = q13_spawn_program("in1");
    task_t *in2 = q13_spawn_program("in2");
    task_t *out = q13_spawn_program("out");
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

/* M204: execve loaded a new program's tables without saying so, and every
   TLB shootdown for that program skipped the core it was running on. The
   record shootdowns read has to move with the load - away from the tables
   being replaced and onto the ones replacing them. */
TEST(scheduler, loading_an_address_space_is_what_a_shootdown_sees) {
    q13_boot();
    task_t *t = q13_spawn("as-exec");
    REQUIRE(t != NULL);
    fake_arch_set_cpu(1);
    t->pml4_phys = 0x7D000;
    scheduler_load_address_space(t, 0x7D000);
    CHECK_EQ(scheduler_cpus_holding_address_space(0x7D000) & 2u, 2u);
    scheduler_load_address_space(t, 0x7E000);
    CHECK_EQ(t->pml4_phys, 0x7E000u);
    CHECK_EQ(scheduler_cpus_holding_address_space(0x7E000) & 2u, 2u);
    CHECK_EQ(scheduler_cpus_holding_address_space(0x7D000) & 2u, 0u);
    scheduler_forget_address_space(0x7E000);
    t->pml4_phys = 0;
    fake_arch_set_cpu(0);
    q13_kill(t);
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
    const uint64_t exits_before = scheduler_exit_sequence();
    scheduler_reap_slot(leader);
    /* M203: and says nothing - no program has gone. Announcing every held
       reap woke every poller on the machine for nothing. */
    CHECK_EQ(scheduler_exit_sequence(), exits_before);

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
    CHECK(scheduler_exit_sequence() > exits_before);
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
    task_t *worker = q13_thread_of("worker", leader);
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

/* M225. Another task's table is read under scheduler_lock - /proc's task
   list counts every task's open descriptors - and that is only safe if a
   task stops naming its table under the same lock BEFORE the table can be
   freed. It used to drop the reference first and clear the pointer after,
   so a reader on another processor could follow the pointer into a table
   being closed and freed. The release hook stands for that reader: it runs
   the moment any lock is let go, and once the descriptors have started to
   close, the dying task must no longer name the table. */
static task_t *m225_dying;
static int m225_named_while_closing;

static void m225_reader_on_another_cpu(void) {
    if (fake_objects_pipe_read_refs() < 0 && m225_dying->descriptor_table != NULL) {
        m225_named_while_closing++;
    }
}

TEST(scheduler, a_task_stops_naming_its_table_before_the_table_is_freed) {
    q13_boot();
    m225_dying = q13_spawn("dying");
    REQUIRE(m225_dying != NULL);
    fake_objects_reset();
    m225_dying->descriptor_table->slots[3].type = FILE_DESCRIPTOR_PIPE_READ;
    m225_dying->descriptor_table->slots[3].pipe = (struct pipe *)0x1000;
    CHECK_EQ(scheduler_open_descriptor_count(m225_dying), 4);

    m225_named_while_closing = 0;
    fake_spinlock_on_release(m225_reader_on_another_cpu);
    scheduler_release_file_descriptors(m225_dying);
    fake_spinlock_on_release(0);

    CHECK_EQ(fake_objects_pipe_read_refs(), -1);
    CHECK_EQ(m225_named_while_closing, 0);
    /* And a task that has let its table go - every zombie - has nothing
       open, rather than whatever a null table's slots read as. */
    CHECK_EQ(m225_dying->descriptor_table, NULL);
    CHECK_EQ(scheduler_open_descriptor_count(m225_dying), 0);
    CHECK_EQ(scheduler_open_descriptor_count(NULL), 0);
    q13_kill(m225_dying);
}

/* M225: a reference is only taken by a task that holds one, so a count of
   zero there is a table already closed - and a release past zero is a
   second close. Both are bugs elsewhere that the count would otherwise
   absorb, so both stop the machine where they happen. */
TEST(scheduler, a_descriptor_table_count_never_rises_from_or_falls_below_zero) {
    q13_boot();
    file_descriptor_table_t *table = descriptor_table_new();
    REQUIRE(table != NULL);
    table->references = 0;
    CHECK_PANIC(descriptor_table_reference(table), "a table nobody holds");
    table->references = 0;
    CHECK_PANIC(descriptor_table_release(table), "released more often");
    table->references = 1;
    descriptor_table_release(table);
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

    task_t *worker = q13_thread_of("cloexec-worker", leader);
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

/* M225: and what the count is FOR can be taken away by a signal. kill(0)
   from anything in process group 0 - the kernel and everything it starts -
   reached every idle task, each one exited at its next tick, and the last
   CPU to need one found none: the battery's "task_exit: terminated task
   resumed" at the M105 stage, with every idle task in the table marked
   TERMINATED. Every route a signal takes ends in scheduler_raise_signal, so
   this asks it directly, with every kind of disposition. */
TEST(scheduler, no_signal_reaches_an_idle_task) {
    q13_boot();
    scheduler_spawn_idle_tasks();

    int idles = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (!t || !t->is_idle || t->state == TASK_TERMINATED) {
            continue;
        }
        idles++;
        scheduler_raise_signal(t, SIGKILL);
        scheduler_raise_signal(t, SIGTERM);
        scheduler_raise_signal(t, SIGUSR1);
        scheduler_raise_signal(t, SIGSTOP);
        scheduler_raise_signal(t, SIGTSTP);
        CHECK_EQ(t->pending_signal, 0);
        CHECK_EQ(t->pending_stop, 0);
        CHECK_EQ(t->sig_pending, 0u);
        /* So that a failure here does not go on to end idle tasks in the
           tests after it. */
        t->pending_signal = 0;
        t->pending_stop = 0;
        t->sig_pending = 0;
    }
    CHECK(idles >= MAX_CPUS);
}

/* M225: the other half of that boot. M105 spawns four writers and then waits
   for each - and a writer that finished before the fourth spawn was released
   by that spawn's sweep, because a child of the kernel is nobody_will_wait.
   Its slot was gone, the self-test read its exit code out of a null task, and
   reaped "pid 0". The slot cannot be kept - the kernel's self-tests that never
   wait rely on the sweep - so the exit code has to outlive it. */
static void m225_exit_on_cpu0(task_t *child, int code, int signal) {
    int placed = 0;
    for (int i = 0; i < 400 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        placed = (scheduler_current() == child);
    }
    REQUIRE(placed);
    fake_arch_set_cpu(0);
    fake_arch_stand_on(child->kernel_stack_top - 64);
    if (signal) {
        CHECK_PANIC(task_exit_with_signal(signal), "terminated task resumed");
    } else {
        CHECK_PANIC(task_exit_with_code(code), "terminated task resumed");
    }
    fake_spinlock_release_all();
    CHECK_EQ(child->state, TASK_TERMINATED);
}

TEST(scheduler, a_child_the_sweep_released_still_says_how_it_ended) {
    q13_boot();
    scheduler_spawn_idle_tasks();

    task_t *quits = q13_spawn("m225quits");
    task_t *killed = q13_spawn("m225killed");
    REQUIRE(quits != NULL);
    REQUIRE(killed != NULL);
    quits->parent_id = 0;
    killed->parent_id = 0;
    int quits_pid = quits->id;
    int killed_pid = killed->id;

    int code = -1;
    CHECK_EQ(scheduler_exit_code(quits_pid, &code), 0);

    m225_exit_on_cpu0(quits, 7, 0);
    m225_exit_on_cpu0(killed, 0, SIGTERM);
    CHECK_EQ(scheduler_exit_code(quits_pid, &code), 1);
    CHECK_EQ(code, 7);

    task_t *next = q13_spawn("m225next");
    REQUIRE(next != NULL);
    CHECK(scheduler_task_by_id(quits_pid) == NULL);
    CHECK(scheduler_task_by_id(killed_pid) == NULL);

    code = -1;
    CHECK_EQ(scheduler_exit_code(quits_pid, &code), 1);
    CHECK_EQ(code, 7);
    code = -1;
    CHECK_EQ(scheduler_exit_code(killed_pid, &code), 1);
    CHECK_EQ(code, 128 + SIGTERM);
    /* Nothing knows this id: -1, which is not "still running". */
    CHECK_EQ(scheduler_exit_code(0x7FFFFF00, &code), -1);

    q13_kill(next);
}

/* M225, the reviewer's third point: one answer, in one encoding. The slot
   path answered the whole exit_code and the record path its low eight bits,
   so a process that called exit(256) read 256 while it was a zombie and 0
   once the sweep had released it - waitpid says 0 both times. And a slot
   released and refilled is not the process that was asked about: the answer
   comes from the record, not from the newcomer. */
TEST(scheduler, an_exit_code_reads_the_same_from_the_slot_and_from_the_record) {
    q13_boot();
    scheduler_spawn_idle_tasks();

    task_t *wide = q13_spawn("m225wide");
    task_t *shot = q13_spawn("m225shot");
    REQUIRE(wide != NULL);
    REQUIRE(shot != NULL);
    wide->parent_id = 0;
    shot->parent_id = 0;
    const int wide_pid = wide->id;
    const int shot_pid = shot->id;
    const int wide_slot = PID_SLOT(wide_pid);

    m225_exit_on_cpu0(wide, 256, 0);
    m225_exit_on_cpu0(shot, 0, SIGKILL);

    int code = -1;
    int status = -1;
    CHECK(scheduler_task_by_id(wide_pid) != NULL);
    CHECK_EQ(scheduler_exit_code(wide_pid, &code), 1);
    CHECK_EQ(code, 0);
    CHECK_EQ(scheduler_exit_status(wide_pid, &status), 1);
    CHECK_EQ(status, 0);
    CHECK_EQ(scheduler_exit_code(shot_pid, &code), 1);
    CHECK_EQ(code, 128 + SIGKILL);
    CHECK_EQ(scheduler_exit_status(shot_pid, &status), 1);
    CHECK_EQ(status, SIGKILL);

    /* The sweep releases both (children of the kernel), and spawning goes on
       until somebody new is standing in the exit(256) task's slot. */
    task_t *spawned[MAX_TASKS];
    int nspawned = 0;
    task_t *heir = (task_t *)0;
    while (nspawned < MAX_TASKS) {
        task_t *t = q13_spawn("m225heir");
        if (!t) {
            break;
        }
        spawned[nspawned++] = t;
        if (PID_SLOT(t->id) == wide_slot) {
            heir = t;
            break;
        }
    }
    REQUIRE(heir != NULL);
    CHECK(scheduler_task_by_id(wide_pid) == NULL);
    CHECK(scheduler_task_by_id(shot_pid) == NULL);

    code = -1;
    CHECK_EQ(scheduler_exit_code(wide_pid, &code), 1);
    CHECK_EQ(code, 0);
    code = -1;
    CHECK_EQ(scheduler_exit_code(shot_pid, &code), 1);
    CHECK_EQ(code, 128 + SIGKILL);
    CHECK_EQ(scheduler_exit_code(heir->id, &code), 0);
    CHECK_EQ(scheduler_exit_code_of_status(0x7F00), 0x7F);
    CHECK_EQ(scheduler_exit_code_of_status(SIGSEGV), 128 + SIGSEGV);

    for (int i = 0; i < nspawned; i++) {
        q13_kill(spawned[i]);
    }
}

/* M225, the reviewer's first point: no signal a program sends reaches the
   kernel, by any of the ways kill(2) names a target. */
/* The permission rule that admits everybody - CAP_KILL_ANY, which sh,
   gui_terminal, desktop_icons and the compositor all hold. The rule about
   the kernel must hold even then. */
static int m225_anyone(task_t *self, task_t *target) {
    (void)self;
    (void)target;
    return 1;
}

static int m225_ours_or_kernel(task_t *self, task_t *target) {
    (void)self;
    return strncmp(target->name, "m225", 4) == 0 || scheduler_task_is_kernel(target);
}

static void m225_quiet(task_t *t) {
    t->pending_signal = 0;
    t->pending_stop = 0;
    t->sig_pending = 0;
}

static int m225_untouched(const task_t *t) {
    return t->pending_signal == 0 && t->pending_stop == 0 && t->sig_pending == 0;
}

/* Every kernel task this test can see - the boot task, each idle task and
   the kernel threads it started - and whether a signal reached any of them. */
static int m225_kernel_untouched(task_t *const *extra, int nextra) {
    task_t *boot = scheduler_task_by_id(0);
    if (!boot || !m225_untouched(boot)) {
        return 0;
    }
    for (int i = 0; i < scheduler_task_count(); i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (t && t->is_idle && t->state != TASK_TERMINATED && !m225_untouched(t)) {
            return 0;
        }
    }
    for (int i = 0; i < nextra; i++) {
        if (!m225_untouched(extra[i])) {
            return 0;
        }
    }
    return 1;
}

TEST(scheduler, a_program_the_kernel_starts_leads_a_group_of_its_own) {
    q13_boot();
    fake_arch_set_cpu(0);
    REQUIRE(scheduler_task_is_kernel(scheduler_current()));

    task_t *program = q13_spawn_program("m225prog");
    task_t *kthread = q13_spawn("m225kthread");
    REQUIRE(program != NULL);
    REQUIRE(kthread != NULL);
    task_t *thread = q13_thread_of("m225thread", program);
    REQUIRE(thread != NULL);

    CHECK(!scheduler_task_is_kernel(program));
    CHECK_EQ(program->pgid, program->id);
    CHECK_EQ(program->sid, program->id);
    CHECK(scheduler_task_is_kernel(kthread));
    CHECK_EQ(kthread->pgid, 0);
    CHECK_EQ(kthread->sid, 0);
    CHECK(scheduler_task_is_kernel(scheduler_task_by_id(0)));
    CHECK(!scheduler_task_is_kernel(thread));
    CHECK_EQ(thread->pgid, program->pgid);
    CHECK_EQ(thread->sid, program->sid);

    q13_kill(thread);
    q13_kill(kthread);
    q13_kill(program);
}

TEST(scheduler, no_kill_a_program_sends_reaches_the_kernel) {
    q13_boot();
    scheduler_spawn_idle_tasks();
    fake_arch_set_cpu(0);
    REQUIRE(scheduler_task_is_kernel(scheduler_current()));

    task_t *kernel = scheduler_task_by_id(0);
    REQUIRE(kernel != NULL);
    task_t *leader = q13_spawn_program("m225leader");
    task_t *child = q13_spawn_program("m225child");
    task_t *stranger = q13_spawn_program("m225stranger");
    task_t *journal = q13_spawn("m225journal");
    REQUIRE(leader != NULL);
    REQUIRE(child != NULL);
    REQUIRE(stranger != NULL);
    REQUIRE(journal != NULL);
    /* What a fork or a spawn by `leader` gives its child. */
    child->parent_id = leader->tgid;
    child->lineage_id = leader->tgid;
    child->pgid = leader->pgid;
    child->sid = leader->sid;
    task_t *kernel_threads[] = {journal};

    /* kill(pid) of a kernel thread, with CAP_KILL_ANY. */
    CHECK_EQ(scheduler_kill(leader, journal->id, SIGKILL, m225_anyone), -OS_ERROR_PERMISSION);
    CHECK(m225_kernel_untouched(kernel_threads, 1));

    /* kill(0): the program's own group, which is it and its child. */
    CHECK_EQ(scheduler_kill(leader, 0, SIGKILL, m225_anyone), 0);
    CHECK_EQ(leader->pending_signal, SIGKILL);
    CHECK_EQ(child->pending_signal, SIGKILL);
    CHECK(m225_untouched(stranger));
    CHECK(m225_kernel_untouched(kernel_threads, 1));
    m225_quiet(leader);
    m225_quiet(child);

    /* kill(-pgid), the same group by name. */
    CHECK_EQ(scheduler_kill(leader, -(long)leader->pgid, SIGTERM, m225_anyone), 0);
    CHECK_EQ(child->pending_signal, SIGTERM);
    CHECK(m225_untouched(stranger));
    CHECK(m225_kernel_untouched(kernel_threads, 1));
    m225_quiet(leader);
    m225_quiet(child);

    /* kill(-1): everybody it may signal but itself, and never the kernel.
       The permission rule here admits this test's tasks and every kernel
       task, so that only the rule about the kernel stands between kill(-1)
       and the kernel - and nothing other tests left in the table is hit. */
    CHECK_EQ(scheduler_kill(leader, -1, SIGKILL, m225_ours_or_kernel), 0);
    CHECK(m225_untouched(leader));
    CHECK_EQ(child->pending_signal, SIGKILL);
    CHECK_EQ(stranger->pending_signal, SIGKILL);
    CHECK(m225_kernel_untouched(kernel_threads, 1));
    m225_quiet(child);
    m225_quiet(stranger);

    /* A kernel thread standing in a program's group - wifi-dhcp, started
       inside a program's system call, used to inherit its group - is still
       nobody's target: not by kill(0), and not by the terminal's ^C. */
    journal->pgid = leader->pgid;
    CHECK_EQ(scheduler_kill(leader, 0, SIGKILL, m225_anyone), 0);
    scheduler_raise_signal_group(leader->pgid, SIGINT);
    CHECK(m225_kernel_untouched(kernel_threads, 1));
    journal->pgid = 0;
    m225_quiet(leader);
    m225_quiet(child);

    /* Process group 0 is nobody's: not the kernel's own kill(0) - which is
       what selftest_reap(NULL) sent - nor a kernel thread's, nor kill(-0). */
    CHECK_EQ(scheduler_kill(kernel, 0, SIGKILL, m225_anyone), -OS_ERROR_SEARCH);
    CHECK_EQ(scheduler_kill(journal, 0, SIGKILL, m225_anyone), -OS_ERROR_SEARCH);
    CHECK_EQ(scheduler_kill(kernel, -0, SIGKILL, m225_anyone), -OS_ERROR_SEARCH);
    scheduler_raise_signal_group(0, SIGKILL);
    CHECK(m225_kernel_untouched(kernel_threads, 1));
    CHECK(m225_untouched(leader));
    CHECK(m225_untouched(stranger));

    /* A pid wider than an int is not the int it truncates to. */
    CHECK_EQ(scheduler_kill(leader, 0x100000000L + stranger->id, SIGKILL, m225_anyone),
             -OS_ERROR_SEARCH);
    CHECK_EQ(scheduler_kill(leader, -0x100000000L - stranger->pgid, SIGKILL, m225_anyone),
             -OS_ERROR_SEARCH);
    CHECK(m225_untouched(stranger));

    /* The kernel still signals the kernel threads it starts (the boot
       self-tests' spinners), and anyone - signal 0 asks only. */
    CHECK_EQ(scheduler_kill(kernel, journal->id, SIGTERM, m225_anyone), 0);
    CHECK_EQ(journal->pending_signal, SIGTERM);
    m225_quiet(journal);
    CHECK_EQ(scheduler_kill(leader, stranger->id, 0, m225_anyone), 0);
    CHECK_EQ(scheduler_kill(leader, stranger->id, SIG_MAX + 1, m225_anyone), -OS_ERROR_INVALID);

    q13_kill(journal);
    q13_kill(stranger);
    q13_kill(child);
    q13_kill(leader);
}

/* M225: and not by a slot that changed hands in the middle of the kill.
   scheduler_kill reads the table without the scheduler lock: it finds the
   target, asks whether it is the kernel's and whether the caller may signal
   it, and only then raises. A target that ends, is swept and has its slot
   refilled by a kernel thread in between used to hand the signal to that
   kernel thread, because the raise was given the slot and asked only
   whether the slot was alive. The permission check is the "other processor"
   here: it runs between the look and the raise, and refills the slot. */
static task_t *m225_victim;
static task_t *m225_refill;
static task_t *m225_spare[MAX_TASKS];
static int m225_spares;
/* M225: refill with an ordinary program instead of a kernel thread, so that
   the rule about the kernel cannot be what refuses it. */
static int m225_refill_with_program;

static int m225_slot_changes_hands(task_t *self, task_t *target) {
    (void)self;
    if (target != m225_victim || m225_refill) {
        return 1;
    }
    q13_kill(m225_victim);
    /* The next kernel thread takes the first free slot, which may be below
       the victim's; spawn until one lands where the victim was. */
    for (int i = 0; i < MAX_TASKS && !m225_refill; i++) {
        task_t *t = m225_refill_with_program ? q13_spawn_program("m225refill")
                                             : q13_spawn("m225refill");
        if (!t) {
            break;
        }
        if (t == m225_victim) {
            m225_refill = t;
        } else {
            m225_spare[m225_spares++] = t;
        }
    }
    return 1;
}

static void m225_slot_test_reset(void) {
    for (int i = 0; i < m225_spares; i++) {
        q13_kill(m225_spare[i]);
    }
    m225_spares = 0;
    if (m225_refill) {
        q13_kill(m225_refill);
    }
    m225_refill = 0;
    m225_victim = 0;
}

TEST(scheduler, a_kill_whose_target_is_replaced_mid_kill_reaches_nobody) {
    q13_boot();
    fake_arch_set_cpu(0);
    task_t *sender = q13_spawn_program("m225sender");
    REQUIRE(sender != NULL);

    /* kill(pid), kill(-pgid) and kill(-1): each names the victim, and each
       finds a kernel thread in its slot by the time it raises. */
    const long how[] = {1, -2, -1};
    for (int k = 0; k < 3; k++) {
        m225_victim = q13_spawn_program("m225victim");
        REQUIRE(m225_victim != NULL);
        long pid = how[k] == 1 ? (long)m225_victim->id
                 : how[k] == -2 ? -(long)m225_victim->pgid
                 : -1;
        long rc = scheduler_kill(sender, pid, SIGKILL, m225_slot_changes_hands);
        REQUIRE(m225_refill != NULL);
        CHECK(scheduler_task_is_kernel(m225_refill));
        CHECK(m225_untouched(m225_refill));
        if (how[k] != -1) {
            /* The only target it named is gone; -1 may have found others. */
            CHECK_EQ(rc, -OS_ERROR_SEARCH);
        }
        m225_slot_test_reset();
    }
    q13_kill(sender);
}

/* M225: what a spawner learns about its child is recorded on the SPAWNER,
   under the lock the slot is filled under. The task_t the spawn returns is
   the child's only while the child lives: a child of the kernel that ends
   at once is swept by the next spawn on any processor, and its slot handed
   on - and the boot self-tests read the child's id out of that task_t after
   the spawn had returned, which is the window the original panic came
   through. Here the child ends and another processor's spawn takes its slot
   before the spawner looks. */
TEST(scheduler, a_spawner_knows_its_child_after_the_slot_has_moved_on) {
    q13_boot();
    scheduler_spawn_idle_tasks();
    fake_arch_set_cpu(0);
    task_t *spawner = scheduler_current();
    REQUIRE(spawner != NULL);

    task_t *child = q13_spawn("m225quick");
    REQUIRE(child != NULL);
    int child_id = child->id;
    uint32_t caps = 0;
    CHECK_EQ(scheduler_last_spawn(&caps), child_id);
    CHECK_EQ(caps, child->caps);

    /* It ends and is swept; another processor's spawn refills the slot. */
    q13_kill(child);
    fake_arch_set_cpu(1);
    task_t *other_spawner = scheduler_current();
    REQUIRE(other_spawner != NULL);
    REQUIRE(other_spawner != spawner);
    task_t *stranger = (task_t *)0;
    task_t *spare[MAX_TASKS];
    int spares = 0;
    for (int i = 0; i < MAX_TASKS && !stranger; i++) {
        task_t *t = q13_spawn("m225stranger2");
        if (!t) {
            break;
        }
        if (t == child) {
            stranger = t;
        } else {
            spare[spares++] = t;
        }
    }
    REQUIRE(stranger != NULL);
    fake_arch_set_cpu(0);

    /* The pointer now names the stranger; the record still names the child. */
    CHECK(child->id != child_id);
    CHECK_EQ(scheduler_last_spawn((uint32_t *)0), child_id);
    CHECK(scheduler_task_by_id(child_id) == NULL);

    for (int i = 0; i < spares; i++) {
        q13_kill(spare[i]);
    }
    q13_kill(stranger);
}

/* M225: a process has ended when the last of its tasks has, and not before.
   wait() answered for the leader alone, so a parent's wait() returned - and
   [m205]'s sweep counted slots - while exit()'s SIGKILL was still on its way
   to the process's other threads on other processors; and SIGCHLD went out
   when the leader ended, which a parent answering it with WNOHANG would now
   find too early and never hear about again. Both move to the moment the
   group is empty, and whichever task is last says so. */
TEST(scheduler, a_process_ends_when_its_last_thread_does) {
    q13_boot();
    scheduler_spawn_idle_tasks();
    fake_arch_set_cpu(0);

    task_t *parent = q13_spawn_program("m225parent");
    task_t *leader = q13_spawn("m225leader2");
    REQUIRE(parent != NULL);
    REQUIRE(leader != NULL);
    task_t *thread = q13_thread_of("m225worker", leader);
    REQUIRE(thread != NULL);
    leader->parent_id = parent->id;
    parent->sig_handler[SIGCHLD] = 0x1000;
    parent->sig_pending = 0;
    parent->si_pid = 0;
    int leader_id = leader->id;

    /* The leader returns first; its thread is still running. */
    m225_exit_on_cpu0(leader, 3, 0);
    CHECK(!scheduler_process_has_ended(leader));
    CHECK_EQ(scheduler_exit_code(leader_id, (int *)0), 0);
    CHECK_EQ(parent->sig_pending & (1u << SIGCHLD), 0u);

    /* The last of it goes, and that is when the parent hears. */
    m225_exit_on_cpu0(thread, 137, SIGKILL);
    CHECK(scheduler_process_has_ended(leader));
    int code = -1;
    CHECK_EQ(scheduler_exit_code(leader_id, &code), 1);
    CHECK_EQ(code, 3);
    CHECK(parent->sig_pending & (1u << SIGCHLD));
    CHECK_EQ(parent->si_pid, leader_id);
    CHECK_EQ(parent->si_status, 3);

    parent->sig_handler[SIGCHLD] = SIG_DFL_ADDR;
    parent->sig_pending = 0;
    scheduler_reap_slot(thread);
    scheduler_reap_slot(leader);
    q13_kill(parent);
}

/* M225: a task on its way out takes no more signals. A process ended by
   SIGTERM sends SIGKILL to its other threads; each of them, dying of it,
   sends SIGKILL to every task of the group that is not yet terminated -
   which includes the leader, still in task_exit tearing itself down. The
   next timer tick delivered it and the exit began again from the top, and
   waitpid then said SIGKILL: forktest's silent "exited 17" on four
   processors. Here the sibling's SIGKILL and the tick arrive while the
   leader is inside its exit. */
static task_t *m225_ending;

static void m225_sibling_dies_mid_exit(int owner) {
    (void)owner;
    scheduler_raise_signal(m225_ending, SIGKILL);
    scheduler_tick_cpu(0);
}

TEST(scheduler, a_task_that_is_exiting_takes_no_more_signals) {
    q13_boot();
    scheduler_spawn_idle_tasks();
    fake_arch_set_cpu(0);
    task_t *victim = q13_spawn("m225sigterm");
    REQUIRE(victim != NULL);
    victim->parent_id = 0;
    int victim_id = victim->id;

    m225_ending = victim;
    fake_objects_on_shared_memory_free(m225_sibling_dies_mid_exit);
    m225_exit_on_cpu0(victim, 0, SIGTERM);
    fake_objects_on_shared_memory_free(0);

    int status = -1;
    CHECK_EQ(scheduler_exit_status(victim_id, &status), 1);
    CHECK_EQ(status, SIGTERM);
    CHECK_EQ(victim->exit_signal, SIGTERM);
    CHECK_EQ(victim->pending_signal, 0);
    scheduler_reap_slot(victim);
}

TEST(scheduler, a_program_cannot_wait_on_the_kernel_or_itself) {
    q13_boot();
    fake_arch_set_cpu(0);
    task_t *kernel = scheduler_task_by_id(0);
    task_t *program = q13_spawn_program("m225waiter");
    task_t *other = q13_spawn_program("m225waited");
    task_t *kthread = q13_spawn("m225kwaited");
    REQUIRE(kernel != NULL);
    REQUIRE(program != NULL);
    REQUIRE(other != NULL);
    REQUIRE(kthread != NULL);

    CHECK(!scheduler_may_wait_for(program, kernel));
    CHECK(!scheduler_may_wait_for(program, kthread));
    CHECK(!scheduler_may_wait_for(program, program));
    CHECK(!scheduler_may_wait_for(program, (task_t *)0));
    CHECK(scheduler_may_wait_for(program, other));
    CHECK(scheduler_may_wait_for(kernel, kthread));
    CHECK(scheduler_may_wait_for(kernel, program));

    q13_kill(kthread);
    q13_kill(other);
    q13_kill(program);
}

/* setpgid(2) cannot move a kernel thread into a program's group, nor a
   program into the kernel's. wifi-dhcp is started inside a system call a
   program made, so that program is its parent - which is setpgid's whole
   permission check. */
TEST(scheduler, setpgid_never_mixes_a_program_and_the_kernel) {
    q13_boot();
    fake_arch_set_cpu(0);
    task_t *program = q13_spawn_program("m225setter");
    task_t *child = q13_spawn_program("m225settee");
    task_t *helper = q13_spawn("m225helper");
    REQUIRE(program != NULL);
    REQUIRE(child != NULL);
    REQUIRE(helper != NULL);
    child->parent_id = program->tgid;
    child->pgid = program->pgid;
    child->sid = program->sid;
    helper->parent_id = program->tgid;

    CHECK_EQ(scheduler_set_process_group(program, helper->id, program->pgid), -1);
    CHECK_EQ(helper->pgid, 0);
    CHECK_EQ(scheduler_set_process_group(program, 0, helper->id), -1);
    CHECK_EQ(program->pgid, program->id);

    CHECK_EQ(scheduler_set_process_group(program, child->id, 0), 0);
    CHECK_EQ(child->pgid, child->id);
    CHECK_EQ(scheduler_set_process_group(program, child->id, program->id), 0);
    CHECK_EQ(child->pgid, program->id);
    CHECK_EQ(scheduler_set_process_group(program, child->id, -5), -1);

    q13_kill(helper);
    q13_kill(child);
    q13_kill(program);
}

/* M225: [m79] asks how many of threadtest's tasks were alive at once. It
   sampled that every 25 ms and, on a fast machine, missed all three and then
   counted the kernel's own threads instead. The scheduler counts it now, at
   the moment a thread is made, and the count only grows. */
TEST(scheduler, a_process_remembers_how_many_of_its_tasks_ran_at_once) {
    q13_boot();
    fake_arch_set_cpu(0);
    task_t *process = q13_spawn_program("m225peak");
    REQUIRE(process != NULL);
    const int pid = process->id;
    CHECK_EQ(scheduler_thread_group_peak(pid), 1);

    task_t *a = q13_thread_of("m225pa", process);
    task_t *b = q13_thread_of("m225pb", process);
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    CHECK_EQ(scheduler_thread_group_peak(pid), 3);
    CHECK_EQ(scheduler_thread_group_peak(a->id), -1);

    q13_kill(a);
    q13_kill(b);
    task_t *c = q13_thread_of("m225pc", process);
    REQUIRE(c != NULL);
    CHECK_EQ(scheduler_thread_group_peak(pid), 3);

    /* Another process's threads are not this one's. */
    task_t *other = q13_spawn_program("m225peak2");
    REQUIRE(other != NULL);
    task_t *d = q13_thread_of("m225pd", other);
    REQUIRE(d != NULL);
    CHECK_EQ(scheduler_thread_group_peak(other->id), 2);
    CHECK_EQ(scheduler_thread_group_peak(pid), 3);

    q13_kill(d);
    q13_kill(other);
    q13_kill(c);
    q13_kill(process);
    CHECK_EQ(scheduler_thread_group_peak(pid), -1);
}

/* getpgid(0) and getsid(0) are the caller's. getpgid looked up pid 0, which
   is the kernel task, so getpgrp() said 0 to every program on the machine. */
TEST(scheduler, pid_zero_as_an_argument_is_the_caller) {
    q13_boot();
    task_t *asker = q13_spawn("m225asker");
    REQUIRE(asker != NULL);
    int placed = 0;
    for (int i = 0; i < 400 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        placed = (scheduler_current() == asker);
    }
    REQUIRE(placed);
    CHECK(scheduler_task_for_pid_argument(0) == asker);
    CHECK(scheduler_task_for_pid_argument(0) != scheduler_task_by_id(0));
    CHECK(scheduler_task_for_pid_argument(asker->id) == asker);
    q13_kill(asker);
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

/* M196: bring cpu 5 up as an AP sitting on its own idle identity, with every
   other task out of the way, so what a wake or a tick does to it is the only
   thing that can move it. */
static task_t *m196_idle_cpu5(void) {
    static task_t *identity;
    if (!identity) {
        fake_arch_set_cpu(5);
        scheduler_init_ap(5);
        identity = scheduler_current();
    }
    return identity;
}

static void m196_block_everything_but(task_t *keep, task_t *also) {
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (!t || t == keep || t == also || t->is_idle) {
            continue;
        }
        if (t->state == TASK_RUNNING || t->state == TASK_READY) {
            t->state = TASK_BLOCKED;
        }
    }
}

static void m196_unblock_everything(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (t && t->state == TASK_BLOCKED) {
            t->state = TASK_READY;
        }
    }
}

TEST(scheduler, a_wake_asks_a_sleeping_cpu_to_look_now_and_asks_it_once) {
    q13_boot();
    task_t *cpu5_idle = m196_idle_cpu5();
    REQUIRE(cpu5_idle != NULL);
    fake_arch_set_cpu(5);
    REQUIRE(scheduler_current() == cpu5_idle);

    int saved_count = smp_cpu_count;
    smp_cpu_count = 6;
    fake_arch_set_smp_initialized(1);
    fake_arch_reset_reschedules();

    task_t *sleeper = q13_spawn("m196wake");
    task_t *second = q13_spawn("m196two");
    REQUIRE(sleeper != NULL);
    REQUIRE(second != NULL);
    sleeper->state = TASK_BLOCKED;
    second->state = TASK_BLOCKED;

    fake_arch_set_cpu(0);
    scheduler_wake_task(sleeper);
    int asked = 0;
    for (int c = 0; c < 6; c++) {
        asked += fake_arch_reschedules_sent(c);
    }
    CHECK_EQ(asked, 1);
    CHECK_EQ(fake_arch_reschedules_sent(0), 0);

    scheduler_wake_task(second);
    for (int c = 0; c < 6; c++) {
        CHECK(fake_arch_reschedules_sent(c) <= 1);
    }

    fake_arch_reset_reschedules();
    scheduler_wake_task(sleeper);
    for (int c = 0; c < 6; c++) {
        CHECK_EQ(fake_arch_reschedules_sent(c), 0);
    }

    smp_cpu_count = saved_count;
    fake_arch_set_smp_initialized(0);
    q13_kill(sleeper);
    q13_kill(second);
}

TEST(scheduler, an_idle_cpu_takes_ready_work_at_its_next_tick_not_its_quantums_end) {
    q13_boot();
    task_t *cpu5_idle = m196_idle_cpu5();
    task_t *worker = q13_spawn("m196work");
    REQUIRE(worker != NULL);

    m196_block_everything_but((task_t *)0, (task_t *)0);
    fake_arch_set_cpu(5);
    task_t *was = scheduler_current();
    if (was != cpu5_idle && !was->is_idle) {
        was->state = TASK_BLOCKED;
    }
    for (int i = 0; i < 40 * Q13_QUANTUM; i++) {
        fake_arch_set_cpu(5);
        if (scheduler_current() == cpu5_idle) {
            break;
        }
        q13_tick(5);
    }
    fake_arch_set_cpu(5);
    REQUIRE(scheduler_current() == cpu5_idle);

    worker->state = TASK_READY;
    q13_tick(5);
    fake_arch_set_cpu(5);
    CHECK(scheduler_current() == worker);

    worker->state = TASK_BLOCKED;
    for (int i = 0; i < 4 * Q13_QUANTUM; i++) {
        fake_arch_set_cpu(5);
        if (scheduler_current() == cpu5_idle) {
            break;
        }
        q13_tick(5);
    }
    m196_unblock_everything();
    q13_kill(worker);
}

TEST(scheduler, a_futex_wake_stays_in_the_address_space_it_was_made_in) {
    q13_boot();
    static uint32_t word;
    task_t *here = q13_spawn("futex-here");
    task_t *there = q13_spawn("futex-there");
    REQUIRE(here != 0);
    REQUIRE(there != 0);
    here->state = TASK_BLOCKED;
    here->wait_chan = &word;
    here->wait_space = 0x1000;
    there->state = TASK_BLOCKED;
    there->wait_chan = &word;
    there->wait_space = 0x2000;

    CHECK_EQ(scheduler_wake_n(&word, 0x2000, 1), 1);
    CHECK_EQ(there->state, TASK_READY);
    CHECK_EQ(here->state, TASK_BLOCKED);

    CHECK_EQ(scheduler_wake_n(&word, 0x3000, 1), 0);
    CHECK_EQ(here->state, TASK_BLOCKED);

    CHECK_EQ(scheduler_wake_n(&word, 0x1000, 1), 1);
    CHECK_EQ(here->state, TASK_READY);

    q13_kill(here);
    q13_kill(there);
}

static void watch_on(task_t *t, const void *object) {
    t->watching = 1;
    t->watch_everything = 0;
    t->watch_fired = 0;
    t->watch_objects[0] = object;
    t->watch_count = 1;
    t->state = TASK_BLOCKED;
    t->wait_chan = SCHEDULER_POLL_CHAN;
}

TEST(scheduler, a_change_to_one_object_wakes_only_the_pollers_watching_it) {
    q13_boot();
    static int pipe_a, pipe_b;
    task_t *reader_a = q13_spawn("watch-a");
    task_t *reader_b = q13_spawn("watch-b");
    REQUIRE(reader_a != 0);
    REQUIRE(reader_b != 0);
    watch_on(reader_a, &pipe_a);
    watch_on(reader_b, &pipe_b);

    scheduler_wake_object(&pipe_a);
    CHECK_EQ(reader_a->state, TASK_READY);
    CHECK_EQ(reader_b->state, TASK_BLOCKED);

    scheduler_wake_objects(&pipe_a, &pipe_b);
    CHECK_EQ(reader_b->state, TASK_READY);

    reader_a->watching = 0;
    reader_b->watching = 0;
    q13_kill(reader_a);
    q13_kill(reader_b);
}

TEST(scheduler, a_broadcast_and_an_unconverted_sleeper_still_behave_as_before) {
    q13_boot();
    static int pipe_a, pipe_b;
    task_t *watcher = q13_spawn("watch-a");
    task_t *legacy = q13_spawn("legacy");
    REQUIRE(watcher != 0);
    REQUIRE(legacy != 0);
    watch_on(watcher, &pipe_a);
    legacy->watching = 0;
    legacy->state = TASK_BLOCKED;
    legacy->wait_chan = SCHEDULER_POLL_CHAN;

    scheduler_wake_object(&pipe_b);
    CHECK_EQ(watcher->state, TASK_BLOCKED);
    CHECK_EQ(legacy->state, TASK_READY);

    scheduler_wake_all(SCHEDULER_POLL_CHAN);
    CHECK_EQ(watcher->state, TASK_READY);

    watcher->watching = 0;
    q13_kill(watcher);
    q13_kill(legacy);
}

TEST(scheduler, a_watcher_woken_before_it_sleeps_does_not_sleep) {
    q13_boot();
    fake_arch_set_cpu(0);
    static int pipe_a;
    task_t *self = scheduler_current();
    REQUIRE(self != 0);
    scheduler_watch_begin();
    scheduler_watch_add(&pipe_a);
    scheduler_wake_object(&pipe_a);
    CHECK_EQ(self->watch_fired, 1);
    scheduler_watch_block(0);
    CHECK(self->state != TASK_BLOCKED);
    CHECK_EQ(self->watching, 0);
}

TEST(scheduler, a_watch_set_that_overflows_watches_everything) {
    q13_boot();
    fake_arch_set_cpu(0);
    static int objects[SCHEDULER_WATCH_MAX + 1];
    task_t *self = scheduler_current();
    scheduler_watch_begin();
    for (int i = 0; i <= SCHEDULER_WATCH_MAX; i++) {
        scheduler_watch_add(&objects[i]);
    }
    CHECK_EQ(self->watch_everything, 1);
    static int unrelated;
    scheduler_wake_object(&unrelated);
    CHECK_EQ(self->watch_fired, 1);
    scheduler_watch_end();
    CHECK_EQ(self->watching, 0);
}

TEST(scheduler, a_sleep_lock_is_counted_against_the_task_holding_it) {
    q13_boot();
    fake_arch_set_cpu(0);
    static sleep_lock_t lock;
    task_t *self = scheduler_current();
    uint32_t before = self->sleep_locks_held;
    sleep_lock_acquire(&lock);
    CHECK_EQ(self->sleep_locks_held, before + 1);
    CHECK_EQ(lock.held, 1);
    CHECK_EQ(lock.holder, self->id);
    sleep_lock_release(&lock);
    CHECK_EQ(self->sleep_locks_held, before);
    CHECK_EQ(lock.held, 0);
}

TEST(scheduler, a_fatal_signal_waits_while_its_task_holds_a_sleep_lock) {
    q13_boot();
    task_t *holder = q13_spawn("holder");
    REQUIRE(holder != 0);
    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        placed = (scheduler_current() == holder);
    }
    REQUIRE(placed);

    holder->sleep_locks_held = 1;
    holder->pending_signal = SIGKILL;
    q13_tick(0);
    CHECK(holder->state != TASK_TERMINATED);
    CHECK_EQ(holder->pending_signal, SIGKILL);

    holder->pending_signal = 0;
    holder->sleep_locks_held = 0;
    q13_kill(holder);
}

TEST(scheduler, the_timer_does_not_take_the_processor_from_a_sleep_lock_holder) {
    q13_boot();
    task_t *holder = q13_spawn("holder");
    task_t *other = q13_spawn("other");
    REQUIRE(holder != 0);
    REQUIRE(other != 0);
    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        placed = (scheduler_current() == holder);
    }
    REQUIRE(placed);

    holder->sleep_locks_held = 1;
    for (int i = 0; i < 20 * Q13_QUANTUM; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        CHECK(scheduler_current() == holder);
    }

    holder->sleep_locks_held = 0;
    int moved = 0;
    for (int i = 0; i < 4 * Q13_QUANTUM && !moved; i++) {
        q13_tick(0);
        fake_arch_set_cpu(0);
        moved = (scheduler_current() != holder);
    }
    CHECK(moved);
    q13_kill(holder);
    q13_kill(other);
}

/* M199. A deadline is armed on the one-shot timer of the core the sleeper
   blocked on, the earliest one wins, and a later one does not push it back
   - otherwise a sixty hertz frame waits for a sleeper due a second later. */
TEST(scheduler, a_deadline_arms_the_cores_one_shot_timer_and_the_earliest_wins) {
    q13_boot();
    fake_lapic_timer_reset(1);
    fake_arch_set_cpu(0);
    task_t *now = scheduler_current();
    fake_arch_stand_on(now->kernel_stack_top ? now->kernel_stack_top - 64 : 0);
    static spinlock_t held;

    uint64_t start = clock_monotonic_ms();
    uint64_t flags = spin_lock_irqsave(&held);
    scheduler_block_on(&held, start + 500, &held, &flags);
    spin_unlock_irqrestore(&held, flags);
    CHECK_EQ(fake_lapic_timer_arms(), 1);
    CHECK(fake_lapic_timer_last_delay_ns() > 0);
    CHECK(fake_lapic_timer_last_delay_ns() <= 500ULL * 1000000ULL);

    fake_arch_set_cpu(0);
    now = scheduler_current();
    fake_arch_stand_on(now->kernel_stack_top ? now->kernel_stack_top - 64 : 0);
    flags = spin_lock_irqsave(&held);
    scheduler_block_on(&held, start + 5000, &held, &flags);
    spin_unlock_irqrestore(&held, flags);
    CHECK_EQ(fake_lapic_timer_arms(), 1);

    fake_arch_set_cpu(0);
    now = scheduler_current();
    fake_arch_stand_on(now->kernel_stack_top ? now->kernel_stack_top - 64 : 0);
    flags = spin_lock_irqsave(&held);
    scheduler_block_on(&held, start + 200, &held, &flags);
    spin_unlock_irqrestore(&held, flags);
    CHECK_EQ(fake_lapic_timer_arms(), 2);
    CHECK(fake_lapic_timer_last_delay_ns() <= 200ULL * 1000000ULL);

    fake_arch_set_cpu(0);
    scheduler_deadline_timer_fired();
    fake_lapic_timer_reset(0);
    fake_arch_set_cpu(0);
    now = scheduler_current();
    fake_arch_stand_on(now->kernel_stack_top ? now->kernel_stack_top - 64 : 0);
    flags = spin_lock_irqsave(&held);
    scheduler_block_on(&held, start + 100, &held, &flags);
    spin_unlock_irqrestore(&held, flags);
    CHECK_EQ(fake_lapic_timer_arms(), 0);
}

static task_t *m199_sleeper;
static int m199_state_before_fire;
static int m199_state_after_fire;
static uint64_t m199_fired_before;

static void m199_fire_while_away(void) {
    m199_state_before_fire = m199_sleeper->state;
    fake_pit_advance(1000);
    fake_arch_set_cpu(0);
    m199_fired_before = scheduler_deadline_timer_interrupts();
    scheduler_deadline_timer_fired();
    m199_state_after_fire = m199_sleeper->state;
}

TEST(scheduler, the_one_shot_timer_wakes_a_sleeper_whose_deadline_passed) {
    q13_boot();
    fake_lapic_timer_reset(1);
    task_t *sleeper = q13_spawn("m199-sleeper");
    task_t *other = q13_spawn("m199-other");
    REQUIRE(sleeper != NULL);
    REQUIRE(other != NULL);
    m196_block_everything_but(sleeper, other);
    int placed = 0;
    for (int i = 0; i < 200 * Q13_QUANTUM && !placed; i++) {
        q13_tick(0);
        placed = (scheduler_current() == sleeper);
    }
    REQUIRE(placed);

    fake_arch_set_cpu(0);
    fake_arch_stand_on(sleeper->kernel_stack_top - 64);
    m199_sleeper = sleeper;
    m199_state_before_fire = -1;
    m199_state_after_fire = -1;
    fake_arch_raise_interrupt(m199_fire_while_away);
    static spinlock_t held;
    uint64_t flags = spin_lock_irqsave(&held);
    scheduler_block_on(&held, clock_monotonic_ms() + 30, &held, &flags);
    spin_unlock_irqrestore(&held, flags);
    fake_arch_raise_interrupt(0);

    CHECK_EQ(m199_state_before_fire, TASK_BLOCKED);
    CHECK_EQ(m199_state_after_fire, TASK_READY);
    CHECK_EQ(scheduler_deadline_timer_interrupts(), m199_fired_before + 1);

    fake_lapic_timer_reset(0);
    m196_unblock_everything();
    q13_kill(sleeper);
    q13_kill(other);
}

/* ---- M225: what a review of this milestone's first scheduler repair found */

/* Idle tasks once for all of these tests: each scheduler_spawn_idle_tasks
   makes a fresh set of sixteen, kernel stacks and all, and the host suite
   as a whole runs close to the fake allocator's ceiling (the unix socket
   tests take half of it at once). */
static void m225_boot(void) {
    static int idle_spawned;
    q13_boot();
    if (!idle_spawned) {
        scheduler_spawn_idle_tasks();
        idle_spawned = 1;
    }
}

/* Put `t` on `cpu` the way the machine would: tick that processor until it
   is the one running. A switch resets the slice, so the next tick on it
   does not preempt. */
static int m225_place(task_t *t, int cpu) {
    for (int i = 0; i < 400 * Q13_QUANTUM; i++) {
        q13_tick(cpu);
        fake_arch_set_cpu(cpu);
        if (scheduler_current() == t) {
            fake_arch_stand_on(t->kernel_stack_top - 64);
            return 1;
        }
    }
    return 0;
}

/* Spawn programs (or kernel threads) until one lands in `slot`'s slot - the
   next spawn takes the lowest free slot, which may be below it. The others
   are kept in `spare` for the caller to end. */
static task_t *m225_refill_slot(int slot, int program, task_t **spare, int *spares) {
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = program ? q13_spawn_program("m225refill") : q13_spawn("m225refill");
        if (!t) {
            return (task_t *)0;
        }
        if (PID_SLOT(t->id) == slot) {
            return t;
        }
        spare[(*spares)++] = t;
    }
    return (task_t *)0;
}

static task_t *m225_held_spare[MAX_TASKS];
static int m225_held_spares;

static void m225_end_spares(void) {
    for (int i = 0; i < m225_held_spares; i++) {
        q13_kill(m225_held_spare[i]);
    }
    m225_held_spares = 0;
}

/* A test that ends a task from inside a hook that is itself inside another
   task's exit needs the outer CHECK_PANIC's landing place back afterwards. */
static void m225_exit_inside(task_t *t, int cpu, int code) {
    jmp_buf outer;
    memcpy(outer, test_panic_jmp, sizeof(jmp_buf));
    int outer_armed = test_panic_armed;
    int was_cpu = smp_current_cpu();
    task_t *was = scheduler_current();
    fake_arch_set_cpu(cpu);
    fake_arch_stand_on(t->kernel_stack_top - 64);
    CHECK_PANIC(task_exit_with_code(code), "terminated task resumed");
    memcpy(test_panic_jmp, outer, sizeof(jmp_buf));
    test_panic_armed = outer_armed;
    fake_arch_set_cpu(was_cpu);
    fake_arch_stand_on(was && was->kernel_stack_top ? was->kernel_stack_top - 64 : 0);
}

/* exit() and every fatal signal of a process with threads SIGKILL the rest
   of the group. M225 collected the victims under the lock and raised after
   dropping it, through the door that checks no id: a detached thread that
   finished in between was swept by another processor's spawn, its slot was
   refilled, and the SIGKILL went to the newcomer. Here the other processor
   acts the moment the lock is let go of. */
static task_t *m225_group_leader;
static task_t *m225_detached;
static task_t *m225_newcomer;

static void m225_detached_thread_finishes_and_its_slot_is_refilled(void) {
    fake_spinlock_on_release(0);
    int slot = PID_SLOT(m225_detached->id);
    q13_kill(m225_detached);
    fake_arch_set_cpu(1);
    m225_newcomer = m225_refill_slot(slot, 1, m225_held_spare, &m225_held_spares);
    fake_arch_set_cpu(0);
}

TEST(scheduler, a_group_kill_reaches_nobody_who_took_a_finished_threads_slot) {
    q13_boot();
    fake_arch_set_cpu(0);
    m225_group_leader = q13_spawn_program("m225leader");
    REQUIRE(m225_group_leader != NULL);
    task_t *sibling = q13_thread_of("m225sibling", m225_group_leader);
    m225_detached = q13_thread_of("m225detached", m225_group_leader);
    REQUIRE(sibling != NULL);
    REQUIRE(m225_detached != NULL);
    m225_detached->detached = 1;
    m225_newcomer = (task_t *)0;

    fake_spinlock_on_release(m225_detached_thread_finishes_and_its_slot_is_refilled);
    scheduler_kill_thread_group(m225_group_leader);
    fake_spinlock_on_release(0);

    REQUIRE(m225_newcomer != NULL);
    CHECK(m225_untouched(m225_newcomer));
    CHECK_EQ(sibling->pending_signal, SIGKILL);
    CHECK(m225_untouched(m225_group_leader));

    m225_quiet(sibling);
    q13_kill(m225_newcomer);
    m225_end_spares();
    q13_kill(sibling);
    q13_kill(m225_group_leader);
}

/* The first version's own test of a kill whose target changes hands refilled
   the slot with a kernel thread, which the rule about the kernel refuses
   anyway - so the id check under it went untested (a mutation that dropped
   it passed every test). Here the slot goes to an ordinary program the
   sender may signal, and only the id stands between it and the signal. */
TEST(scheduler, a_kill_whose_target_is_replaced_by_a_program_reaches_nobody) {
    q13_boot();
    fake_arch_set_cpu(0);
    task_t *sender = q13_spawn_program("m225sender");
    REQUIRE(sender != NULL);
    m225_refill_with_program = 1;
    const long how[] = {1, -2};
    for (int k = 0; k < 2; k++) {
        m225_victim = q13_spawn_program("m225victim");
        REQUIRE(m225_victim != NULL);
        long pid = how[k] == 1 ? (long)m225_victim->id : -(long)m225_victim->pgid;
        long rc = scheduler_kill(sender, pid, SIGKILL, m225_slot_changes_hands);
        REQUIRE(m225_refill != NULL);
        CHECK(!scheduler_task_is_kernel(m225_refill));
        CHECK(m225_untouched(m225_refill));
        CHECK_EQ(rc, -OS_ERROR_SEARCH);
        m225_slot_test_reset();
    }
    m225_refill_with_program = 0;
    q13_kill(sender);
}

/* A pid names a process, and a process whose main thread left through
   pthread_exit is still running in its other threads - wait() has said so
   since M225. kill(pid) said ESRCH all the same: the parent had a child it
   could neither signal nor stop waiting for. Linux delivers to the group. */
TEST(scheduler, a_kill_reaches_a_process_whose_main_thread_left_first) {
    m225_boot();
    fake_arch_set_cpu(0);
    task_t *parent = q13_spawn_program("m225parent");
    task_t *leader = q13_spawn_program("m225main");
    REQUIRE(parent != NULL);
    REQUIRE(leader != NULL);
    task_t *blocking = q13_thread_of("m225blocking", leader);
    task_t *taking = q13_thread_of("m225taking", leader);
    REQUIRE(blocking != NULL);
    REQUIRE(taking != NULL);
    leader->parent_id = parent->id;
    leader->lineage_id = parent->id;
    const int leader_id = leader->id;

    m225_exit_on_cpu0(leader, 0, 0);
    REQUIRE(!scheduler_process_has_ended(leader));

    /* Signal 0 asks whether the process is there, and it is. */
    CHECK_EQ(scheduler_kill(parent, leader_id, 0, m225_anyone), 0);

    /* A caught signal goes to a thread that does not block it. */
    blocking->sig_handler[SIGUSR1] = 0x1000;
    taking->sig_handler[SIGUSR1] = 0x1000;
    blocking->sig_blocked = 1u << SIGUSR1;
    CHECK_EQ(scheduler_kill(parent, leader_id, SIGUSR1, m225_anyone), 0);
    CHECK(taking->sig_pending & (1u << SIGUSR1));
    CHECK_EQ(blocking->sig_pending & (1u << SIGUSR1), 0u);
    m225_quiet(taking);
    blocking->sig_blocked = 0;

    /* SIGKILL reaches it - the first of them dies, and takes the rest. */
    CHECK_EQ(scheduler_kill(parent, leader_id, SIGKILL, m225_anyone), 0);
    CHECK(blocking->pending_signal == SIGKILL || taking->pending_signal == SIGKILL);
    m225_quiet(blocking);
    m225_quiet(taking);

    /* Once the last of it has gone, the process is gone. */
    m225_exit_on_cpu0(blocking, 0, 0);
    m225_exit_on_cpu0(taking, 0, 0);
    CHECK(scheduler_process_has_ended(leader));
    CHECK_EQ(scheduler_kill(parent, leader_id, 0, m225_anyone), -OS_ERROR_SEARCH);
    CHECK_EQ(scheduler_kill(parent, taking->id, 0, m225_anyone), -OS_ERROR_SEARCH);

    scheduler_reap_slot(blocking);
    scheduler_reap_slot(taking);
    scheduler_reap_slot(leader);
    m225_quiet(parent);
    q13_kill(parent);
}

/* SIGCHLD and its siginfo are one thing now, written to the task the signal
   lands on. They were written to the parent's slot before the raise looked
   at it - into a parent whose main thread had gone (and which the raise then
   refused), and without the lock, so two children ending at once could
   leave one's pid beside the other's status. */
TEST(scheduler, a_sigchld_and_its_siginfo_reach_the_parent_process_together) {
    m225_boot();
    fake_arch_set_cpu(0);
    task_t *parent = q13_spawn_program("m225pparent");
    REQUIRE(parent != NULL);
    task_t *survivor = q13_thread_of("m225psurvivor", parent);
    task_t *child = q13_spawn_program("m225pchild");
    REQUIRE(survivor != NULL);
    REQUIRE(child != NULL);
    child->parent_id = parent->id;
    survivor->sig_handler[SIGCHLD] = 0x1000;
    survivor->si_pid = 0;
    survivor->si_status = 0;
    const int child_id = child->id;

    /* The parent's main thread leaves; its other thread carries on. */
    parent->parent_id = 0;
    m225_exit_on_cpu0(parent, 0, 0);
    parent->si_pid = 0;
    parent->si_status = 0;

    m225_exit_on_cpu0(child, 5, 0);
    CHECK(survivor->sig_pending & (1u << SIGCHLD));
    CHECK_EQ(survivor->si_pid, child_id);
    CHECK_EQ(survivor->si_status, 5);
    CHECK_EQ(parent->si_pid, 0);

    m225_quiet(survivor);
    scheduler_reap_slot(child);
    m225_exit_on_cpu0(survivor, 0, 0);
    scheduler_reap_slot(survivor);
    scheduler_reap_slot(parent);
}

/* Two threads of one process exiting at once: the first marks itself and
   sees the second still live, so it is not last and does not leave the
   address space; the second skips the first (marked) and destroys it. The
   first was still running its exit on that page table. Here the second
   exits on CPU 1 the moment the first lets go of the lock it marked itself
   under. */
static task_t *m225_first;
static task_t *m225_second;
static uint64_t m225_space;
static int m225_second_done;
static int m225_destroyed_by_second;
static uint32_t m225_holders_when_destroyed;

static void m225_second_thread_exits_meanwhile(void) {
    if (!m225_first->exiting || m225_second_done) {
        return;
    }
    fake_spinlock_on_release(0);
    m225_second_done = 1;
    int before = fake_objects_address_spaces_destroyed();
    m225_exit_inside(m225_second, 1, 0);
    m225_destroyed_by_second = fake_objects_address_spaces_destroyed() - before;
    m225_holders_when_destroyed = scheduler_cpus_holding_address_space(m225_space);
}

TEST(scheduler, an_address_space_is_never_destroyed_under_a_thread_still_on_it) {
    m225_boot();
    fake_arch_set_cpu(0);
    m225_first = q13_spawn_program("m225first");
    REQUIRE(m225_first != NULL);
    m225_second = q13_thread_of("m225second", m225_first);
    REQUIRE(m225_second != NULL);
    m225_first->parent_id = 0;
    m225_space = m225_first->pml4_phys;
    m225_second_done = 0;
    m225_destroyed_by_second = 0;

    REQUIRE(m225_place(m225_first, 0));
    REQUIRE(m225_place(m225_second, 1));
    fake_arch_set_cpu(0);
    REQUIRE(scheduler_current() == m225_first);
    CHECK_EQ(scheduler_cpus_holding_address_space(m225_space), 0x3u);

    fake_spinlock_on_release(m225_second_thread_exits_meanwhile);
    fake_arch_stand_on(m225_first->kernel_stack_top - 64);
    CHECK_PANIC(task_exit_with_code(0), "terminated task resumed");
    fake_spinlock_on_release(0);
    fake_spinlock_release_all();

    CHECK(m225_second_done);
    CHECK_EQ(m225_destroyed_by_second, 1);
    CHECK_EQ(m225_holders_when_destroyed, 0u);
    scheduler_reap_slot(m225_second);
    scheduler_reap_slot(m225_first);
}

/* And a task on its way out that is switched back in - it can be preempted
   during its exit - is put on the kernel's page table, not the one it still
   names, which the last thread out may have freed by then. */
TEST(scheduler, a_task_on_its_way_out_runs_on_the_kernels_page_table) {
    m225_boot();
    task_t *leaving = q13_spawn_program("m225leaving");
    REQUIRE(leaving != NULL);
    leaving->exiting = 1;
    REQUIRE(m225_place(leaving, 1));
    CHECK_EQ(scheduler_cpus_holding_address_space(leaving->pml4_phys) & (1u << 1), 0u);
    leaving->exiting = 0;
    /* Off CPU 1 the way an ending task leaves (q13_kill does this on 0). */
    leaving->state = TASK_TERMINATED;
    fake_arch_set_cpu(1);
    fake_arch_stand_on(leaving->kernel_stack_top - 64);
    schedule();
    fake_arch_set_cpu(0);
    scheduler_reap_slot(leaving);
}

/* fork() returned child->id, read after task_fork had made the child
   runnable - which on another processor could already have exec'd, failed,
   _exit'd, been reaped by a sibling's waitpid(-1) and had its slot refilled -
   and wrote the child's environment record after that too. task_fork now
   gives the child its records before it is runnable and writes down what it
   made on the forking thread, under the lock. */
TEST(scheduler, a_fork_is_complete_before_the_child_can_run_and_its_id_outlives_it) {
    m225_boot();
    task_t *parent = q13_spawn("m225forker");
    REQUIRE(parent != NULL);
    static const char env[] = "A=1\0B=22";
    REQUIRE(scheduler_set_env(parent, env, sizeof(env), 2) == 0);
    REQUIRE(m225_place(parent, 0));
    isr_regs_t regs;
    memset(&regs, 0, sizeof(regs));

    task_t *child = task_fork(parent->pml4_phys, &regs);
    REQUIRE(child != NULL);
    const int child_id = child->id;
    fake_arch_set_cpu(0);
    CHECK_EQ(scheduler_last_spawn((uint32_t *)0), child_id);
    REQUIRE(child->env_block != NULL);
    CHECK_EQ(child->env_length, (uint32_t)sizeof(env));
    CHECK_EQ(child->env_count, 2u);
    CHECK(memcmp(child->env_block, env, sizeof(env)) == 0);
    CHECK(child->env_block != parent->env_block);

    /* It ends and its slot is another processor's spawn's. */
    int slot = PID_SLOT(child_id);
    scheduler_release_env(child);
    scheduler_release_cmdline(child);
    q13_kill(child);
    fake_arch_set_cpu(1);
    task_t *stranger = m225_refill_slot(slot, 0, m225_held_spare, &m225_held_spares);
    fake_arch_set_cpu(0);
    REQUIRE(stranger != NULL);
    CHECK_EQ(scheduler_last_spawn((uint32_t *)0), child_id);

    q13_kill(stranger);
    m225_end_spares();
    scheduler_release_env(parent);
    q13_kill(parent);
}

/* A new process's capabilities, command line and environment are its own
   the moment anything else can see it. M225 wrote them after the slot was
   runnable and held the child at the door meanwhile; the door did not stop
   the child being ENDED (and its slot refilled before the writes), and a
   spawner killed in between left it at the door for good. Here the other
   processor looks at every release of a lock during the spawn. */
static int m225_born_seen;
static uint32_t m225_born_caps;
static int m225_born_had_cmdline;
static int m225_born_had_env;

static void m225_look_for_the_newborn(void) {
    for (int i = 0; i < scheduler_task_count(); i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (!t || t->state != TASK_READY || strcmp(t->name, "m225born") != 0) {
            continue;
        }
        fake_spinlock_on_release(0);
        m225_born_seen = 1;
        m225_born_caps = t->caps;
        m225_born_had_cmdline = t->cmdline_block != NULL;
        m225_born_had_env = t->env_block != NULL;
        return;
    }
}

TEST(scheduler, a_new_process_has_everything_it_is_given_before_it_can_run) {
    q13_boot();
    fake_arch_set_cpu(0);
    task_t *spawner = scheduler_current();
    REQUIRE(spawner != NULL);
    uint32_t spawner_caps = spawner->caps;
    spawner->caps = 0xFFu;

    task_spawn_setup_t setup = {0};
    setup.caps = 0x05u;
    static const char *const argv[] = {"m225born", "--flag", 0};
    setup.cmdline = scheduler_pack_cmdline(argv, &setup.cmdline_length);
    setup.env = (char *)kmalloc(4);
    REQUIRE(setup.cmdline != NULL);
    REQUIRE(setup.env != NULL);
    memcpy(setup.env, "X=1", 4);
    setup.env_length = 4;
    setup.env_count = 1;

    m225_born_seen = 0;
    fake_spinlock_on_release(m225_look_for_the_newborn);
    task_t *born = task_spawn_program("m225born", 0x7800000, q13_body, (void *)0, 0, 0, &setup);
    fake_spinlock_on_release(0);
    REQUIRE(born != NULL);

    CHECK(m225_born_seen);
    CHECK_EQ(m225_born_caps, 0x05u);
    CHECK(m225_born_had_cmdline);
    CHECK(m225_born_had_env);
    uint32_t caps = 0;
    CHECK_EQ(scheduler_last_spawn(&caps), born->id);
    CHECK_EQ(caps, 0x05u);
    char cmdline[64];
    int length = scheduler_copy_cmdline(born->id, cmdline, sizeof(cmdline));
    CHECK_EQ(length, (int)sizeof("m225born\0--flag"));
    CHECK(length > 0 && memcmp(cmdline, "m225born\0--flag", (size_t)length) == 0);

    spawner->caps = spawner_caps;
    scheduler_release_cmdline(born);
    scheduler_release_env(born);
    q13_kill(born);
}

/* The orderly stop at shutdown wrote SIGTERM straight into the fatal path,
   so a program's SIGTERM handler never ran and one that ignored SIGTERM died
   of it anyway; and a task asleep in a system call was never woken for
   either signal. It goes through the same door as kill(2) now. */
TEST(scheduler, the_orderly_stop_sends_real_signals) {
    m225_boot();
    fake_arch_set_cpu(0);
    task_t *self = scheduler_current();
    REQUIRE(self != NULL);

    /* Everybody else in the table is put back as it was afterwards. */
    static int saved_pending[MAX_TASKS];
    static int saved_stop[MAX_TASKS];
    static uint32_t saved_sig[MAX_TASKS];
    for (int i = 0; i < scheduler_task_count(); i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (t) {
            saved_pending[i] = t->pending_signal;
            saved_stop[i] = t->pending_stop;
            saved_sig[i] = t->sig_pending;
        }
    }

    task_t *handles = q13_spawn_program("m225handles");
    task_t *ignores = q13_spawn_program("m225ignores");
    task_t *sleeps = q13_spawn_program("m225sleeps");
    task_t *leaving = q13_spawn_program("m225leaving2");
    REQUIRE(handles != NULL);
    REQUIRE(ignores != NULL);
    REQUIRE(sleeps != NULL);
    REQUIRE(leaving != NULL);
    handles->sig_handler[SIGTERM] = 0x1000;
    ignores->sig_handler[SIGTERM] = SIG_IGN_ADDR;
    static int nothing_will_wake_this;
    sleeps->state = TASK_BLOCKED;
    sleeps->wait_chan = &nothing_will_wake_this;
    leaving->ending = 1;

    CHECK(scheduler_signal_orderly_stop(self, SIGTERM) >= 3);
    CHECK(handles->sig_pending & (1u << SIGTERM));
    CHECK_EQ(handles->pending_signal, 0);
    CHECK(m225_untouched(ignores));
    CHECK_EQ(sleeps->pending_signal, SIGTERM);
    CHECK_EQ(sleeps->state, TASK_READY);
    CHECK(m225_untouched(leaving));
    CHECK(m225_untouched(self));

    CHECK(scheduler_signal_orderly_stop(self, SIGKILL) >= 4);
    CHECK_EQ(ignores->pending_signal, SIGKILL);
    CHECK_EQ(handles->pending_signal, SIGKILL);
    CHECK(m225_untouched(leaving));

    for (int i = 0; i < scheduler_task_count(); i++) {
        task_t *t = scheduler_task_by_slot(i);
        if (t && t != handles && t != ignores && t != sleeps && t != leaving) {
            t->pending_signal = saved_pending[i];
            t->pending_stop = saved_stop[i];
            t->sig_pending = saved_sig[i];
        }
    }
    m225_quiet(handles);
    m225_quiet(ignores);
    m225_quiet(sleeps);
    leaving->ending = 0;
    q13_kill(handles);
    q13_kill(ignores);
    q13_kill(sleeps);
    q13_kill(leaving);
}

/* Where scheduler_begin_exit is called, graded: the window between a fatal
   signal's group kill and task_exit_with_code is where a sibling's SIGKILL
   came back on four processors, so the flag must be up before the group kill
   starts. The first version's test hooked the inside of task_exit_with_code,
   after that function's own begin_exit, and so could not see this window -
   both calls could be deleted one at a time with every test passing. */
static task_t *m225_dying;

static void m225_sibling_sigkill_lands(void) {
    fake_spinlock_on_release(0);
    scheduler_raise_signal(m225_dying, SIGKILL);
    scheduler_tick_cpu(0);
}

TEST(scheduler, a_fatal_signal_shuts_the_door_before_it_kills_the_group) {
    m225_boot();
    m225_dying = q13_spawn("m225dying");
    REQUIRE(m225_dying != NULL);
    m225_dying->parent_id = 0;
    const int id = m225_dying->id;
    REQUIRE(m225_place(m225_dying, 0));

    fake_spinlock_on_release(m225_sibling_sigkill_lands);
    CHECK_PANIC(task_exit_with_signal(SIGTERM), "terminated task resumed");
    fake_spinlock_on_release(0);
    fake_spinlock_release_all();

    int status = -1;
    CHECK_EQ(scheduler_exit_status(id, &status), 1);
    CHECK_EQ(status, SIGTERM);
    CHECK_EQ(m225_dying->pending_signal, 0);
    scheduler_reap_slot(m225_dying);
}

/* The first version's guards that no test failed without. A kernel thread a
   PROGRAM starts (wifi-dhcp inside a program's system call) is the kernel's,
   group and session; every test started its kernel threads from the kernel. */
TEST(scheduler, a_kernel_thread_a_program_starts_is_in_the_kernels_group) {
    m225_boot();
    task_t *program = q13_spawn_program("m225starter");
    REQUIRE(program != NULL);
    REQUIRE(m225_place(program, 0));
    task_t *kthread = q13_spawn("m225kthread2");
    REQUIRE(kthread != NULL);
    CHECK(scheduler_task_is_kernel(kthread));
    CHECK_EQ(kthread->pgid, 0);
    CHECK_EQ(kthread->sid, 0);
    CHECK_EQ(kthread->parent_id, program->tgid);
    q13_kill(kthread);
    q13_kill(program);
}

/* setpgid's own refusal of a kernel target: the call the first version's
   test made was already refused by the session check. A kernel thread made
   its own group (pgid 0 means "my id") asks no leader, so only the kernel
   rule refuses. */
TEST(scheduler, setpgid_refuses_a_kernel_thread_whatever_group_is_asked_for) {
    q13_boot();
    fake_arch_set_cpu(0);
    task_t *program = q13_spawn_program("m225setter2");
    task_t *helper = q13_spawn("m225helper2");
    REQUIRE(program != NULL);
    REQUIRE(helper != NULL);
    helper->parent_id = program->tgid;
    CHECK_EQ(scheduler_set_process_group(program, helper->id, 0), -1);
    CHECK_EQ(helper->pgid, 0);
    CHECK_EQ(scheduler_set_process_group(program, helper->id, helper->id), -1);
    CHECK_EQ(helper->pgid, 0);
    q13_kill(helper);
    q13_kill(program);
}

/* A task on its way out takes no pending signal at the tick, nor where a
   blocking call takes one - even one written straight into pending_signal,
   which raise would have refused (the orderly stop did exactly that until
   M225). */
TEST(scheduler, a_task_on_its_way_out_is_not_ended_again_by_a_pending_signal) {
    m225_boot();
    task_t *t = q13_spawn("m225gate");
    REQUIRE(t != NULL);
    REQUIRE(m225_place(t, 0));
    t->ending = 1;
    t->pending_signal = SIGKILL;
    CHECK_NO_PANIC(scheduler_tick_cpu(0));
    CHECK(t->state != TASK_TERMINATED);

    if (scheduler_current() == t) {
        t->watch_fired = 1;
        CHECK_NO_PANIC(scheduler_watch_block(0));
        CHECK(t->state != TASK_TERMINATED);
        t->watch_fired = 0;
    }
    t->pending_signal = 0;
    t->ending = 0;
    fake_spinlock_release_all();
    q13_kill(t);
}

/* A one-shot hook a failed test leaves armed is disarmed by the end of the
   test - two tests, in this order. */
static int m225_stale_hook_ran;

static void m225_stale_hook(int owner) {
    (void)owner;
    m225_stale_hook_ran = 1;
}

TEST(scheduler, m225_a_test_that_leaves_a_hook_armed) {
    m225_stale_hook_ran = 0;
    fake_objects_on_shared_memory_free(m225_stale_hook);
    /* ...and ends, as a REQUIRE that failed would, without disarming it. */
}

TEST(scheduler, m225_the_next_test_does_not_meet_that_hook) {
    shared_memory_free_by_owner(0x7FFF0000);
    CHECK_EQ(m225_stale_hook_ran, 0);
}

/* A page fault maps from a snapshot of the region table that can be stale
   by the time the page goes in (M181 holds no lock across it, on purpose).
   A writer that ran in between dealt with the pages that were present, and
   this one was not yet - so it stayed: mapped from a region munmap had
   removed (for a memfd page, a frame the memfd later frees while it is
   still mapped here), or writable after mprotect(PROT_READ) returned. The
   other core acts while the fault is allocating its frame. */
static task_t *m225_faulter;
static int m225_writer_ran;

static void m225_munmap_on_another_core(void) {
    uint64_t f = scheduler_regions_begin_change(m225_faulter);
    m225_faulter->mmaps[0].pages = 0;
    scheduler_regions_end_change(m225_faulter, f);
    m225_writer_ran = 1;
}

static void m225_mprotect_on_another_core(void) {
    uint64_t f = scheduler_regions_begin_change(m225_faulter);
    m225_faulter->mmaps[0].prot = PROT_READ;
    scheduler_regions_end_change(m225_faulter, f);
    /* sys_mprotect then fixes the pages that are present - none yet. */
    virtual_memory_protect_range_in(m225_faulter->pml4_phys, m225_faulter->mmaps[0].base,
                                    m225_faulter->mmaps[0].base + 4 * 4096,
                                    VIRTUAL_MEMORY_FLAG_USER);
    m225_writer_ran = 1;
}

#define M225_REGION 0xA000200000ULL

static task_t *m225_fault_fixture(const char *name) {
    fake_virtual_memory_model_user_pages(1);
    task_t *p = q13_spawn_program(name);
    if (!p || !m225_place(p, 0) || scheduler_regions_reserve(p) != 0) {
        return (task_t *)0;
    }
    p->mmaps[0] = (mmap_region_t){.base = M225_REGION, .pages = 4,
                                  .prot = PROT_READ | PROT_WRITE, .handle = -1};
    m225_faulter = p;
    m225_writer_ran = 0;
    return p;
}

static void m225_fault_teardown(task_t *p) {
    uint64_t phys = 0;
    for (uint64_t page = M225_REGION; page < M225_REGION + 4 * 4096; page += 4096) {
        if (fake_virtual_memory_user_page(page, &phys, (uint64_t *)0)) {
            virtual_memory_unmap_page_take(p->pml4_phys, page);
            physical_memory_free_frame(phys);
        }
    }
    fake_virtual_memory_model_user_pages(0);
    scheduler_regions_release(p);
    q13_kill(p);
}

TEST(scheduler, a_page_faulted_in_from_a_region_unmapped_meanwhile_does_not_stay) {
    m225_boot();
    task_t *p = m225_fault_fixture("m225unmapped");
    REQUIRE(p != NULL);
    const uint64_t page = M225_REGION + 4096;
    uint64_t frames = fake_physical_memory_outstanding();

    fake_physical_memory_on_alloc(m225_munmap_on_another_core);
    CHECK_EQ(scheduler_fault_fill(page, 2, 0), 1);
    CHECK(m225_writer_ran);
    CHECK(!fake_virtual_memory_user_page(page, (uint64_t *)0, (uint64_t *)0));
    CHECK_EQ(fake_physical_memory_outstanding(), frames);
    /* The retry meets a snapshot that is not stale, and is refused. */
    CHECK_EQ(scheduler_fault_fill(page, 2, 0), 0);
    m225_fault_teardown(p);
}

TEST(scheduler, a_page_faulted_in_across_an_mprotect_takes_the_new_protection) {
    m225_boot();
    task_t *p = m225_fault_fixture("m225protected");
    REQUIRE(p != NULL);
    const uint64_t page = M225_REGION + 2 * 4096;

    fake_physical_memory_on_alloc(m225_mprotect_on_another_core);
    CHECK_EQ(scheduler_fault_fill(page, 0, 0), 1);
    CHECK(m225_writer_ran);
    uint64_t flags = 0;
    CHECK(fake_virtual_memory_user_page(page, (uint64_t *)0, &flags));
    CHECK_EQ(flags & VIRTUAL_MEMORY_FLAG_WRITABLE, 0u);
    m225_fault_teardown(p);
}

/* And a fault that meets no writer still takes no lock after the map, as it
   took none before it (M181's measurement): the region lock is held as if
   by somebody else, and a fault that reached for it would be a recursive
   acquire here. */
TEST(scheduler, a_fault_that_meets_no_writer_takes_no_lock_after_the_map_either) {
    m225_boot();
    task_t *p = m225_fault_fixture("m225quiet");
    REQUIRE(p != NULL);
    const uint64_t page = M225_REGION + 3 * 4096;
    p->mmap_lock.locked = 1;
    int filled = -1;
    CHECK_NO_PANIC(filled = scheduler_fault_fill(page, 2, 0));
    p->mmap_lock.locked = 0;
    CHECK_EQ(filled, 1);
    uint64_t flags = 0;
    CHECK(fake_virtual_memory_user_page(page, (uint64_t *)0, &flags));
    CHECK(flags & VIRTUAL_MEMORY_FLAG_WRITABLE);
    m225_fault_teardown(p);
}

/* An alarm is raised where the tick finds it due, under the same hold of the
   lock (M225): it was looked up by id again afterwards and raised through
   the door that checks none. What is graded here is that it still arrives,
   once, at the task that set it - and that a task that ended meanwhile is
   not alarmed. */
TEST(scheduler, an_alarm_reaches_the_task_that_set_it_once) {
    m225_boot();
    fake_arch_set_cpu(0);
    task_t *t = q13_spawn_program("m225alarm");
    task_t *gone = q13_spawn_program("m225alarmgone");
    REQUIRE(t != NULL);
    REQUIRE(gone != NULL);
    t->sig_handler[SIGALRM] = 0x1000;
    gone->sig_handler[SIGALRM] = 0x1000;
    scheduler_set_alarm(t, 1);
    scheduler_set_alarm(gone, 1);
    gone->state = TASK_TERMINATED;
    fake_pit_advance(2 * PIT_HZ);
    q13_tick(0);
    fake_arch_set_cpu(0);
    CHECK(t->sig_pending & (1u << SIGALRM));
    CHECK_EQ(t->alarm_deadline_ms, 0u);
    CHECK_EQ(gone->sig_pending & (1u << SIGALRM), 0u);
    t->sig_pending = 0;
    q13_tick(0);
    fake_arch_set_cpu(0);
    CHECK_EQ(t->sig_pending & (1u << SIGALRM), 0u);
    m225_quiet(t);
    q13_kill(gone);
    q13_kill(t);
}

/* M225 (process-lifetimes). Record locks belong to the PROCESS (POSIX
   fcntl): any thread of the holder may unlock, a second thread of the same
   process does not conflict with its own process's lock, F_GETLK names the
   process, and the locks go when the process ends - not when the thread that
   took one does. They were keyed by the task's own id. Every flock_* call
   in the kernel keys on scheduler_record_lock_owner(), which is what is
   asked here, through the table itself. */
TEST(scheduler, a_record_lock_belongs_to_the_process_not_to_the_thread_that_took_it) {
    q13_boot();
    fake_arch_set_cpu(0);
    task_t *leader = q13_spawn_program("m225locks");
    REQUIRE(leader != NULL);
    task_t *worker = q13_thread_of("m225lockworker", leader);
    task_t *other = q13_spawn_program("m225lockother");
    REQUIRE(worker != NULL);
    REQUIRE(other != NULL);
    const int process = scheduler_record_lock_owner(leader);
    const int stranger = scheduler_record_lock_owner(other);
    CHECK_EQ(process, leader->id);
    CHECK_EQ(scheduler_record_lock_owner(worker), process);
    int before = flock_count();

    /* The leader holds [0,10); its thread asking for the same range is the
       same owner asking again, not a second one. */
    CHECK_EQ(flock_set(31, process, OS_FLOCK_WR, 0, 10), 0);
    CHECK_EQ(flock_set(31, scheduler_record_lock_owner(worker), OS_FLOCK_WR, 0, 10), 0);
    os_flock_t who;
    CHECK_EQ(flock_test(31, scheduler_record_lock_owner(worker), OS_FLOCK_WR, 0, 10, &who), 0);
    /* F_GETLK from another process names the PROCESS - what its getpid()
       says - not a thread. */
    CHECK_EQ(flock_test(31, stranger, OS_FLOCK_WR, 0, 10, &who), 1);
    CHECK_EQ(who.pid, leader->id);
    CHECK_EQ(flock_set(31, stranger, OS_FLOCK_RD, 0, 1), FLOCK_CONFLICT);

    /* The thread takes [20,30) and exits. The lock is its process's, and
       the process is still running. */
    CHECK_EQ(flock_set(31, scheduler_record_lock_owner(worker), OS_FLOCK_WR, 20, 10), 0);
    scheduler_release_file_descriptors(worker);
    CHECK_EQ(flock_test(31, stranger, OS_FLOCK_RD, 20, 10, &who), 1);
    CHECK_EQ(who.pid, leader->id);

    /* Another thread of the holder - the leader - may unlock it. */
    CHECK_EQ(flock_set(31, process, OS_FLOCK_UNLCK, 20, 10), 0);
    CHECK_EQ(flock_test(31, stranger, OS_FLOCK_RD, 20, 10, &who), 0);

    /* And the process's last task out takes what is left with it. */
    scheduler_release_file_descriptors(leader);
    CHECK_EQ(flock_test(31, stranger, OS_FLOCK_WR, 0, 0, &who), 0);
    CHECK_EQ(flock_count(), before);

    scheduler_release_file_descriptors(other);
    q13_kill(worker);
    q13_kill(leader);
    q13_kill(other);
}

/* The same through the real way out, task_exit_with_code, in the order a
   program whose main thread returns first takes it: the leader ends while
   its thread runs on, and the locks stay; the thread ends, and they go -
   before wait() could say the process has ended. Two of its tasks leaving
   at once is the other shape: the second leaves entirely the moment the
   first lets go of any lock at all, and still the locks are not left behind
   by each deciding the other was last. */
static task_t *m225_lock_leader;
static task_t *m225_lock_thread;
static int m225_lock_thread_left;

static void m225_lock_thread_leaves_meanwhile(void) {
    if (m225_lock_thread_left) {
        return;
    }
    fake_spinlock_on_release(0);
    m225_lock_thread_left = 1;
    scheduler_release_file_descriptors(m225_lock_thread);
}

TEST(scheduler, a_process_keeps_its_record_locks_until_its_last_task_ends) {
    m225_boot();
    fake_arch_set_cpu(0);
    task_t *leader = q13_spawn_program("m225lockexit");
    REQUIRE(leader != NULL);
    task_t *thread = q13_thread_of("m225lockexitthread", leader);
    REQUIRE(thread != NULL);
    leader->parent_id = 0;
    const int process = scheduler_record_lock_owner(leader);
    int before = flock_count();
    CHECK_EQ(flock_set(32, process, OS_FLOCK_WR, 0, 0), 0);
    CHECK_EQ(flock_set(33, scheduler_record_lock_owner(thread), OS_FLOCK_RD, 5, 5), 0);

    m225_exit_on_cpu0(leader, 0, 0);
    /* A program the kernel starts leads a session, and its leader's end
       hangs that session up; this thread is not here to be hung up. */
    m225_quiet(thread);
    os_flock_t who;
    CHECK_EQ(flock_test(32, 0, OS_FLOCK_RD, 0, 1, &who), 1);
    CHECK_EQ(who.pid, process);
    CHECK_EQ(flock_test(33, 0, OS_FLOCK_WR, 5, 1, &who), 1);
    CHECK_EQ(flock_count(), before + 2);

    m225_exit_on_cpu0(thread, 0, 0);
    CHECK_EQ(flock_count(), before);
    CHECK_EQ(flock_test(32, 0, OS_FLOCK_WR, 0, 0, &who), 0);
    scheduler_reap_slot(thread);
    scheduler_reap_slot(leader);

    /* Both at once. */
    m225_lock_leader = q13_spawn_program("m225lockpair");
    REQUIRE(m225_lock_leader != NULL);
    m225_lock_thread = q13_thread_of("m225lockpairthread", m225_lock_leader);
    REQUIRE(m225_lock_thread != NULL);
    CHECK_EQ(flock_set(34, scheduler_record_lock_owner(m225_lock_thread), OS_FLOCK_WR, 0, 0), 0);
    m225_lock_thread_left = 0;
    fake_spinlock_on_release(m225_lock_thread_leaves_meanwhile);
    scheduler_release_file_descriptors(m225_lock_leader);
    fake_spinlock_on_release(0);
    CHECK(m225_lock_thread_left);
    CHECK_EQ(flock_count(), before);
    q13_kill(m225_lock_thread);
    q13_kill(m225_lock_leader);
}

/* M225 (process-lifetimes). /proc/<pid>/cmdline was answered with a pointer
   to the owner's record, taken with no lock, and the process ending on
   another processor freed that record at its reap - a read racing the end
   read freed memory. The answer is a COPY now, made under the lock the reap
   takes the record away under.

   The other processor here reaps the slot the moment the read lets go of a
   lock. So two things are graded at that moment: that the read took the
   lock at all - one that takes none is not ordered against the reap, and
   the reap here never gets a chance to happen during it - and that by then
   the reader already HAS the record, because once it lets go the record
   can be freed and its memory handed to somebody else. (The kernel heap is
   not the host's, so the sanitizer cannot see a read of a freed block here;
   this is what can.) And through a thread, whose cmdline is its leader's. */
static const char m225_cmdline_expected[] = "m225cmd\0--type=renderer\0--lang=en-US";
static task_t *m225_cmdline_victim;
static char *m225_cmdline_out;
static int m225_cmdline_reaped;
static int m225_cmdline_had_it_when_let_go;

static void m225_cmdline_reap_now(void) {
    fake_spinlock_on_release(0);
    if (m225_cmdline_reaped) {
        return;
    }
    m225_cmdline_reaped = 1;
    m225_cmdline_had_it_when_let_go =
        memcmp(m225_cmdline_out, m225_cmdline_expected, sizeof(m225_cmdline_expected)) == 0;
    m225_cmdline_victim->state = TASK_TERMINATED;
    scheduler_reap_slot(m225_cmdline_victim);
}

TEST(scheduler, a_cmdline_read_racing_the_reap_never_reads_a_freed_record) {
    static const char *const argv[] = {"m225cmd", "--type=renderer", "--lang=en-US", 0};
    for (int through_thread = 0; through_thread < 2; through_thread++) {
        q13_boot();
        fake_arch_set_cpu(0);
        task_t *p = q13_spawn_program("m225cmd");
        REQUIRE(p != NULL);
        scheduler_set_cmdline(p, argv);
        task_t *thread = (task_t *)0;
        int asked = p->id;
        if (through_thread) {
            thread = q13_thread_of("m225cmdthread", p);
            REQUIRE(thread != NULL);
            asked = thread->id;
            /* The thread has ended and is waiting to be joined; the
               leader's reap is the one that frees the record. */
            thread->state = TASK_TERMINATED;
            REQUIRE(scheduler_task_by_id(asked) == thread);
        }
        scheduler_release_file_descriptors(p);

        char out[128];
        memset(out, 0x5A, sizeof(out));
        const int pid = p->id;
        m225_cmdline_victim = p;
        m225_cmdline_out = out;
        m225_cmdline_reaped = 0;
        m225_cmdline_had_it_when_let_go = 0;
        fake_spinlock_on_release(m225_cmdline_reap_now);
        int n = scheduler_copy_cmdline(asked, out, sizeof(out));
        fake_spinlock_on_release(0);
        CHECK(m225_cmdline_reaped);
        CHECK(m225_cmdline_had_it_when_let_go);
        if (!m225_cmdline_reaped) {
            m225_cmdline_reap_now();
        }
        CHECK(scheduler_task_by_id(pid) == NULL);

        CHECK_EQ(n, (int)sizeof(m225_cmdline_expected));
        CHECK(n > 0 && memcmp(out, m225_cmdline_expected, (size_t)n) == 0);
        if (thread) {
            scheduler_reap_slot(thread);
        }
    }
}

/* What a read is given when there is nothing recorded, and what one of a
   task that has gone is given - and that a record longer than the reader's
   buffer is cut at the buffer, not past it. */
TEST(scheduler, a_cmdline_copy_stays_inside_the_buffer_it_is_given) {
    q13_boot();
    fake_arch_set_cpu(0);
    task_t *p = q13_spawn_program("m225short");
    REQUIRE(p != NULL);
    char out[8];
    memset(out, 0x5A, sizeof(out));
    CHECK_EQ(scheduler_copy_cmdline(p->id, out, sizeof(out)), (int)sizeof("m225sho"));
    CHECK(memcmp(out, "m225sho", sizeof("m225sho")) == 0);

    static const char *const argv[] = {"m225short", "--a-long-argument", 0};
    scheduler_set_cmdline(p, argv);
    memset(out, 0x5A, sizeof(out));
    char guard = 0x5A;
    CHECK_EQ(scheduler_copy_cmdline(p->id, out, 5), 5);
    CHECK(memcmp(out, "m225s", 5) == 0);
    CHECK_EQ(out[5], guard);
    CHECK_EQ(scheduler_copy_cmdline(p->id, (char *)0, 5), -1);

    const int pid = p->id;
    scheduler_release_file_descriptors(p);
    q13_kill(p);
    CHECK_EQ(scheduler_copy_cmdline(pid, out, sizeof(out)), -1);
}

/* M225 (process-lifetimes). The environment record is the PROCESS's too:
   every reader asks the owner - a thread has none of its own - and the
   leader's exit freed it while its threads ran on. A thread that spawned
   after its main thread had returned read the freed block, or found none
   and gave its child an empty environment. It goes at the last member's
   reap now, as the command line does. */
TEST(scheduler, a_process_keeps_its_environment_while_any_of_its_threads_runs) {
    m225_boot();
    fake_arch_set_cpu(0);
    task_t *leader = q13_spawn_program("m225envleader");
    REQUIRE(leader != NULL);
    static const char env[] = "HOME=/home\0LANG=C";
    REQUIRE(scheduler_set_env(leader, env, sizeof(env), 2) == 0);
    task_t *thread = q13_thread_of("m225envthread", leader);
    REQUIRE(thread != NULL);
    leader->parent_id = 0;

    m225_exit_on_cpu0(leader, 0, 0);
    m225_quiet(thread);
    char out[64];
    uint32_t count = 0;
    CHECK_EQ(scheduler_copy_env(thread, out, sizeof(out), &count), (uint32_t)sizeof(env));
    CHECK_EQ(count, 2u);
    CHECK(memcmp(out, env, sizeof(env)) == 0);
    /* Too small a buffer gets nothing rather than half a string. */
    CHECK_EQ(scheduler_copy_env(thread, out, 4, &count), 0u);
    CHECK_EQ(count, 0u);

    m225_exit_on_cpu0(thread, 0, 0);
    scheduler_reap_slot(thread);
    scheduler_reap_slot(leader);
}

/* M225 (process-lifetimes). task_spawn_thread took ANY leader, and "a table
   at zero is never revived" (descriptor_table_reference panics at <= 0)
   held only because its one caller passed scheduler_current(). A leader
   whose process was exiting on another processor - its last task's table
   taken away and about to be freed - would have been handed a thread with
   a table nobody held, and one in a different process would have been given
   a thread of somebody else's. Now only a task running in the process may
   ask, and nothing is counted for one that may not. */
TEST(scheduler, only_a_task_running_in_a_process_can_give_it_a_thread) {
    q13_boot();
    fake_arch_set_cpu(0);
    task_t *process = q13_spawn_program("m225threads");
    task_t *stranger = q13_spawn_program("m225threadstranger");
    REQUIRE(process != NULL);
    REQUIRE(stranger != NULL);
    file_descriptor_table_t *table = process->descriptor_table;
    REQUIRE(table != NULL);
    const int references = table->references;
    const int live = scheduler_live_task_count();
    const int cpu = smp_current_cpu();

    /* From another process: refused, nothing counted, no slot taken. */
    task_t *was = scheduler_host_test_seat(cpu, stranger);
    task_t *foreign = task_spawn_thread("m225foreign", process, q13_body, (void *)0);
    scheduler_host_test_seat(cpu, was);
    CHECK(foreign == NULL);
    CHECK_EQ(table->references, references);
    CHECK_EQ(scheduler_live_task_count(), live);

    /* From the kernel task the test stands on: the same. */
    CHECK(task_spawn_thread("m225fromkernel", process, q13_body, (void *)0) == NULL);
    CHECK_EQ(table->references, references);

    /* From the process itself, and from a thread of it asking for its own
       process. */
    task_t *mine = q13_thread_of("m225mine", process);
    REQUIRE(mine != NULL);
    CHECK_EQ(table->references, references + 1);
    task_t *sibling = q13_thread_of("m225sibling", mine);
    REQUIRE(sibling != NULL);
    CHECK_EQ(sibling->tgid, process->tgid);
    CHECK_EQ(table->references, references + 2);

    /* A task that has let go of its table - it is on its way out - holds no
       reference to lend: refused, rather than a thread with no table. */
    scheduler_release_file_descriptors(sibling);
    CHECK(q13_thread_of("m225late", sibling) == NULL);
    CHECK_EQ(table->references, references + 1);

    /* Nor may a stranger ask in the name of a thread of the process. */
    was = scheduler_host_test_seat(cpu, stranger);
    CHECK(task_spawn_thread("m225foreign2", mine, q13_body, (void *)0) == NULL);
    scheduler_host_test_seat(cpu, was);
    CHECK_EQ(table->references, references + 1);
    CHECK(task_spawn_thread("m225none", (task_t *)0, q13_body, (void *)0) == NULL);

    scheduler_release_file_descriptors(mine);
    scheduler_release_file_descriptors(process);
    scheduler_release_file_descriptors(stranger);
    q13_kill(sibling);
    q13_kill(mine);
    q13_kill(process);
    q13_kill(stranger);
}

/* M225 (fd-use-holds): a call that uses a descriptor holds the object with
   a reference of its own while another thread shares the table - and a task
   can END inside such a call: a fatal signal is acted on in the middle of a
   blocking wait, and from the timer tick. The call never gets to give its
   reference back, so the exit does. Without that a reader killed while
   blocked on a pipe kept the pipe's read end open for good: no EPIPE for
   the writer, and the pipe never freed. */
TEST(scheduler, a_thread_that_ends_inside_a_call_gives_back_what_the_call_held) {
    m225_boot();
    fake_arch_set_cpu(0);
    task_t *leader = q13_spawn_program("m225uses");
    REQUIRE(leader != NULL);
    task_t *thread = q13_thread_of("m225usesthread", leader);
    REQUIRE(thread != NULL);
    m225_quiet(thread);
    file_descriptor_table_t *table = leader->descriptor_table;
    REQUIRE(table != NULL && table == thread->descriptor_table);

    int before = fake_objects_pipe_write_refs();
    file_descriptor_slot_t end;
    memset(&end, 0, sizeof(end));
    end.type = FILE_DESCRIPTOR_PIPE_WRITE;
    end.pipe = (struct pipe *)0x2000;
    int fd = file_descriptor_claim(table, 3);
    REQUIRE(fd >= 3);
    file_descriptor_install(table, fd, &end);

    /* The thread is part way through a write() on it - and the descriptor
       is closed by its sibling meanwhile, as a program's other thread may. */
    file_descriptor_use_t in_flight;
    REQUIRE(file_descriptor_get(thread, fd, &in_flight) == 0);
    CHECK_EQ(in_flight.counted, 1);
    CHECK_EQ(fake_objects_pipe_write_refs(), before + 1);
    file_descriptor_slot_t gone;
    REQUIRE(file_descriptor_detach(table, fd, &gone) == 0);
    file_descriptor_release(&gone);
    CHECK_EQ(fake_objects_pipe_write_refs(), before); /* only the call holds it */

    m225_exit_on_cpu0(thread, 0, SIGKILL);
    CHECK_EQ(fake_objects_pipe_write_refs(), before - 1); /* and now nobody */
    CHECK(thread->descriptor_uses == NULL);
    scheduler_reap_slot(thread);
    m225_quiet(leader);
    scheduler_release_file_descriptors(leader);
    q13_kill(leader);
}

/* M225 (fd-use-holds): fork copies its parent's environment and command
   line, and it took a 4 KiB block for each whatever there was to copy - so a
   fork of a process with NO environment could fail with ENOMEM for a copy it
   was never going to make. Here the heap has no block of 4 KiB left and the
   page allocator gives one more allocation (the child's kernel stack): the
   fork must not need any other. */
TEST(scheduler, a_fork_of_a_process_with_no_environment_allocates_nothing_for_one) {
    m225_boot();
    task_t *parent = q13_spawn("m225bare");
    REQUIRE(parent != NULL);
    scheduler_release_env(parent);
    scheduler_release_cmdline(parent);
    REQUIRE(parent->env_block == NULL && parent->cmdline_block == NULL);
    REQUIRE(m225_place(parent, 0));
    isr_regs_t regs;
    memset(&regs, 0, sizeof(regs));

    /* Room for the child's descriptor table, kept back while the rest of the
       heap is used up, then handed back for the fork to find. */
    void *table_room = kmalloc(sizeof(file_descriptor_table_t));
    REQUIRE(table_room != NULL);
    fake_physical_memory_fail_after((int64_t)fake_physical_memory_total_allocs());
    void *filled = NULL;
    int blocks = 0;
    for (;;) {
        void **block = (void **)kmalloc(4096);
        if (!block) {
            break;
        }
        *block = filled;
        filled = block;
        blocks++;
    }
    kfree(table_room);
    /* One more allocation: the stack. */
    fake_physical_memory_fail_after((int64_t)fake_physical_memory_total_allocs() + 1);

    task_t *child = task_fork(parent->pml4_phys, &regs);
    fake_physical_memory_fail_after(-1);
    while (filled) {
        void *next = *(void **)filled;
        kfree(filled);
        filled = next;
    }
    CHECK_MSG(child != NULL, "a fork of a process with no environment failed for want of "
                             "a block to copy no environment into (%d 4 KiB blocks filled "
                             "the heap first)",
              blocks);
    if (child) {
        CHECK(child->env_block == NULL);
        CHECK_EQ(child->env_count, 0u);
        q13_kill(child);
    }

    /* And one that HAS an environment still gets a copy of it, exactly its
       size. */
    static const char env[] = "HOME=/\0TERM=lean";
    REQUIRE(scheduler_set_env(parent, env, sizeof(env), 2) == 0);
    uint32_t length = 0, count = 0;
    int no_memory = 1;
    char *copy = scheduler_duplicate_env(parent, &length, &count, &no_memory);
    REQUIRE(copy != NULL);
    CHECK_EQ(no_memory, 0);
    CHECK_EQ(length, (uint32_t)sizeof(env));
    CHECK_EQ(count, 2u);
    CHECK(memcmp(copy, env, sizeof(env)) == 0);
    kfree(copy);
    scheduler_release_env(parent);
    copy = scheduler_duplicate_env(parent, &length, &count, &no_memory);
    CHECK(copy == NULL);
    CHECK_EQ(no_memory, 0);
    CHECK_EQ(length, 0u);
    q13_kill(parent);
}
