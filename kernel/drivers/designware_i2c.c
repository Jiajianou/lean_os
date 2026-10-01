#include "designware_i2c.h"

#include "drivers/designware_i2c_timing.h"
#include "drivers/kernel_log.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "memory_management/virtual_memory.h"
#ifndef LEANOS_HOST_TEST
#include "scheduler/scheduler.h"
#include "architecture/x86_64/io.h"
#include "architecture/x86_64/timestamp_counter.h"
#endif

#define REGISTER_CONTROL        0x00
#define REGISTER_TARGET_ADDRESS 0x04
#define REGISTER_DATA_COMMAND   0x10
#define REGISTER_FS_SCL_HIGH    0x1C
#define REGISTER_FS_SCL_LOW     0x20
#define REGISTER_INTERRUPT_MASK 0x30
#define REGISTER_RAW_INTERRUPT  0x34
#define REGISTER_RX_THRESHOLD   0x38
#define REGISTER_TX_THRESHOLD   0x3C
#define REGISTER_CLEAR_INTERRUPT 0x40
#define REGISTER_CLEAR_TX_ABORT  0x54
#define REGISTER_ENABLE         0x6C
#define REGISTER_STATUS         0x70
#define REGISTER_TX_LEVEL       0x74
#define REGISTER_RX_LEVEL       0x78
#define REGISTER_TX_ABORT_SOURCE 0x80
#define REGISTER_ENABLE_STATUS  0x9C
#define REGISTER_COMPONENT_PARAM 0xF4
#define REGISTER_COMPONENT_TYPE 0xFC

#define CONTROL_MASTER_MODE   (1u << 0)
#define CONTROL_SPEED_FAST    (2u << 1)
#define CONTROL_RESTART_ENABLE (1u << 5)
#define CONTROL_SLAVE_DISABLE (1u << 6)

#define STATUS_ACTIVITY          (1u << 0)
#define STATUS_TX_FIFO_NOT_FULL  (1u << 1)
#define STATUS_RX_FIFO_NOT_EMPTY (1u << 3)

#define DATA_COMMAND_READ    (1u << 8)
#define DATA_COMMAND_STOP    (1u << 9)
#define DATA_COMMAND_RESTART (1u << 10)

#define RAW_INTERRUPT_TX_ABORT (1u << 6)
#define RAW_INTERRUPT_STOP     (1u << 9)

#define INTEL_VENDOR_ID 0x8086

#define LPSS_PRIVATE_RESETS 0x204
#define LPSS_RESET_RELEASED 0x7u

#define ASSUMED_CLOCK_KHZ 216000u

#define POLL_LIMIT 50000

/* Fast mode clocks nine bits a byte at 400 kHz: 22.5 us. A millisecond is
   the shortest sleep the scheduler offers, so a transfer only gives the
   processor up while at least that much is still on the wire. */
#define BYTES_PER_MILLISECOND 44u

typedef struct {
    volatile uint8_t *base;
    uint32_t transmit_depth;
    uint32_t receive_depth;
} controller_t;

static controller_t controllers[DESIGNWARE_I2C_MAX_CONTROLLERS];
static int controller_count;
static int sleep_while_clocking;

/* M205. A transfer used to spin on the status register for every byte, and
   the touchpad is read forty times a second with nobody touching it - a
   tenth of a core on the laptop, all of it waiting for a 400 kHz bus. Only
   the poller opts in: the probe runs at boot, before there is a scheduler
   to give the processor to. */
void designware_i2c_sleep_while_clocking(int enabled) {
    sleep_while_clocking = enabled;
}

static void wait_for_the_bus(uint32_t bytes_on_the_wire) {
#ifdef LEANOS_HOST_TEST
    (void)bytes_on_the_wire;
#else
    if (sleep_while_clocking && bytes_on_the_wire >= BYTES_PER_MILLISECOND) {
        scheduler_sleep_ms(1);
    }
#endif
}

#ifdef LEANOS_HOST_TEST
/* A DesignWare register is not memory: reading the data register pops the
   receive queue and reading the abort source clears it. The host tests hand
   those two functions a model of the controller, which is the only way this
   file's transfer loop is ever executed off the machine it was written for. */
uint32_t designware_i2c_host_read(uint64_t base, uint32_t offset);
void designware_i2c_host_write(uint64_t base, uint32_t offset, uint32_t value);

static uint32_t read_register(const controller_t *c, uint32_t offset) {
    return designware_i2c_host_read((uint64_t)(uintptr_t)c->base, offset);
}

static void write_register(const controller_t *c, uint32_t offset, uint32_t value) {
    designware_i2c_host_write((uint64_t)(uintptr_t)c->base, offset, value);
}
#else
static uint32_t read_register(const controller_t *c, uint32_t offset) {
    return *(volatile uint32_t *)(c->base + offset);
}

static void write_register(const controller_t *c, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(c->base + offset) = value;
}
#endif

static int disable_controller(const controller_t *c) {
    write_register(c, REGISTER_ENABLE, 0);
    for (int spins = 0; spins < POLL_LIMIT; spins++) {
        if (!(read_register(c, REGISTER_ENABLE_STATUS) & 1u)) {
            return 1;
        }
    }
    return 0;
}

static void configure(const controller_t *c) {
    uint16_t high = 0;
    uint16_t low = 0;
    designware_i2c_counts(ASSUMED_CLOCK_KHZ, &high, &low);

    write_register(c, REGISTER_CONTROL,
                   CONTROL_MASTER_MODE | CONTROL_SPEED_FAST | CONTROL_RESTART_ENABLE |
                       CONTROL_SLAVE_DISABLE);
    write_register(c, REGISTER_FS_SCL_HIGH, high);
    write_register(c, REGISTER_FS_SCL_LOW, low);
    write_register(c, REGISTER_INTERRUPT_MASK, 0);
    write_register(c, REGISTER_RX_THRESHOLD, 0);
    write_register(c, REGISTER_TX_THRESHOLD, 0);
}

int designware_i2c_transfer(int controller, uint8_t address, const uint8_t *write,
                            uint32_t write_length, uint8_t *read, uint32_t read_length) {
    if (controller < 0 || controller >= controller_count) {
        return 0;
    }
    const controller_t *c = &controllers[controller];

    if (!disable_controller(c)) {
        return 0;
    }
    configure(c);
    write_register(c, REGISTER_TARGET_ADDRESS, address & 0x7Fu);
    write_register(c, REGISTER_ENABLE, 1);
    (void)read_register(c, REGISTER_CLEAR_INTERRUPT);

    uint32_t written = 0;
    uint32_t read_commands = 0;
    uint32_t received = 0;
    int spins = 0;

    while (written < write_length || read_commands < read_length || received < read_length) {
        if (read_register(c, REGISTER_RAW_INTERRUPT) & RAW_INTERRUPT_TX_ABORT) {
            (void)read_register(c, REGISTER_CLEAR_TX_ABORT);
            disable_controller(c);
            return 0;
        }

        while (received < read_commands && (read_register(c, REGISTER_RX_LEVEL) > 0)) {
            read[received++] = (uint8_t)(read_register(c, REGISTER_DATA_COMMAND) & 0xFFu);
            spins = 0;
        }

        if (read_register(c, REGISTER_STATUS) & STATUS_TX_FIFO_NOT_FULL) {
            if (written < write_length) {
                uint32_t command = write[written];
                if (written + 1 == write_length && read_length == 0) {
                    command |= DATA_COMMAND_STOP;
                }
                write_register(c, REGISTER_DATA_COMMAND, command);
                written++;
                spins = 0;
                continue;
            }
            /* Never more reads asked for than the receive queue holds: the
               controller clocks a byte for each, and while this side sleeps
               nobody drains it - an overrun loses the byte and says nothing. */
            if (read_commands < read_length && read_commands - received < c->receive_depth) {
                uint32_t command = DATA_COMMAND_READ;
                if (read_commands == 0 && write_length > 0) {
                    command |= DATA_COMMAND_RESTART;
                }
                if (read_commands + 1 == read_length) {
                    command |= DATA_COMMAND_STOP;
                }
                write_register(c, REGISTER_DATA_COMMAND, command);
                read_commands++;
                spins = 0;
                continue;
            }
        }

        /* Only once the last command - the one carrying STOP - is queued: a
           controller built without IC_EMPTYFIFO_HOLD_MASTER_EN ends the
           transaction itself when its transmit queue runs dry, and nothing
           in its registers says which kind this is. */
        uint32_t outstanding = read_commands - received;
        if (written == write_length && read_commands == read_length &&
            outstanding >= BYTES_PER_MILLISECOND && sleep_while_clocking) {
            wait_for_the_bus(outstanding);
            spins = 0;
            continue;
        }
        if (++spins > POLL_LIMIT) {
            disable_controller(c);
            return 0;
        }
    }

    for (spins = 0; spins < POLL_LIMIT; spins++) {
        if (!(read_register(c, REGISTER_STATUS) & STATUS_ACTIVITY)) {
            break;
        }
    }

    if (read_register(c, REGISTER_RAW_INTERRUPT) & RAW_INTERRUPT_TX_ABORT) {
        (void)read_register(c, REGISTER_CLEAR_TX_ABORT);
        disable_controller(c);
        return 0;
    }

    disable_controller(c);
    return 1;
}

int designware_i2c_controller_count(void) {
    return controller_count;
}

static uint64_t physical_address_limit(void) {
#ifdef LEANOS_HOST_TEST
    return 1ULL << 39;
#else
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000u));
    if (eax < 0x80000008u) {
        return 1ULL << 36;
    }
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000008u));
    return 1ULL << (eax & 0xFF);
#endif
}

int designware_i2c_init(void) {
    controller_count = 0;

    for (uint32_t index = 0; index < 16 && controller_count < DESIGNWARE_I2C_MAX_CONTROLLERS; index++) {
        pci_device_t device;
        if (!pci_find_class(0x0C, 0x80, PCI_PROG_IF_ANY, index, &device)) {
            break;
        }

        uint64_t base = pci_bar_memory_base(&device, 0);
        uint64_t size = pci_bar_memory_size(&device, 0);
        kernel_log_puts("[i2c] class 0C80 at ");
        kernel_log_put_hex32(device.vendor_id);
        kernel_log_puts(":");
        kernel_log_put_hex32(device.device_id);
        kernel_log_puts(" BAR0 ");
        kernel_log_put_hex64(base);
        kernel_log_puts(" size ");
        kernel_log_put_hex64(size);
        if (base == 0 && size >= 0x1000) {
            /* Tiger Lake firmware leaves the LPSS I2C controllers unplaced and
               expects the operating system to assign them, which Linux does. */
#ifndef LEANOS_HOST_TEST
            /* Sizing every other device's BARs briefly moves them; nothing may
               touch a device - the xHCI poll runs from the timer - meanwhile. */
            uint64_t flags = irq_save_disable();
            base = pci_assign_memory_bar(&device, 0, physical_address_limit());
            irq_restore(flags);
#else
            base = pci_assign_memory_bar(&device, 0, physical_address_limit());
#endif
            kernel_log_puts(base ? " - unassigned, placed at " : " - unassigned, and no room to place it");
            if (base) {
                kernel_log_put_hex64(base);
            }
        }
        if (base == 0 || size < 0x1000) {
            kernel_log_puts(" - no usable BAR, skipped.\n");
            continue;
        }

        pci_set_power_state_d0(&device);
#ifndef LEANOS_HOST_TEST
        /* PCI allows a function 10 ms to come out of D3hot, and real silicon takes it. */
        uint64_t settle_start = tsc_read();
        while (tsc_to_us(tsc_read() - settle_start) < 10000) {
            __asm__ volatile("pause");
        }
#endif
        pci_enable_device(&device);

        volatile uint8_t *registers = (volatile uint8_t *)base;
#ifndef LEANOS_HOST_TEST
        registers = (volatile uint8_t *)virtual_memory_map_mmio(base, size < 0x4000 ? size : 0x4000);
        if (!registers) {
            continue;
        }
#endif

        controller_t candidate;
        candidate.base = registers;
        candidate.transmit_depth = 0;
        candidate.receive_depth = 1;

        uint32_t component = read_register(&candidate, REGISTER_COMPONENT_TYPE);
        kernel_log_puts(", component ");
        kernel_log_put_hex32(component);
        if (component != DESIGNWARE_I2C_COMPONENT_TYPE && device.vendor_id == INTEL_VENDOR_ID &&
            size > LPSS_PRIVATE_RESETS) {
            /* Intel wraps the Synopsys block in a private register space whose reset is
               held until somebody releases it - a controller still in reset identifies
               as nothing at all. Nobody else's class-0C80 device gets written to. */
            write_register(&candidate, LPSS_PRIVATE_RESETS, 0);
            write_register(&candidate, LPSS_PRIVATE_RESETS, LPSS_RESET_RELEASED);
            component = read_register(&candidate, REGISTER_COMPONENT_TYPE);
            kernel_log_puts(", after releasing reset ");
            kernel_log_put_hex32(component);
        }
        if (component != DESIGNWARE_I2C_COMPONENT_TYPE) {
            kernel_log_puts(" - not a DesignWare I2C block.\n");
            continue;
        }
        kernel_log_puts(".\n");

        uint32_t parameters = read_register(&candidate, REGISTER_COMPONENT_PARAM);
        candidate.transmit_depth = ((parameters >> 16) & 0xFFu) + 1u;
        candidate.receive_depth = ((parameters >> 8) & 0xFFu) + 1u;

        controllers[controller_count++] = candidate;

        kernel_log_puts("[i2c] DesignWare master at ");
        kernel_log_put_hex64(base);
        kernel_log_puts(" (");
        kernel_log_put_hex32(device.vendor_id);
        kernel_log_puts(":");
        kernel_log_put_hex32(device.device_id);
        kernel_log_puts("), transmit fifo ");
        kernel_log_put_dec(candidate.transmit_depth);
        kernel_log_puts(" deep.\n");
    }

    if (controller_count == 0) {
        kernel_log_puts("[i2c] no DesignWare I2C master on this machine - nothing to probe.\n");
    }

    return controller_count;
}
