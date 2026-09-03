/* tests/fakes/fake_arch.c - Q13
 *
 * The machine kernel/sched/sched.c runs on, reduced to the questions it
 * actually asks and the answers a test wants to control.
 *
 * ---- what this is NOT, and the line matters ---------------------------
 *
 * It is not a simulated x86-64. Q13's whole claim is that most of
 * sched.c is *policy* - who runs next, what a state transition means,
 * which task a signal lands on - and that policy is a pure function of a
 * task table. This file supplies the four things underneath it that a
 * host process cannot have, and makes each one observable rather than
 * silent:
 *
 *   - **The CPU number.** A test sets it, which is what lets one process
 *     drive a multi-core scheduler one core at a time. The real one
 *     reads the task register.
 *   - **The context switch.** A no-op that counts. This is the honest
 *     limit of the tier and it is worth stating plainly: after
 *     `schedule()` picks a new task, `current_task[cpu]` names it and
 *     control returns to the caller anyway, because there is no second
 *     stack to go to. So these tests grade **which task the scheduler
 *     chose and what it did to the table**, and they cannot grade the
 *     switch itself. The switch is x86 assembly and the boot self-tests
 *     are what prove it.
 *   - **The FPU and the thread pointer.** Recorded, because "did the
 *     scheduler load the incoming task's FS base" is exactly the kind of
 *     question this tier exists to answer, and the fork half of that
 *     question was a real bug (M85's second attempt).
 *   - **The stack-pointer check.** M106 panics if a CPU is switching
 *     away from a task whose stack it is not standing on. On a host
 *     there is one stack and no task is ever really on it, so a test
 *     says where the CPU is standing; the default is "inside the
 *     outgoing task's stack", which is the answer that lets the check
 *     pass, and a test that wants to prove the check FIRES sets it
 *     somewhere else.
 */
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "fakes/fakes.h"

void panic(const char *msg);

/* ---- which CPU is this ------------------------------------------------ */

static int cur_cpu;

void fake_arch_set_cpu(int cpu) { cur_cpu = cpu; }
int smp_current_cpu(void) { return cur_cpu; }
int gdt_current_cpu(void) { return cur_cpu; }

int smp_cpu_count = 1;

static int broadcasts;
void fake_arch_reset(void);
int fake_arch_broadcasts(void) { return broadcasts; }
void smp_broadcast_schedule_tick(void) { broadcasts++; }

/* ---- the context switch, counted rather than performed ---------------- */

static uint64_t switches;
uint64_t fake_arch_switches(void) { return switches; }

void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp);
void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp) {
    /* The one thing the real one does that the table can observe: it
     * writes the outgoing task's resume point. Written here too, with a
     * value that is recognisably not a stack pointer, so a test that
     * asserts on rsp is asserting on something this file produced rather
     * than on uninitialised memory. */
    if (old_rsp_out) {
        *old_rsp_out = 0xC0DEC0DEC0DEC0DEull;
    }
    (void)new_rsp;
    switches++;
}

void fork_return_to_user(void *frame) __attribute__((noreturn));
void fork_return_to_user(void *frame) {
    (void)frame;
    /* A forked child never returns from here on the real machine either:
     * it `iretq`s into ring 3. There is no ring 3 to go to, and a test
     * that reaches this has driven the scheduler into a path only a real
     * user-mode task takes - which is a broken test rather than a broken
     * scheduler, so it says so and stops. */
    panic("fake_arch: a task tried to return to user mode");
    for (;;) {
    }
}

/* ---- the two instructions the scheduler names ------------------------- */

void cpu_enable_interrupts(void) {}
void cpu_spin_hint(void) {}

static uint64_t claimed_sp;
static int sp_claimed;

void fake_arch_stand_on(uint64_t sp) {
    claimed_sp = sp;
    sp_claimed = 1;
}

unsigned long long cpu_stack_pointer(void) {
    if (!sp_claimed) {
        /* Deliberately 0, which fails M106's check loudly.
         *
         * There is no honest default: on a host the real answer is "this
         * process's stack", which is inside no task and would make the
         * check fire *by accident* on every switch. A test that drives
         * the scheduler has to say where the CPU is standing - see
         * q13_stand_on_current in tests/test_sched.c, which is one line
         * and makes the claim explicit at every call. */
        return 0;
    }
    return claimed_sp;
}

/* ---- the MSRs -------------------------------------------------------- */

#define MSR_SLOTS 4
static unsigned int msr_num[MSR_SLOTS];
static unsigned long long msr_val[MSR_SLOTS];

void fake_cpu_record_msr(unsigned int msr, unsigned long long value);
void fake_cpu_record_msr(unsigned int msr, unsigned long long value) {
    for (int i = 0; i < MSR_SLOTS; i++) {
        if (msr_num[i] == msr || msr_num[i] == 0) {
            msr_num[i] = msr;
            msr_val[i] = value;
            return;
        }
    }
}

unsigned long long fake_cpu_last_msr(unsigned int msr);
unsigned long long fake_cpu_last_msr(unsigned int msr) {
    for (int i = 0; i < MSR_SLOTS; i++) {
        if (msr_num[i] == msr) {
            return msr_val[i];
        }
    }
    return 0;
}

void cpu_write_msr(unsigned int msr, unsigned long long value) {
    fake_cpu_record_msr(msr, value);
}

unsigned long long cpu_read_msr(unsigned int msr) {
    return fake_cpu_last_msr(msr);
}

/* ---- the TSS, the FPU, and the timer hook ----------------------------- */

#define FAKE_MAX_CPUS 8
static uint64_t rsp0[FAKE_MAX_CPUS];

void tss_set_rsp0(int cpu_id, uint64_t v) {
    if (cpu_id >= 0 && cpu_id < FAKE_MAX_CPUS) {
        rsp0[cpu_id] = v;
    }
}
uint64_t tss_get_rsp0(int cpu_id) {
    return (cpu_id >= 0 && cpu_id < FAKE_MAX_CPUS) ? rsp0[cpu_id] : 0;
}
void gdt_get_table_ptr(uint16_t *limit_out, uint64_t *base_out) {
    if (limit_out) {
        *limit_out = 0;
    }
    if (base_out) {
        *base_out = 0;
    }
}

/* The FPU area is a byte array either way. Zeroing on init and copying
 * on switch is what the real one does to the same bytes, so a test can
 * assert that a task's state survived another task running - which is
 * M63's claim and has never been checked anywhere but on the machine. */
static uint8_t fpu_scratch[512];

void fpu_state_init(uint8_t *state) {
    if (state) {
        memset(state, 0, 512);
    }
}
void fpu_save(uint8_t *state) {
    if (state) {
        memcpy(state, fpu_scratch, sizeof(fpu_scratch));
    }
}
void fpu_restore(const uint8_t *state) {
    if (state) {
        memcpy(fpu_scratch, state, sizeof(fpu_scratch));
    }
}
uint8_t *fake_arch_fpu_scratch(void) { return fpu_scratch; }

static void (*tick_hook)(void);
void pit_set_tick_hook(void (*hook)(void)) { tick_hook = hook; }
void fake_arch_fire_tick_hook(void) {
    if (tick_hook) {
        tick_hook();
    }
}

void fake_arch_reset(void) {
    cur_cpu = 0;
    broadcasts = 0;
    switches = 0;
    sp_claimed = 0;
    claimed_sp = 0;
    memset(msr_num, 0, sizeof(msr_num));
    memset(msr_val, 0, sizeof(msr_val));
    memset(rsp0, 0, sizeof(rsp0));
    memset(fpu_scratch, 0, sizeof(fpu_scratch));
}
