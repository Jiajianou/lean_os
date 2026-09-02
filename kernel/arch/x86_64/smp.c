#include "smp.h"

#include <stdint.h>

#include "acpi/acpi.h"
#include "drivers/klog.h"
#include "drivers/pit.h"
#include "fpu.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "lapic.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "panic.h"
#include "profile/sampler.h"
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
 * else in the kernel will ever collide with this scratch space.
 *
 *   offset  0  (8 bytes)  cr3          - physical address to load into CR3
 *   offset  8  (8 bytes)  stack_top    - this AP's private kernel stack (RSP)
 *   offset 16  (8 bytes)  entry64      - address of ap_entry_asm_stub
 *   offset 24  (2 bytes)  gdt_limit  \_ the kernel's real GDT pointer
 *   offset 26  (8 bytes)  gdt_base   /  (gdt.c's gdt_get_table_ptr)
 *   offset 34  (4 bytes)  cpu_id       - this AP's index into smp_cpus[]/sched.c's per-CPU arrays
 *   offset 38  (4 bytes)  ap_ready     - 0 until ap_entry_asm_stub is done reading everything above
 *   offset 42  (1 byte)   nx           - M106: nonzero if the BSP enabled NX, so the AP can set EFER.NXE BEFORE it turns on paging with tables that carry bit 63
 */
#define AP_PARAMS_ADDR       0x7000ULL
#define AP_OFF_CR3           0
#define AP_OFF_STACK_TOP     8
#define AP_OFF_ENTRY64       16
#define AP_OFF_GDT_LIMIT     24
#define AP_OFF_GDT_BASE      26
#define AP_OFF_CPU_ID        34
#define AP_OFF_AP_READY      38
#define AP_OFF_NX            42
#define AP_PARAMS_SIZE       43

#define AP_TRAMPOLINE_LOAD_ADDR 0x8000ULL
/* SIPI's vector operand IS the target physical address divided by 4 KiB -
 * see the Intel SDM's description of the STARTUP IPI - so
 * AP_TRAMPOLINE_LOAD_ADDR has to stay 4 KiB-aligned for this to be exact. */
#define AP_TRAMPOLINE_VECTOR ((uint32_t)(AP_TRAMPOLINE_LOAD_ADDR >> 12))

extern const uint8_t ap_trampoline_start[];
extern const uint8_t ap_trampoline_end[]; /* kernel/proc/embed_ap_trampoline.asm */
extern void ap_entry_asm_stub(void);      /* kernel/arch/x86_64/ap_entry.asm */

static void verify_cpu_identity(uint32_t cpu_id); /* M106 - defined below */

static inline void ap_params_write64(uint64_t offset, uint64_t value) {
    *(volatile uint64_t *)(uintptr_t)(AP_PARAMS_ADDR + offset) = value;
}
static inline void ap_params_write32(uint64_t offset, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(AP_PARAMS_ADDR + offset) = value;
}
static inline void ap_params_write16(uint64_t offset, uint16_t value) {
    *(volatile uint16_t *)(uintptr_t)(AP_PARAMS_ADDR + offset) = value;
}
static inline void ap_params_write8(uint64_t offset, uint8_t value) {
    *(volatile uint8_t *)(uintptr_t)(AP_PARAMS_ADDR + offset) = value;
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
    /* M63: CR0/CR4 are per-CPU, so every core has to be told that this
     * OS uses FXSAVE - an AP that missed this would fault the first time
     * a task doing floating-point work was scheduled onto it, which is a
     * bug that shows up as "sometimes". */
    fpu_init_cpu();
    /* M91: EFER.NXE, for exactly the reason the FXSAVE line above gives.
     * EFER is per-CPU and the page tables carrying PTE_NX are not, so an
     * AP that skipped this would take a reserved-bit page fault the first
     * time it touched a user page - which is the same "shows up as
     * sometimes" bug, on a core that was fine until a task migrated to
     * it. */
    vmm_enable_nx_this_cpu();
    lapic_init_this_cpu();
    verify_cpu_identity(cpu_id); /* M106 - before anything is indexed by it */
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
    /* M106: `str`, not a LAPIC read and a table scan. See
     * gdt.h's gdt_current_cpu for the whole argument - the short version
     * is that the old one returned 0 for a core it did not recognise,
     * which aliases that core's current_task, its TSS rsp0 and its slice
     * counter onto the boot CPU's, and that is a corruption with no
     * message attached to it.
     *
     * The two answers are cross-checked once per boot, from ap_main, so
     * that "the TSS selector and the MADT agree about who this is" is a
     * fact this kernel has established rather than one it assumes. */
    return gdt_current_cpu();
}

/* M106: the check that makes replacing the lookup honest. Run once per
 * core, at bring-up, while both answers are still available. */
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
        klog_puts("[smp] this core was started as cpu ");
        klog_put_dec(cpu_id);
        klog_puts(" but its task register says ");
        klog_put_dec((uint32_t)by_tss);
        klog_puts(" and its LAPIC id 0x");
        klog_put_hex32(id);
        klog_puts(" maps to ");
        klog_put_dec((uint32_t)by_lapic);
        klog_putc('\n');
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
    /* M106: read by the trampoline before it sets CR0.PG. ap_main's
     * vmm_enable_nx_this_cpu stays - it is still what keeps EFER.NXE set
     * once C is running - but it can only run on a core that got here,
     * and without this byte no core ever did. */
    ap_params_write8(AP_OFF_NX, vmm_nx_enabled() ? 1u : 0u);

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
        /* M106: this core's share of the SAMPLE, and it was missing.
         *
         * M101 put profile_sample in pit.c, which runs on the BSP alone -
         * the 8259 delivers IRQ 0 to one CPU. So the profiler sampled one
         * core out of however many, and what it reported was that core's
         * view of the machine. On four cores the kernel busy loop in
         * M101's own self-test migrated off the BSP and every sample the
         * BSP took was of an idle task: samples counted, histogram empty,
         * and the self-test panicked - which is the right outcome and the
         * first time it could ever have happened, because every harness
         * in this project ran -smp 1 until M106.
         *
         * Here rather than in a LAPIC timer of its own: this IPI is
         * already the per-CPU tick, broadcast by the BSP's PIT, so it
         * arrives at PIT_HZ on every core and needs no second timer to
         * be programmed and calibrated. Ahead of the scheduler hook for
         * exactly the reason pit.c gives - a tick that ends in a context
         * switch never comes back to this function, so anything that must
         * see THIS tick's interrupted RIP has to read it first. */
        profile_sample(regs);
        /* M88: this core's share of the tick, charged before the hook
         * that may switch away from the task it belongs to. The BSP's
         * copy of this is in pit.c, next to profile_sample and for the
         * same reason. */
        sched_account_tick((regs->cs & 3) != 0);
        scheduler_tick_cpu(smp_current_cpu());
    }
}
