#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "fakes/fakes.h"

void panic(const char *msg);

static int current_cpu;

void fake_arch_set_cpu(int cpu) { current_cpu = cpu; }
int smp_current_cpu(void) { return current_cpu; }
int gdt_current_cpu(void) { return current_cpu; }

int smp_cpu_count = 1;

static int broadcasts;
void fake_arch_reset(void);
int fake_arch_broadcasts(void) { return broadcasts; }
void smp_broadcast_schedule_tick(void) { broadcasts++; }

static uint64_t switches;
uint64_t fake_arch_switches(void) { return switches; }

void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp);
void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp) {
    if (old_rsp_out) {
        *old_rsp_out = 0xC0DEC0DEC0DEC0DEull;
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
