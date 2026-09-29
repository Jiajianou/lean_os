#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "fakes/fakes.h"

void panic(const char *message);

static int current_cpu;

/* M191. Which CPU the caller is on is only true while it cannot be moved, and
   on the real machine what stops a move is interrupts being off. The fake
   keeps that one bit so a test can ask whether the scheduler ever read the
   index while a tick could still have migrated it. */
static int interrupts_on = 1;
static int unguarded_cpu_reads;

uint64_t fake_irq_save_disable(void) {
    uint64_t was = (uint64_t)interrupts_on;
    interrupts_on = 0;
    return was;
}

void fake_irq_restore(uint64_t flags) { interrupts_on = flags ? 1 : 0; }

void fake_arch_reset_unguarded_cpu_reads(void);
int fake_arch_unguarded_cpu_reads(void);
void fake_arch_reset_unguarded_cpu_reads(void) {
    interrupts_on = 1;
    unguarded_cpu_reads = 0;
}
int fake_arch_unguarded_cpu_reads(void) { return unguarded_cpu_reads; }

void fake_arch_set_cpu(int cpu) { current_cpu = cpu; }
int smp_current_cpu(void) {
    if (interrupts_on) {
        unguarded_cpu_reads++;
    }
    return current_cpu;
}
int gdt_current_cpu(void) { return smp_current_cpu(); }

int smp_cpu_count = 1;

static int broadcasts;
void fake_arch_reset(void);
int fake_arch_broadcasts(void) { return broadcasts; }
void smp_broadcast_schedule_tick(void) { broadcasts++; }
static int smp_ready = 0;
static int reschedules[16];
void fake_arch_set_smp_initialized(int on) { smp_ready = on; }
int smp_is_initialized(void) { return smp_ready; }
void smp_send_reschedule(int cpu) {
    if (cpu >= 0 && cpu < 16) {
        reschedules[cpu]++;
    }
}
int fake_arch_reschedules_sent(int cpu) { return (cpu >= 0 && cpu < 16) ? reschedules[cpu] : 0; }
void fake_arch_reset_reschedules(void) {
    for (int i = 0; i < 16; i++) {
        reschedules[i] = 0;
    }
}
static int shootdowns;
void smp_tlb_shootdown(void) { shootdowns++; }
int fake_smp_shootdowns(void) { return shootdowns; }

static uint64_t switches;
uint64_t fake_arch_switches(void) { return switches; }

void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp);
unsigned long long cpu_stack_pointer(void);
void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp) {
    /* M182: this used to save 0xC0DEC0DEC0DEC0DE, on the reasoning that
       nothing should depend on what a fake switch leaves behind. Something
       does now: schedule() checks that the task it is about to resume has a
       saved stack pointer inside its own stack, and a sentinel is outside
       every stack there is - so every task the host tests had ever switched
       away from looked corrupt. It saves what it is standing on, which is
       what the real one does. */
    if (old_rsp_out) {
        *old_rsp_out = (uint64_t)cpu_stack_pointer();
    }
    (void)new_rsp;
    switches++;
}

void fork_return_to_user(void *frame) __attribute__((noreturn));
void fork_return_to_user(void *frame) {
    (void)frame;
    panic("fake_arch: a task tried to return to user mode");
    for (;;) {
    }
}

void cpu_enable_interrupts(void) { interrupts_on = 1; }
void cpu_disable_interrupts(void) { interrupts_on = 0; }
void cpu_spin_hint(void) {}

static uint64_t claimed_sp;
static int sp_claimed;

void fake_arch_stand_on(uint64_t sp) {
    claimed_sp = sp;
    sp_claimed = 1;
}

unsigned long long cpu_stack_pointer(void) {
    if (!sp_claimed) {
        return 0;
    }
    return claimed_sp;
}

#define MSR_SLOTS 4
static unsigned int msr_number[MSR_SLOTS];
static unsigned long long msr_value[MSR_SLOTS];

void fake_cpu_record_msr(unsigned int msr, unsigned long long value);
void fake_cpu_record_msr(unsigned int msr, unsigned long long value) {
    for (int i = 0; i < MSR_SLOTS; i++) {
        if (msr_number[i] == msr || msr_number[i] == 0) {
            msr_number[i] = msr;
            msr_value[i] = value;
            return;
        }
    }
}

unsigned long long fake_cpu_last_msr(unsigned int msr);
unsigned long long fake_cpu_last_msr(unsigned int msr) {
    for (int i = 0; i < MSR_SLOTS; i++) {
        if (msr_number[i] == msr) {
            return msr_value[i];
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
void gdt_get_table_pointer(uint16_t *limit_out, uint64_t *base_out) {
    if (limit_out) {
        *limit_out = 0;
    }
    if (base_out) {
        *base_out = 0;
    }
}

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
    current_cpu = 0;
    broadcasts = 0;
    switches = 0;
    sp_claimed = 0;
    claimed_sp = 0;
    memset(msr_number, 0, sizeof(msr_number));
    memset(msr_value, 0, sizeof(msr_value));
    memset(rsp0, 0, sizeof(rsp0));
    memset(fpu_scratch, 0, sizeof(fpu_scratch));
}
