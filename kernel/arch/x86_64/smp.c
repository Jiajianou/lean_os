#include "smp.h"

#include <stdint.h>

#include "acpi/acpi.h"
#include "drivers/klog.h"
#include "drivers/pit.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "lapic.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "panic.h"
#include "sched/sched.h"

cpu_info_t smp_cpus[MAX_CPUS];
int smp_cpu_count = 1;

static volatile int initialized;

#define AP_STACK_SIZE (16 * 1024)

/* Shared low-memory scratch smp.c (C, on the BSP) and ap_trampoline.asm/
 * ap_entry.asm (raw asm, on the AP being started) both read/write - see
 * kernel/arch/x86_64/ap_trampoline.asm's own header comment for the full
 * picture of how an AP gets from real mode to here. There's no way to
 * share a real C struct type across that boundary (the trampoline is
 * assembled completely separately, as flat 16-bit machine code - see the
 * Makefile), so both sides just have to agree on these exact byte offsets
 * by hand, the same technique isr.h/isr_asm.asm already use for the
 * register-dump layout. AP_PARAMS_ADDR (0x7000) and AP_TRAMPOLINE_LOAD_ADDR
 * (0x8000) both sit well within the low 1 MiB pmm.c unconditionally
 * reserves and never hands out - see pmm.c's LOW_MEMORY_LIMIT - so nothing
 * else in the kernel will ever collide with this scratch space, even
 * though by the time this runs (deep into kernel_main) it happens to
 * physically overlap where stage1/stage2 used to live; neither is read
 * again after boot handoff, so overwriting them here is harmless.
 *
 *   offset  0  (8 bytes)  cr3          - physical address to load into CR3
 *   offset  8  (8 bytes)  stack_top    - this AP's private kernel stack (RSP)
 *   offset 16  (8 bytes)  entry64      - address of ap_entry_asm_stub
 *   offset 24  (2 bytes)  gdt_limit  \_ the kernel's real GDT pointer
 *   offset 26  (8 bytes)  gdt_base   /  (gdt.c's gdt_get_table_ptr)
 *   offset 34  (4 bytes)  cpu_id       - this AP's index into smp_cpus[]/sched.c's per-CPU arrays
 *   offset 38  (4 bytes)  ap_ready     - 0 until ap_entry_asm_stub is done reading everything above
 */
#define AP_PARAMS_ADDR       0x7000ULL
#define AP_OFF_CR3           0
#define AP_OFF_STACK_TOP     8
#define AP_OFF_ENTRY64       16
#define AP_OFF_GDT_LIMIT     24
#define AP_OFF_GDT_BASE      26
#define AP_OFF_CPU_ID        34
#define AP_OFF_AP_READY      38
#define AP_PARAMS_SIZE       42

#define AP_TRAMPOLINE_LOAD_ADDR 0x8000ULL
/* SIPI's vector operand IS the target physical address divided by 4 KiB -
 * see the Intel SDM's description of the STARTUP IPI - so
 * AP_TRAMPOLINE_LOAD_ADDR has to stay 4 KiB-aligned for this to be exact. */
#define AP_TRAMPOLINE_VECTOR ((uint32_t)(AP_TRAMPOLINE_LOAD_ADDR >> 12))

extern const uint8_t ap_trampoline_start[];
extern const uint8_t ap_trampoline_end[]; /* kernel/proc/embed_ap_trampoline.asm */
extern void ap_entry_asm_stub(void);      /* kernel/arch/x86_64/ap_entry.asm */

static inline void ap_params_write64(uint64_t offset, uint64_t value) {
    *(volatile uint64_t *)(uintptr_t)(AP_PARAMS_ADDR + offset) = value;
}
static inline void ap_params_write32(uint64_t offset, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(AP_PARAMS_ADDR + offset) = value;
}
static inline void ap_params_write16(uint64_t offset, uint16_t value) {
    *(volatile uint16_t *)(uintptr_t)(AP_PARAMS_ADDR + offset) = value;
}
static inline uint32_t ap_params_read32(uint64_t offset) {
    return *(volatile uint32_t *)(uintptr_t)(AP_PARAMS_ADDR + offset);
}

/* ap_main: the C side of an AP's boot path - called by ap_entry_asm_stub
 * once this core has its own private stack and the kernel's real GDT
 * loaded. This is the same point BSP's kernel_main reaches right after
 * entry.asm's call into it, just for a second (or third, ...) core, and
 * it mirrors kernel_main's own init order for exactly the pieces a second
 * core needs: its own GDT/TSS slot, the shared IDT, its own Local APIC,
 * and a scheduler identity - then it becomes an idle loop that, like
 * kernel_main's own tail end, exists only to keep calling schedule() so
 * this core picks up whatever READY task is waiting. */
void ap_main(uint32_t cpu_id) {
    gdt_init_ap((int)cpu_id);
    idt_load_ap();
    lapic_init_this_cpu();
    sched_init_ap((int)cpu_id);

    smp_cpus[cpu_id].online = 1;

    klog_puts("[smp] AP online: cpu_id=");
    klog_put_hex32(cpu_id);
    klog_puts(" apic_id=");
    klog_put_hex32(smp_cpus[cpu_id].apic_id);
    klog_putc('\n');

    __asm__ volatile("sti");
    for (;;) {
        schedule();
        __asm__ volatile("hlt");
    }
}

int smp_current_cpu(void) {
    uint32_t id = lapic_id();
    for (int i = 0; i < MAX_CPUS; i++) {
        if (smp_cpus[i].online && smp_cpus[i].apic_id == id) {
            return i;
        }
    }
    return 0;
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

void smp_halt_other_cpus(void) {
    if (!initialized) {
        return;
    }
    lapic_send_ipi_all_excl_self(LAPIC_ICR_DELIVERY_NMI);
}

/* Starts exactly one AP and waits (bounded) for it to check in before
 * returning - APs are brought up strictly one at a time because
 * AP_PARAMS_ADDR is a single shared scratch struct; starting a second one
 * before the first has finished reading it would be a real race. Mirrors
 * the project's existing "bounded wait, diagnosable timeout message"
 * pattern (M6's keyboard/M18's mouse self-tests) rather than hanging boot
 * forever on a CPU that never shows up. */
static int start_ap(int cpu_id, uint32_t apic_id) {
    uint8_t *stack = (uint8_t *)kmalloc(AP_STACK_SIZE);
    if (!stack) {
        panic("smp: out of heap memory for an AP stack");
    }

    uint16_t gdt_limit;
    uint64_t gdt_base;
    gdt_get_table_ptr(&gdt_limit, &gdt_base);

    ap_params_write32(AP_OFF_AP_READY, 0);
    ap_params_write64(AP_OFF_CR3, vmm_kernel_pml4_phys());
    ap_params_write64(AP_OFF_STACK_TOP, (uint64_t)(uintptr_t)(stack + AP_STACK_SIZE));
    ap_params_write64(AP_OFF_ENTRY64, (uint64_t)(uintptr_t)ap_entry_asm_stub);
    ap_params_write16(AP_OFF_GDT_LIMIT, gdt_limit);
    ap_params_write64(AP_OFF_GDT_BASE, gdt_base);
    ap_params_write32(AP_OFF_CPU_ID, (uint32_t)cpu_id);

    /* AP_TRAMPOLINE_LOAD_ADDR is <1 MiB, always identity-mapped - see
     * vmm.c's phys_to_table for why every low-memory address is safe to
     * touch directly as a pointer under the kernel's own page tables. */
    uint8_t *dst = (uint8_t *)(uintptr_t)AP_TRAMPOLINE_LOAD_ADDR;
    uint64_t blob_len = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
    for (uint64_t i = 0; i < blob_len; i++) {
        dst[i] = ap_trampoline_start[i];
    }

    /* Standard INIT-SIPI-SIPI (Intel MP spec / SDM Vol 3A 8.4.4): INIT,
     * a real-hardware-safe delay, then SIPI twice (some CPUs boot off the
     * first, some need the second - sending it unconditionally is
     * harmless either way, since by then the AP has already left the
     * halted-after-INIT state and just ignores a redundant SIPI). The
     * spec calls for ~10ms after INIT and ~200us between the two SIPIs;
     * pit_sleep_ms's coarsest unit is one 10ms tick, so the SIPI gap ends
     * up slower than spec, not tighter - safe, just not minimal, the same
     * "correct, not maximally efficient" tradeoff as PIT_HZ's own 10ms
     * granularity everywhere else in this kernel. */
    lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_INIT | LAPIC_ICR_LEVEL_ASSERT);
    pit_sleep_ms(10);
    lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_STARTUP | AP_TRAMPOLINE_VECTOR);
    pit_sleep_ms(10);
    lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_STARTUP | AP_TRAMPOLINE_VECTOR);

    uint64_t deadline = pit_get_ticks() + PIT_HZ; /* 1s */
    while (ap_params_read32(AP_OFF_AP_READY) == 0 && pit_get_ticks() < deadline) {
        __asm__ volatile("pause");
    }
    return ap_params_read32(AP_OFF_AP_READY) != 0;
}

void smp_init(void) {
    acpi_madt_info_t madt;
    if (!acpi_find_madt(&madt)) {
        smp_cpus[0].apic_id = 0;
        smp_cpus[0].online = 1;
        initialized = 1;
        klog_puts("[smp] running single-core.\n");
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
            continue; /* that's the BSP itself, already cpu 0 */
        }
        if (next_cpu >= MAX_CPUS) {
            klog_puts("[smp] MAX_CPUS reached - ignoring the remaining CPU(s) listed in the MADT.\n");
            break;
        }
        smp_cpus[next_cpu].apic_id = madt.cpu_apic_ids[i];
        klog_puts("[smp] starting AP apic_id=0x");
        klog_put_hex32(madt.cpu_apic_ids[i]);
        klog_puts("...\n");
        if (start_ap(next_cpu, madt.cpu_apic_ids[i])) {
            smp_cpu_count = next_cpu + 1;
            next_cpu++;
        } else {
            klog_puts("[smp] AP did not check in within the timeout - skipping it.\n");
        }
    }

    initialized = 1;
    klog_puts("[smp] ");
    klog_put_hex32((uint32_t)smp_cpu_count);
    klog_puts(" CPU(s) online.\n");
}

/* EOI goes out *before* dispatching, not after - the exact same bug M7's
 * progress log already caught once for the PIC (pic_send_eoi) and fixed
 * the same way: scheduler_tick_cpu can call schedule(), which can
 * context_switch this CPU away entirely, and that switch doesn't "return"
 * in the normal sense - it only unwinds back through here much later,
 * whenever this exact interrupted context is next resumed (which itself
 * requires *another* interrupt on this CPU to make happen - circular, if
 * that next interrupt is exactly the one EOI is withholding). Sending EOI
 * first means the Local APIC stops withholding IPI_SCHEDULE_VECTOR (and
 * everything else at its priority) on this CPU regardless of whether or
 * when this call ever "returns" - found by testing this exact scenario
 * hanging every AP after its first real reschedule, not anticipated in
 * advance, the same way M7's original PIC version of this bug was. */
void lapic_vector_handler(isr_regs_t *regs) {
    lapic_send_eoi();
    if (regs->vector == IPI_SCHEDULE_VECTOR) {
        scheduler_tick_cpu(smp_current_cpu());
    }
}
