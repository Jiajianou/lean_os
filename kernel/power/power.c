#include "power.h"

#include "acpi/acpi.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/smp.h"
#include "drivers/klog.h"
#include "drivers/pit.h"
#include "fs/vfs.h"
#include "sched/sched.h"
#include "signal.h" /* system_api/include/signal.h */

static acpi_power_info_t power_info;
static int power_info_valid;

/* The sleep type S5 ("soft off") wants written into PM1_CNT's SLP_TYP
 * field. Properly this comes from the `\_S5` object in the DSDT, which is
 * AML - and writing an AML parser is not in scope for this project (see
 * acpi.h's own note). So these are the well-known values instead, tried
 * in order: 0 is what QEMU's own tables define and is by far the most
 * likely to be right on the one platform this OS is actually tested on,
 * and 5 is the value most physical chipsets historically used. Writing a
 * wrong SLP_TYP is harmless - the machine simply doesn't sleep - so
 * trying both costs nothing but two port writes. */
static const uint16_t S5_SLEEP_TYPES[] = {0, 5};
#define S5_SLEEP_TYPE_COUNT ((int)(sizeof(S5_SLEEP_TYPES) / sizeof(S5_SLEEP_TYPES[0])))

#define PM1_SLP_TYP_SHIFT 10
#define PM1_SLP_EN        (1u << 13)
#define PM1_SCI_EN        1

/* QEMU's documented ACPI PM base. The last-resort tier: if the FADT was
 * unreadable (or its PM1a_CNT was 0) this is still the right answer on
 * the emulator this project is developed against, and being explicit
 * about that in the log is the point - a machine that powers off this way
 * has not proved anything about its own ACPI tables. 0xB004 is the older
 * Bochs/QEMU base, kept because it costs one more outw. */
#define QEMU_PM1A_CNT      0x604
#define BOCHS_PM1A_CNT     0xB004

#define KBD_STATUS_PORT  0x64
#define KBD_CMD_RESET    0xFE
#define KBD_INPUT_FULL   2

void power_init(void) {
    power_info_valid = acpi_find_power(&power_info);
}

/* Hands the PM registers from firmware to the OS, if this platform says
 * that handshake is needed at all (SMI_CMD == 0 means it isn't, which is
 * the normal case under UEFI - the firmware has already done it). Bounded
 * rather than an unbounded poll: a chipset that never sets SCI_EN would
 * otherwise hang the shutdown path, which is the one path that must not
 * hang. */
static void acpi_enable_if_needed(void) {
    if (!power_info_valid || power_info.smi_cmd == 0 || power_info.pm1a_cnt == 0) {
        return;
    }
    if (inw((uint16_t)power_info.pm1a_cnt) & PM1_SCI_EN) {
        return;
    }
    outb((uint16_t)power_info.smi_cmd, power_info.acpi_enable);
    for (int i = 0; i < 300; i++) {
        if (inw((uint16_t)power_info.pm1a_cnt) & PM1_SCI_EN) {
            return;
        }
        pit_sleep_ms(1);
    }
    klog_puts("[power] the ACPI-enable handshake never set SCI_EN - trying S5 anyway.\n");
}

static void write_sleep(uint16_t port, uint16_t slp_typ) {
    outw(port, (uint16_t)((slp_typ << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN));
}

/* Every tier, in order, each announcing itself before it fires - so the
 * last line in the log is always the mechanism that was actually tried
 * last, whether or not it worked. Returns only if the machine is still
 * on afterwards, which for a successful S5 it never does. */
static void power_off_now(void) {
    acpi_enable_if_needed();

    if (power_info_valid && power_info.pm1a_cnt != 0) {
        for (int i = 0; i < S5_SLEEP_TYPE_COUNT; i++) {
            klog_puts("[power] S5 via the FADT's PM1a_CNT (port 0x");
            klog_put_hex32(power_info.pm1a_cnt);
            klog_puts(", SLP_TYP ");
            klog_put_hex32(S5_SLEEP_TYPES[i]);
            klog_puts(" - a well-known value, not read from AML).\n");
            write_sleep((uint16_t)power_info.pm1a_cnt, S5_SLEEP_TYPES[i]);
            if (power_info.pm1b_cnt != 0) {
                write_sleep((uint16_t)power_info.pm1b_cnt, S5_SLEEP_TYPES[i]);
            }
            pit_sleep_ms(50);
        }
    }

    klog_puts("[power] the FADT path did not power the machine off - falling back to "
              "QEMU's documented PM base (0x604), then Bochs's (0xB004).\n");
    write_sleep(QEMU_PM1A_CNT, 0);
    pit_sleep_ms(50);
    write_sleep(BOCHS_PM1A_CNT, 0);
    pit_sleep_ms(50);
}

/* Reboot in three tiers, worst case a deliberate triple fault - which is
 * not elegant, but a machine that will not restart at all is worse than
 * one that restarts inelegantly. */
static void reboot_now(void) {
    if (power_info_valid && power_info.reset_port != 0) {
        klog_puts("[power] reset via the FADT's reset register (port 0x");
        klog_put_hex32(power_info.reset_port);
        klog_puts(").\n");
        outb((uint16_t)power_info.reset_port, power_info.reset_value);
        pit_sleep_ms(50);
    }

    klog_puts("[power] falling back to the 8042 reset pulse (0xFE to port 0x64).\n");
    for (int i = 0; i < 1000 && (inb(KBD_STATUS_PORT) & KBD_INPUT_FULL); i++) {
        /* Drain the controller's input buffer first - it ignores a
         * command written while one is still pending. Bounded, for the
         * same reason acpi_enable_if_needed's poll is. */
    }
    outb(KBD_STATUS_PORT, KBD_CMD_RESET);
    pit_sleep_ms(50);

    /* Triple fault: load a zero-length IDT so the next interrupt has no
     * handler, then raise one. The CPU faults, fails to deliver the
     * fault handler, fails again handling *that*, and resets - the
     * classic last-resort reboot. */
    klog_puts("[power] falling back to a triple fault.\n");
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } null_idt = {0, 0};
    __asm__ volatile("lidt %0" : : "m"(null_idt));
    __asm__ volatile("int $3");
}

int power_orderly_stop(uint64_t grace_ticks) {
    task_t *self = sched_current();
    int total = sched_task_count();

    /* Task 0 is the boot/idle task and the AP idle identities
     * (sched_init_ap) are the same thing on other cores - neither is a
     * program anyone asked to run, and killing them is how a machine
     * stops being able to reach its own shutdown code. They are told
     * apart the only way this scheduler can: they have no parent. */
    for (int i = 0; i < total; i++) {
        task_t *t = sched_task_by_id(i);
        if (!t || t == self || t->parent_id < 0 || t->state == TASK_TERMINATED) {
            continue;
        }
        t->pending_signal = SIGTERM;
    }

    uint64_t deadline = pit_get_ticks() + grace_ticks;
    for (;;) {
        int alive = 0;
        for (int i = 0; i < total; i++) {
            task_t *t = sched_task_by_id(i);
            if (t && t != self && t->parent_id >= 0 && t->state != TASK_TERMINATED) {
                alive++;
            }
        }
        if (alive == 0 || pit_get_ticks() >= deadline) {
            break;
        }
        schedule();
    }

    int killed = 0;
    for (int i = 0; i < total; i++) {
        task_t *t = sched_task_by_id(i);
        if (!t || t == self || t->parent_id < 0 || t->state == TASK_TERMINATED) {
            continue;
        }
        t->pending_signal = SIGKILL;
        killed++;
    }

    /* A SIGKILL is noticed at the target's next syscall or scheduler tick
     * (sched.c), so this has to wait for the deaths themselves rather
     * than for the signals to have been posted - the same distinction
     * kernel.c's selftest_reap exists for. Bounded again: a task that
     * somehow cannot be killed must not be able to prevent the machine
     * from powering off. */
    uint64_t kill_deadline = pit_get_ticks() + 100; /* 100 ticks = ~1s */
    while (pit_get_ticks() < kill_deadline) {
        int alive = 0;
        for (int i = 0; i < total; i++) {
            task_t *t = sched_task_by_id(i);
            if (t && t != self && t->parent_id >= 0 && t->state != TASK_TERMINATED) {
                alive++;
            }
        }
        if (alive == 0) {
            break;
        }
        schedule();
    }
    return killed;
}

void power_shutdown(int mode) {
    klog_puts(mode == POWER_REBOOT ? "[power] restarting.\n" : "[power] shutting down.\n");

    /* ~1 second, counted in real PIT ticks rather than as a spin - a
     * grace period measured in loop iterations would mean something
     * different on every host. */
    int killed = power_orderly_stop(100);
    klog_puts("[power] orderly stop complete (0x");
    klog_put_hex32((uint32_t)killed);
    klog_puts(" task(s) needed SIGKILL after the grace period).\n");

    vfs_sync();

    /* Other cores are still running whatever they were running. The
     * sleep/reset write below stops the whole machine either way, but
     * halting them first means nothing is touching the disk or the
     * framebuffer while this finishes. No-op on a single-core boot. */
    smp_halt_other_cpus();

    if (mode == POWER_REBOOT) {
        reboot_now();
    } else {
        power_off_now();
    }

    klog_puts("[power] every tier failed - the machine is still on. Halting.\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
