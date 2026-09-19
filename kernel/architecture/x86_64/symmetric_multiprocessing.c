#include "symmetric_multiprocessing.h"

#include <stdint.h>

#include "acpi/acpi.h"
#include "drivers/kernel_log.h"
#include "drivers/pit.h"
#include "floating_point_unit.h"
#include "global_descriptor_table.h"
#include "interrupt_descriptor_table.h"
#include "interrupt_service_routines.h"
#include "lapic.h"
#include "memory_management/heap.h"
#include "memory_management/virtual_memory.h"
#include "library/spinlock.h"
#include "panic.h"
#include "profile/sampler.h"
#include "scheduler/scheduler.h"

static cpu_info_t smp_cpus[MAX_CPUS];
int smp_cpu_count = 1;

static volatile int initialized;

#define AP_STACK_SIZE (16 * 1024)

#define AP_PARAMETERS_ADDRESS       0x7000ULL
#define AP_OFF_CR3           0
#define AP_OFF_STACK_TOP     8
#define AP_OFF_ENTRY64       16
#define AP_OFF_GDT_LIMIT     24
#define AP_OFF_GDT_BASE      26
#define AP_OFF_CPU_ID        34
#define AP_OFF_AP_READY      38
#define AP_OFF_NX            42
#define AP_PARAMETERS_SIZE       43

#define AP_TRAMPOLINE_LOAD_ADDRESS 0x8000ULL
#define AP_TRAMPOLINE_VECTOR ((uint32_t)(AP_TRAMPOLINE_LOAD_ADDRESS >> 12))

extern const uint8_t ap_trampoline_start[];
extern const uint8_t ap_trampoline_end[];
extern void ap_entry_asm_stub(void);

static void verify_cpu_identity(uint32_t cpu_id);

static inline void ap_parameters_write64(uint64_t offset, uint64_t value) {
    *(volatile uint64_t *)(uintptr_t)(AP_PARAMETERS_ADDRESS + offset) = value;
}
static inline void ap_parameters_write32(uint64_t offset, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(AP_PARAMETERS_ADDRESS + offset) = value;
}
static inline void ap_parameters_write16(uint64_t offset, uint16_t value) {
    *(volatile uint16_t *)(uintptr_t)(AP_PARAMETERS_ADDRESS + offset) = value;
}
static inline void ap_parameters_write8(uint64_t offset, uint8_t value) {
    *(volatile uint8_t *)(uintptr_t)(AP_PARAMETERS_ADDRESS + offset) = value;
}
static inline uint32_t ap_parameters_read32(uint64_t offset) {
    return *(volatile uint32_t *)(uintptr_t)(AP_PARAMETERS_ADDRESS + offset);
}

void ap_main(uint32_t cpu_id) {
    gdt_init_ap((int)cpu_id);
    idt_load_ap();
    fpu_init_cpu();
    virtual_memory_enable_nx_this_cpu();
    lapic_init_this_cpu();
    verify_cpu_identity(cpu_id);
    scheduler_init_ap((int)cpu_id);

    smp_cpus[cpu_id].online = 1;

    kernel_log_puts("[smp] AP online: cpu_id=");
    kernel_log_put_hex32(cpu_id);
    kernel_log_puts(" apic_id=");
    kernel_log_put_hex32(smp_cpus[cpu_id].apic_id);
    kernel_log_putc('\n');

    __asm__ volatile("sti");
    for (;;) {
        schedule();
        __asm__ volatile("hlt");
    }
}

int smp_current_cpu(void) {
    return gdt_current_cpu();
}

static void verify_cpu_identity(uint32_t cpu_id) {
    uint32_t id = lapic_id();
    int by_tss = gdt_current_cpu();
    int by_lapic = -1;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (smp_cpus[i].apic_id == id && (i == 0 || (uint32_t)i <= cpu_id)) {
            by_lapic = i;
            break;
        }
    }
    if (by_tss != (int)cpu_id || by_lapic != (int)cpu_id) {
        kernel_log_puts("[smp] this core was started as cpu ");
        kernel_log_put_dec(cpu_id);
        kernel_log_puts(" but its task register says ");
        kernel_log_put_dec((uint32_t)by_tss);
        kernel_log_puts(" and its LAPIC id 0x");
        kernel_log_put_hex32(id);
        kernel_log_puts(" maps to ");
        kernel_log_put_dec((uint32_t)by_lapic);
        kernel_log_putc('\n');
        panic("smp: a core does not agree with the kernel about which core it is");
    }
}

int smp_is_initialized(void) {
    return initialized;
}

void smp_broadcast_schedule_tick(void) {
    if (!initialized) {
        return;
    }
    lapic_send_ipi_all_excl_self(IPI_SCHEDULE_VECTOR | LAPIC_ICR_DELIVERY_FIXED);
}

/* One core at a time may ask, because the acknowledgement count below is a
   single number rather than one per request. The lock is taken WITHOUT
   disabling interrupts on purpose: the asking core spins here waiting for
   every other core to take an interrupt, and a core that cannot take one
   would never answer. For the same reason nothing that holds a lock another
   core takes with interrupts off may call this. */
static spinlock_t shootdown_lock;
static volatile int shootdown_acknowledgements;

void smp_tlb_shootdown(void) {
    if (!initialized || smp_cpu_count < 2) {
        return;
    }

    spin_lock(&shootdown_lock);
    __atomic_store_n(&shootdown_acknowledgements, smp_cpu_count - 1,
                     __ATOMIC_SEQ_CST);
    lapic_send_ipi_all_excl_self(IPI_TLB_SHOOTDOWN_VECTOR |
                                 LAPIC_ICR_DELIVERY_FIXED);
    while (__atomic_load_n(&shootdown_acknowledgements, __ATOMIC_SEQ_CST) > 0) {
        cpu_spin_hint();
    }
    spin_unlock(&shootdown_lock);
}

void smp_tlb_shootdown_acknowledge(void) {
    virtual_memory_flush_local_tlb();
    __atomic_sub_fetch(&shootdown_acknowledgements, 1, __ATOMIC_SEQ_CST);
}

void smp_halt_other_cpus(void) {
    if (!initialized) {
        return;
    }
    lapic_send_ipi_all_excl_self(LAPIC_ICR_DELIVERY_NMI);
}

static int start_ap(int cpu_id, uint32_t apic_id) {
    uint8_t *stack = (uint8_t *)kmalloc(AP_STACK_SIZE);
    if (!stack) {
        panic("smp: out of heap memory for an AP stack");
    }

    uint16_t gdt_limit;
    uint64_t gdt_base;
    gdt_get_table_pointer(&gdt_limit, &gdt_base);

    ap_parameters_write32(AP_OFF_AP_READY, 0);
    ap_parameters_write64(AP_OFF_CR3, virtual_memory_kernel_pml4_phys());
    ap_parameters_write64(AP_OFF_STACK_TOP, (uint64_t)(uintptr_t)(stack + AP_STACK_SIZE));
    ap_parameters_write64(AP_OFF_ENTRY64, (uint64_t)(uintptr_t)ap_entry_asm_stub);
    ap_parameters_write16(AP_OFF_GDT_LIMIT, gdt_limit);
    ap_parameters_write64(AP_OFF_GDT_BASE, gdt_base);
    ap_parameters_write32(AP_OFF_CPU_ID, (uint32_t)cpu_id);
    ap_parameters_write8(AP_OFF_NX, virtual_memory_nx_enabled() ? 1u : 0u);

    uint8_t *destination = (uint8_t *)(uintptr_t)AP_TRAMPOLINE_LOAD_ADDRESS;
    uint64_t blob_length = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
    for (uint64_t i = 0; i < blob_length; i++) {
        destination[i] = ap_trampoline_start[i];
    }

    lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_INIT | LAPIC_ICR_LEVEL_ASSERT);
    pit_sleep_ms(10);
    lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_STARTUP | AP_TRAMPOLINE_VECTOR);
    pit_sleep_ms(10);
    lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_STARTUP | AP_TRAMPOLINE_VECTOR);

    uint64_t deadline = pit_get_ticks() + PIT_HZ;
    while (ap_parameters_read32(AP_OFF_AP_READY) == 0 && pit_get_ticks() < deadline) {
        __asm__ volatile("pause");
    }
    return ap_parameters_read32(AP_OFF_AP_READY) != 0;
}

void smp_init(void) {
    acpi_madt_info_t madt;
    if (!acpi_find_madt(&madt)) {
        smp_cpus[0].apic_id = 0;
        smp_cpus[0].online = 1;
        initialized = 1;
        kernel_log_puts("[smp] running single-core.\n");
        return;
    }

    lapic_init(madt.lapic_base);
    uint32_t bsp_apic_id = lapic_id();
    smp_cpus[0].apic_id = bsp_apic_id;
    smp_cpus[0].online = 1;
    smp_cpu_count = 1;

    int next_cpu = 1;
    for (int i = 0; i < madt.cpu_count; i++) {
        if (madt.cpu_apic_ids[i] == bsp_apic_id) {
            continue;
        }
        if (next_cpu >= MAX_CPUS) {
            kernel_log_puts("[smp] MAX_CPUS reached - ignoring the remaining CPU(s) listed in the MADT.\n");
            break;
        }
        smp_cpus[next_cpu].apic_id = madt.cpu_apic_ids[i];
        kernel_log_puts("[smp] starting AP apic_id=0x");
        kernel_log_put_hex32(madt.cpu_apic_ids[i]);
        kernel_log_puts("...\n");
        if (start_ap(next_cpu, madt.cpu_apic_ids[i])) {
            smp_cpu_count = next_cpu + 1;
            next_cpu++;
        } else {
            kernel_log_puts("[smp] AP did not check in within the timeout - skipping it.\n");
        }
    }

    initialized = 1;
    kernel_log_puts("[smp] ");
    kernel_log_put_hex32((uint32_t)smp_cpu_count);
    kernel_log_puts(" CPU(s) online.\n");
}

void lapic_vector_handler(isr_regs_t *regs) {
    lapic_send_eoi();
    if (regs->vector == IPI_SCHEDULE_VECTOR) {
        profile_sample(regs);
        scheduler_account_tick((regs->cs & 3) != 0);
        scheduler_tick_cpu(smp_current_cpu());
    } else if (regs->vector == IPI_TLB_SHOOTDOWN_VECTOR) {
        smp_tlb_shootdown_acknowledge();
    }
}
