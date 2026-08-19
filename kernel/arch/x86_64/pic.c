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

void pic_set_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(port, inb(port) | (uint8_t)(1 << bit));
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(port, inb(port) & (uint8_t)~(1 << bit));
}
