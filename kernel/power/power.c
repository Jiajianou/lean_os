#include "power.h"

#include "acpi/acpi.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/smp.h"
#include "drivers/klog.h"
#include "drivers/pit.h"
#include "fs/vfs.h"
#include "sched/sched.h"
#include "signal.h"

static acpi_power_info_t power_info;
static int power_info_valid;

static const uint16_t S5_SLEEP_TYPES[] = {0, 5};
#define S5_SLEEP_TYPE_COUNT ((int)(sizeof(S5_SLEEP_TYPES) / sizeof(S5_SLEEP_TYPES[0])))

static int s5_from_aml;
static uint8_t s5_slp_a, s5_slp_b;

#define PM1_SLP_TYP_SHIFT 10
#define PM1_SLP_EN        (1u << 13)
#define PM1_SCI_EN        1

#define QEMU_PM1A_CNT      0x604
#define BOCHS_PM1A_CNT     0xB004

#define KBD_STATUS_PORT  0x64
#define KBD_CMD_RESET    0xFE
#define KBD_INPUT_FULL   2

void power_init(void) {
    power_info_valid = acpi_find_power(&power_info);
    s5_from_aml = acpi_find_s5(&s5_slp_a, &s5_slp_b);
}

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

static void power_off_now(void) {
    acpi_enable_if_needed();

    if (power_info_valid && power_info.pm1a_cnt != 0 && s5_from_aml) {
        klog_puts("[power] S5 via the FADT's PM1a_CNT (port 0x");
        klog_put_hex32(power_info.pm1a_cnt);
        klog_puts(", SLP_TYP ");
        klog_put_hex32(s5_slp_a);
        klog_puts(" - read from the DSDT's own AML).\n");
        write_sleep((uint16_t)power_info.pm1a_cnt, s5_slp_a);
        if (power_info.pm1b_cnt != 0) {
            write_sleep((uint16_t)power_info.pm1b_cnt, s5_slp_b);
        }
        pit_sleep_ms(50);
    }

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
    }
    outb(KBD_STATUS_PORT, KBD_CMD_RESET);
    pit_sleep_ms(50);

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

    for (int i = 0; i < total; i++) {
        task_t *t = sched_task_by_slot(i);
        if (!t || t == self || t->parent_id < 0 || t->state == TASK_TERMINATED) {
            continue;
        }
        t->pending_signal = SIGTERM;
    }

    uint64_t deadline = pit_get_ticks() + grace_ticks;
    for (;;) {
        int alive = 0;
        for (int i = 0; i < total; i++) {
            task_t *t = sched_task_by_slot(i);
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
        task_t *t = sched_task_by_slot(i);
        if (!t || t == self || t->parent_id < 0 || t->state == TASK_TERMINATED) {
            continue;
        }
        t->pending_signal = SIGKILL;
        killed++;
    }

    uint64_t kill_deadline = pit_get_ticks() + 100;
    while (pit_get_ticks() < kill_deadline) {
        int alive = 0;
        for (int i = 0; i < total; i++) {
            task_t *t = sched_task_by_slot(i);
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

    int killed = power_orderly_stop(100);
    klog_puts("[power] orderly stop complete (0x");
    klog_put_hex32((uint32_t)killed);
    klog_puts(" task(s) needed SIGKILL after the grace period).\n");

    vfs_sync();

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
