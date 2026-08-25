#include "pic.h"

#include "io.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_ICW4_NEEDED 0x01
#define ICW1_INIT        0x10
#define ICW4_8086_MODE   0x01

#define PIC_EOI 0x20

#define PIC2_IRQ_VECTOR_OFFSET (PIC_IRQ_VECTOR_OFFSET + 8)

/* pic_remap: reprograms both 8259s off their power-on vectors (0x08-0x0F,
 * which collide head-on with CPU exceptions like #DF=8 and #GP=13) onto
 * 0x20-0x2F, then masks every line. Nothing unmasks a line until a real
 * driver for it exists (M6+) - until then a stray/unexpected IRQ would
 * have nothing meaningful to do anyway. */
void pic_remap(void) {
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4_NEEDED);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4_NEEDED);
    io_wait();

    outb(PIC1_DATA, PIC_IRQ_VECTOR_OFFSET);
    io_wait();
    outb(PIC2_DATA, PIC2_IRQ_VECTOR_OFFSET);
    io_wait();

    outb(PIC1_DATA, 4); /* tell master: slave PIC lives on IRQ2 */
    io_wait();
    outb(PIC2_DATA, 2); /* tell slave: its cascade identity is IRQ2 */
    io_wait();

    outb(PIC1_DATA, ICW4_8086_MODE);
    io_wait();
    outb(PIC2_DATA, ICW4_8086_MODE);
    io_wait();

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

/* The master's input the slave is wired into - fixed by the PC's own
 * design since the AT, and the reason pic_clear_mask below has a special
 * case rather than a parameter. */
#define PIC_CASCADE_IRQ 2

void pic_set_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(port, inb(port) | (uint8_t)(1 << bit));
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(port, inb(port) & (uint8_t)~(1 << bit));
    /* M62: a line on the slave PIC reaches the CPU only through the
     * master's cascade input (IRQ 2), so unmasking one without the other
     * unmasks nothing at all. Everything this kernel had unmasked until
     * now was on the master - the timer, the keyboard, the mouse - and
     * the RTL8139's own IRQ is never actually waited on (rtl8139.c polls
     * its status register), so an AC'97 controller on IRQ 11 was the
     * first device to find this out: its completion bits were set in the
     * status register the whole time and no interrupt had ever been
     * delivered. */
    if (irq >= 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) & (uint8_t)~(1 << PIC_CASCADE_IRQ));
    }
}
