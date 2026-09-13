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

void pic_remap(void) {
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4_NEEDED);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4_NEEDED);
    io_wait();

    outb(PIC1_DATA, PIC_IRQ_VECTOR_OFFSET);
    io_wait();
    outb(PIC2_DATA, PIC2_IRQ_VECTOR_OFFSET);
    io_wait();

    outb(PIC1_DATA, 4);
    io_wait();
    outb(PIC2_DATA, 2);
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
    if (irq >= 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) & (uint8_t)~(1 << PIC_CASCADE_IRQ));
    }
}
