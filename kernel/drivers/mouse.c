#include "mouse.h"

#include "scheduler/scheduler.h"

#include "architecture/x86_64/io.h"
#include "architecture/x86_64/interrupt_service_routines.h"
#include "architecture/x86_64/pic.h"
#include "device/fwcfg.h"
#include "drivers/kernel_log.h"
#include "drivers/pit.h"
#include "drivers/ps2_controller.h"

#define PS2_DATA_PORT   0x60

#define MOUSE_IRQ    12
#define CASCADE_IRQ  2

static int mouse_present;

static int mouse_write(uint8_t data) {
    return ps2_controller_write_aux(data);
}

#define EVENT_BUFFER_SIZE 64
static mouse_event_t event_buffer[EVENT_BUFFER_SIZE];
static volatile uint32_t buffer_head;
static volatile uint32_t buffer_tail;

static int packet_bytes = 3;

/* M211. QEMU has no I2C touchpad, so the trackpad's half of the pointer
   settings could never be graded by the input suite - every event it can
   make is a PS/2 mouse's. opt/leanos/pointer=trackpad, from outside the
   image like every other harness switch, makes the machine's mouse - this
   port's, or a USB one on a machine with no PS/2 controller - arrive
   labelled as a trackpad, which is the only thing the compositor looks at
   to choose between the two sets of settings. */
static uint8_t ps2_source = MOUSE_SOURCE_MOUSE;

static uint8_t packet[4];
static int packet_index;

int mouse_pending(void) {
    return buffer_head != buffer_tail;
}

static void push_event(mouse_event_t ev) {
    uint32_t next = (buffer_head + 1) % EVENT_BUFFER_SIZE;
    if (next == buffer_tail) {
        return;
    }
    event_buffer[buffer_head] = ev;
    buffer_head = next;
    scheduler_wake_object(SCHEDULER_INPUT_OBJECT);
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
    ev.source = ps2_source;
    ev.flags = 0;
    ev.time_ms = (uint32_t)(clock_monotonic_ms());
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
    mouse_present = 0;

    char pointer_switch[16];
    int switch_length = fwcfg_read_file("opt/leanos/pointer", pointer_switch, sizeof(pointer_switch) - 1);
    ps2_source = MOUSE_SOURCE_MOUSE;
    if (switch_length == 8) {
        pointer_switch[switch_length] = '\0';
        static const char trackpad[] = "trackpad";
        int same = 1;
        for (int i = 0; i < 8; i++) {
            same &= pointer_switch[i] == trackpad[i];
        }
        if (same) {
            ps2_source = MOUSE_SOURCE_TRACKPAD;
            kernel_log_puts("[mouse] opt/leanos/pointer=trackpad - the mouse's events are labelled as a "
                            "trackpad's.\n");
        }
    }

    if (!(ps2_controller_ports() & PS2_AUX_PORT_PRESENT)) {
        kernel_log_puts("[mouse] no PS/2 auxiliary port - nothing to negotiate with. A USB "
                   "mouse reaches the same ring buffer through the xHCI driver.\n");
        return;
    }

    if (!mouse_write(0xF6)) {
        kernel_log_puts("[mouse] the auxiliary port tested good but nothing on it answered - "
                   "IRQ12 left masked.\n");
        return;
    }
    mouse_present = 1;

    mouse_write(0xF3); mouse_write(200);
    mouse_write(0xF3); mouse_write(100);
    mouse_write(0xF3); mouse_write(80);

    uint8_t device_id = 0;
    packet_bytes = 3;
    if (mouse_write(0xF2) && ps2_controller_read_data(&device_id) && device_id == 0x03) {
        packet_bytes = 4;
    }

    mouse_write(0xF4);
    ps2_controller_flush();

    kernel_log_puts(packet_bytes == 4
                  ? "[mouse] IntelliMouse 4-byte protocol negotiated - wheel events enabled.\n"
                  : "[mouse] standard 3-byte protocol - no wheel on this device.\n");

    irq_register_handler(MOUSE_IRQ, mouse_irq);
    irq_enable_line(CASCADE_IRQ);
    irq_enable_line(MOUSE_IRQ);
}

int mouse_is_present(void) {
    return mouse_present;
}

int mouse_labelled_trackpad(void) {
    return ps2_source == MOUSE_SOURCE_TRACKPAD;
}

int mouse_wheel_present(void) {
    return packet_bytes == 4;
}

void mouse_inject(int32_t dx, int32_t dy, uint8_t buttons, int32_t wheel) {
    mouse_inject_from(ps2_source, 0, dx, dy, buttons, wheel);
}

void mouse_inject_from(uint8_t source, uint8_t flags, int32_t dx, int32_t dy, uint8_t buttons, int32_t wheel) {
    mouse_event_t ev;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = buttons & 0x07u;
    ev.source = source;
    ev.flags = flags;
    ev.time_ms = (uint32_t)(clock_monotonic_ms());
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
