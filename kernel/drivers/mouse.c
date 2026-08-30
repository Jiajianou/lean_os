#include "mouse.h"

#include "sched/sched.h"

#include "arch/x86_64/io.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "drivers/klog.h"
#include "drivers/pit.h" /* pit_get_ticks - timestamping each event at the source (M40) */

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT    0x64

#define PS2_STATUS_OUTPUT_FULL 0x01u
#define PS2_STATUS_INPUT_FULL  0x02u

#define MOUSE_IRQ    12
#define CASCADE_IRQ  2 /* IRQ2 on the master PIC carries the slave's lines through - M6 never needed it (PIT/keyboard are both master-only) */

/* Bounded polling loops, not indefinite ones - a real 8042 controller
 * that never raises these bits (no aux device present, wrong QEMU
 * machine type, ...) shouldn't be able to hang boot; it just leaves the
 * driver mis-programmed, which mouse_init's caller can't detect yet but
 * won't wedge the machine either. */
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

static void ps2_write_command(uint8_t cmd) {
    ps2_wait_input_clear();
    outb(PS2_CMD_PORT, cmd);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_input_clear();
    outb(PS2_DATA_PORT, data);
}

static uint8_t ps2_read_data(void) {
    ps2_wait_output_full();
    return inb(PS2_DATA_PORT);
}

/* Reads and discards any byte(s) still sitting in the controller's output
 * buffer. Observed in practice (not theoretical): after the 0xF4 "enable
 * reporting" ACK is polled off cleanly, one more byte shows up moments
 * later, right around when IRQ12 gets unmasked - without this flush it
 * lands as packet_index 0 in mouse_irq and permanently shifts 3-byte
 * packet sync by one, so every subsequent real packet gets decoded one
 * byte off (verified by logging the raw byte stream: a stray 0xFA where
 * a real status byte should be, then the actual status/dx bytes
 * misread as dx/dy). Bounded iteration count, not a while-true - this
 * runs during init and must not be able to hang boot. */
static void ps2_flush_output_buffer(void) {
    for (int i = 0; i < 16; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) {
            return;
        }
        (void)inb(PS2_DATA_PORT);
    }
}

/* Routes a byte to the auxiliary device via the controller's "next byte
 * is for the mouse" command (0xD4), then reads back its ACK (0xFA) -
 * discarded rather than checked, since there's nothing more useful to do
 * here if a real mouse doesn't ACK than continue anyway. */
static void mouse_write(uint8_t data) {
    ps2_write_command(0xD4);
    ps2_write_data(data);
    ps2_read_data();
}

#define EVENT_BUFFER_SIZE 64
static mouse_event_t event_buffer[EVENT_BUFFER_SIZE];
static volatile uint32_t buf_head;
static volatile uint32_t buf_tail;

/* M49: 3 for a standard PS/2 mouse, 4 once the IntelliMouse extension is
 * negotiated (see mouse_init). Set once at init and never changed, so the
 * IRQ handler's packet framing is a plain comparison rather than a mode
 * it has to keep track of. */
static int packet_bytes = 3;

static uint8_t packet[4];
static int packet_index;

static void push_event(mouse_event_t ev) {
    uint32_t next = (buf_head + 1) % EVENT_BUFFER_SIZE;
    if (next == buf_tail) {
        return; /* full: drop rather than overwrite an unread event */
    }
    event_buffer[buf_head] = ev;
    buf_head = next;
    /* M68: a mouse event wakes anything in SYS_waitfds - the compositor,
     * which is the one process on this machine that cares. */
    sched_wake_all(SCHED_POLL_CHAN);
}

static void mouse_irq(isr_regs_t *regs) {
    (void)regs;
    uint8_t data = inb(PS2_DATA_PORT);

    /* Byte 0 of a standard 3-byte packet always has bit 3 set - if that's
     * not true here, packet sync was lost (e.g. a byte got dropped
     * somewhere); drop this byte and keep waiting for one that looks
     * like a real first byte instead of decoding garbage. */
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
        dx -= 256; /* sign-extend the 9-bit two's-complement X delta */
    }
    if (status & 0x20) {
        dy -= 256; /* same for Y */
    }

    mouse_event_t ev;
    ev.dx = dx;
    ev.dy = -dy; /* wire protocol is up-positive; screen coordinates are down-positive */
    ev.buttons = status & 0x07;
    /* M40: same value SYS_uptime_ms reports (pit.h's PIT_HZ is 100, so
     * 10ms resolution), stamped here in the interrupt handler - see
     * mouse_event_t.time_ms for why it has to be here and not at the
     * reader. */
    ev.time_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
    /* M49: byte 3 of an IntelliMouse packet is the Z (wheel) delta, a
     * 4-bit two's-complement value in its low nibble - the high nibble
     * carries buttons 4/5 on a 5-button mouse, which this driver has no
     * use for and deliberately masks off rather than misreading as a huge
     * scroll. Sign sense: the wire protocol reports *up* as positive
     * (like dy), and screen coordinates are down-positive, so this is
     * negated for the same reason dy is - one convention for both axes. */
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
    buf_head = 0;
    buf_tail = 0;
    packet_index = 0;
    packet_bytes = 3;

    ps2_write_command(0xA8); /* enable the auxiliary (mouse) device */

    /* F6/F4's ACKs are read via polling, not interrupts - deliberately
     * done *before* the controller's IRQ12-on-aux-activity bit gets set
     * below. Enabling that bit first (as a more "linear" ordering might
     * suggest) let a stray interrupt fire the instant IRQ12 was unmasked
     * even though every byte up to that point had already been drained -
     * observed directly (not theoretical): logging each raw byte showed
     * a phantom 0xFA arriving as the very first IRQ12 event regardless
     * of a buffer flush placed right before unmasking, which only makes
     * sense as a masked/latched interrupt condition from while the
     * aux-IRQ bit was live during the F6/F4 exchange, not a leftover
     * data byte. Keeping IRQ12 generation off at the controller level
     * for the entire polling handshake avoids ever latching that
     * condition in the first place. */
    mouse_write(0xF6); /* set defaults */

    /* M49: the IntelliMouse "knock" - set sample rate to 200, then 100,
     * then 80, and ask the device what it is (0xF2). A mouse that
     * implements the extension answers 0x03 and starts sending 4-byte
     * packets with a wheel delta in the fourth; anything else answers
     * 0x00 and keeps the 3-byte protocol, which is a perfectly ordinary
     * outcome and not a failure - every scrollable surface simply gets
     * `wheel == 0` forever, exactly as it did before this milestone.
     *
     * Done here, in the same polled window as F6/F4 and before IRQ12
     * generation is enabled at the controller, for precisely the reason
     * this function's existing comment gives: the whole handshake has to
     * stay out of the interrupt path or a latched aux-IRQ condition
     * desynchronizes packet framing from the very first real packet. */
    mouse_write(0xF3); mouse_write(200);
    mouse_write(0xF3); mouse_write(100);
    mouse_write(0xF3); mouse_write(80);
    mouse_write(0xF2); /* get device id */
    uint8_t device_id = ps2_read_data();
    packet_bytes = (device_id == 0x03) ? 4 : 3;

    mouse_write(0xF4); /* enable data reporting */
    ps2_flush_output_buffer(); /* belt-and-suspenders: drop anything unexpected still sitting there */

    ps2_write_command(0x20); /* "read controller configuration byte" */
    uint8_t config = ps2_read_data();
    config |= 0x02; /* bit 1: enable IRQ12 on aux (mouse) activity */
    ps2_write_command(0x60); /* "write controller configuration byte" */
    ps2_write_data(config);

    klog_puts(packet_bytes == 4
                  ? "[mouse] IntelliMouse 4-byte protocol negotiated - wheel events enabled.\n"
                  : "[mouse] standard 3-byte protocol - no wheel on this device.\n");

    irq_register_handler(MOUSE_IRQ, mouse_irq);
    /* M62: pic_clear_mask unmasks the cascade itself now for any slave
     * line, so this is redundant - kept because it is also *documented*
     * here (see CASCADE_IRQ) and because a driver saying which lines it
     * needs is not the wrong thing for it to say. */
    pic_clear_mask(CASCADE_IRQ);
    pic_clear_mask(MOUSE_IRQ);
}

void mouse_inject(int32_t dx, int32_t dy, uint8_t buttons, int32_t wheel) {
    mouse_event_t ev;
    ev.dx = dx;
    ev.dy = dy; /* already in screen coordinates - the caller is not a wire protocol, so there is nothing to flip */
    ev.buttons = buttons & 0x07u;
    ev.time_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
    ev.wheel = wheel;
    push_event(ev);
}

int mouse_read(mouse_event_t *ev) {
    if (buf_tail == buf_head) {
        return 0;
    }
    *ev = event_buffer[buf_tail];
    buf_tail = (buf_tail + 1) % EVENT_BUFFER_SIZE;
    return 1;
}
