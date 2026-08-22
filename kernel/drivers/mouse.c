#include "mouse.h"

#include "arch/x86_64/io.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"

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

static uint8_t packet[3];
static int packet_index;

static void push_event(mouse_event_t ev) {
    uint32_t next = (buf_head + 1) % EVENT_BUFFER_SIZE;
    if (next == buf_tail) {
        return; /* full: drop rather than overwrite an unread event */
    }
    event_buffer[buf_head] = ev;
    buf_head = next;
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
    if (packet_index < 3) {
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
    push_event(ev);
}

void mouse_init(void) {
    buf_head = 0;
    buf_tail = 0;
    packet_index = 0;

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
    mouse_write(0xF4); /* enable data reporting */
    ps2_flush_output_buffer(); /* belt-and-suspenders: drop anything unexpected still sitting there */

    ps2_write_command(0x20); /* "read controller configuration byte" */
    uint8_t config = ps2_read_data();
    config |= 0x02; /* bit 1: enable IRQ12 on aux (mouse) activity */
    ps2_write_command(0x60); /* "write controller configuration byte" */
    ps2_write_data(config);

    irq_register_handler(MOUSE_IRQ, mouse_irq);
    pic_clear_mask(CASCADE_IRQ);
    pic_clear_mask(MOUSE_IRQ);
}

int mouse_read(mouse_event_t *ev) {
    if (buf_tail == buf_head) {
        return 0;
    }
    *ev = event_buffer[buf_tail];
    buf_tail = (buf_tail + 1) % EVENT_BUFFER_SIZE;
    return 1;
}
