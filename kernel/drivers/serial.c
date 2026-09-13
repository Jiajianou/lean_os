#include "serial.h"

#include <stdint.h>

#include "architecture/x86_64/io.h"

#define COM1 0x3F8

#define REG_DATA        (COM1 + 0)
#define REG_INT_ENABLE  (COM1 + 1)
#define REG_DIV_LOW     (COM1 + 0)
#define REG_DIV_HIGH    (COM1 + 1)
#define REG_FIFO_CTRL   (COM1 + 2)
#define REG_LINE_CTRL   (COM1 + 3)
#define REG_MODEM_CTRL  (COM1 + 4)
#define REG_LINE_STATUS (COM1 + 5)

#define LCR_8N1      0x03
#define LCR_DLAB     0x80
#define LSR_TX_EMPTY 0x20

void serial_init(void) {
    outb(REG_INT_ENABLE, 0x00);
    outb(REG_LINE_CTRL, LCR_DLAB);
    outb(REG_DIV_LOW, 0x03);
    outb(REG_DIV_HIGH, 0x00);
    outb(REG_LINE_CTRL, LCR_8N1);
    outb(REG_FIFO_CTRL, 0xC7);
    outb(REG_MODEM_CTRL, 0x0B);
}

static int transmit_empty(void) {
    return inb(REG_LINE_STATUS) & LSR_TX_EMPTY;
}

void serial_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');
    }
    while (!transmit_empty()) {
    }
    outb(REG_DATA, (uint8_t)c);
}
