#include "mouse.h"

#include "scheduler/scheduler.h"

#include "architecture/x86_64/io.h"
#include "architecture/x86_64/interrupt_service_routines.h"
#include "architecture/x86_64/pic.h"
#include "drivers/kernel_log.h"
#include "drivers/pit.h"

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT    0x64

#define PS2_STATUS_OUTPUT_FULL 0x01u
#define PS2_STATUS_INPUT_FULL  0x02u

#define MOUSE_IRQ    12
#define CASCADE_IRQ  2

#define PS2_POLL_LIMIT 100000

static void ps2_wait_input_clear(void) {
    for (int timeout = PS2_POLL_LIMIT; timeout > 0; timeout--) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) {
            return;
        }
    }
}

static void ps2_wait_output_full(void) {
    for (int timeout = PS2_POLL_LIMIT; timeout > 0; timeout--) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            return;
        }
    }
}

static void ps2_write_command(uint8_t command) {
    ps2_wait_input_clear();
    outb(PS2_COMMAND_PORT, command);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_input_clear();
    outb(PS2_DATA_PORT, data);
}

static uint8_t ps2_read_data(void) {
    ps2_wait_output_full();
    return inb(PS2_DATA_PORT);
}

static void ps2_flush_output_buffer(void) {
    for (int i = 0; i < 16; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) {
            return;
        }
        (void)inb(PS2_DATA_PORT);
    }
}

static void mouse_write(uint8_t data) {
    ps2_write_command(0xD4);
    ps2_write_data(data);
    ps2_read_data();
}

#define EVENT_BUFFER_SIZE 64
static mouse_event_t event_buffer[EVENT_BUFFER_SIZE];
static volatile uint32_t buffer_head;
static volatile uint32_t buffer_tail;

static int packet_bytes = 3;

static uint8_t packet[4];
static int packet_index;

static void push_event(mouse_event_t ev) {
    uint32_t next = (buffer_head + 1) % EVENT_BUFFER_SIZE;
    if (next == buffer_tail) {
        return;
    }
    event_buffer[buffer_head] = ev;
    buffer_head = next;
    scheduler_wake_all(SCHEDULER_POLL_CHAN);
}

static void mouse_irq(isr_regs_t *regs) {
    (void)regs;
    uint8_t data = inb(PS2_DATA_PORT);

    if (packet_index == 0 && !(data & 0x08)) {
        return;
    }

    packet[packet_index++] = data;
    if (packet_index < packet_bytes) {
        return;
    }
    packet_index = 0;

    uint8_t status = packet[0];
    int32_t dx = packet[1];
    int32_t dy = packet[2];
    if (status & 0x10) {
        dx -= 256;
    }
    if (status & 0x20) {
        dy -= 256;
    }

    mouse_event_t ev;
    ev.dx = dx;
    ev.dy = -dy;
    ev.buttons = status & 0x07;
    ev.time_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
    ev.wheel = 0;
    if (packet_bytes == 4) {
        int32_t z = packet[3] & 0x0F;
        if (z & 0x08) {
            z -= 16;
        }
        ev.wheel = -z;
    }
    push_event(ev);
}

void mouse_init(void) {
    buffer_head = 0;
    buffer_tail = 0;
    packet_index = 0;
    packet_bytes = 3;

    ps2_write_command(0xA8);

    mouse_write(0xF6);

    mouse_write(0xF3); mouse_write(200);
    mouse_write(0xF3); mouse_write(100);
    mouse_write(0xF3); mouse_write(80);
    mouse_write(0xF2);
    uint8_t device_id = ps2_read_data();
    packet_bytes = (device_id == 0x03) ? 4 : 3;

    mouse_write(0xF4);
    ps2_flush_output_buffer();

    ps2_write_command(0x20);
    uint8_t config = ps2_read_data();
    config |= 0x02;
    ps2_write_command(0x60);
    ps2_write_data(config);

    kernel_log_puts(packet_bytes == 4
                  ? "[mouse] IntelliMouse 4-byte protocol negotiated - wheel events enabled.\n"
                  : "[mouse] standard 3-byte protocol - no wheel on this device.\n");

    irq_register_handler(MOUSE_IRQ, mouse_irq);
    irq_enable_line(CASCADE_IRQ);
    irq_enable_line(MOUSE_IRQ);
}

void mouse_inject(int32_t dx, int32_t dy, uint8_t buttons, int32_t wheel) {
    mouse_event_t ev;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = buttons & 0x07u;
    ev.time_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
    ev.wheel = wheel;
    push_event(ev);
}

int mouse_read(mouse_event_t *ev) {
    if (buffer_tail == buffer_head) {
        return 0;
    }
    *ev = event_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % EVENT_BUFFER_SIZE;
    return 1;
}
