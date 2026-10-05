#include "serial.h"

#include <stdint.h>

#include "architecture/x86_64/io.h"

#define COM1 0x3F8

#define REG_DATA        (COM1 + 0)
#define REG_INT_ENABLE  (COM1 + 1)
#define REG_DIV_LOW     (COM1 + 0)
#define REG_DIV_HIGH    (COM1 + 1)
#define REG_FIFO_CTRL   (COM1 + 2)
#define REG_INT_ID      (COM1 + 2)
#define REG_LINE_CTRL   (COM1 + 3)
#define REG_MODEM_CTRL  (COM1 + 4)
#define REG_LINE_STATUS (COM1 + 5)
#define REG_SCRATCH     (COM1 + 7)

#define LCR_8N1      0x03
#define LCR_DLAB     0x80
#define LSR_TX_EMPTY 0x20

static int present;

/* M225: how many characters the transmitter holds. The port has been
   programmed with its FIFO on (0xC7) since the start, and then fed one
   character at a time with a wait for an EMPTY transmitter before each -
   so the FIFO was never used, and every character cost a full character
   time (260 us at this divisor's 38400 baud) with the log's lock held and
   interrupts off. A 16550A says its FIFO is working in the top two bits of
   the interrupt identification register; anything else is fed one at a
   time, as before. */
static int fifo_depth = 1;

void serial_init(void) {
    outb(REG_SCRATCH, 0x5A);
    uint8_t first = inb(REG_SCRATCH);
    outb(REG_SCRATCH, 0xA5);
    uint8_t second = inb(REG_SCRATCH);
    present = first == 0x5A && second == 0xA5;
    if (!present) {
        return;
    }
    outb(REG_INT_ENABLE, 0x00);
    outb(REG_LINE_CTRL, LCR_DLAB);
    outb(REG_DIV_LOW, 0x03);
    outb(REG_DIV_HIGH, 0x00);
    outb(REG_LINE_CTRL, LCR_8N1);
    outb(REG_FIFO_CTRL, 0xC7);
    outb(REG_MODEM_CTRL, 0x0B);
    fifo_depth = (inb(REG_INT_ID) & 0xC0) == 0xC0 ? 16 : 1;
}

static int transmit_empty(void) {
    return inb(REG_LINE_STATUS) & LSR_TX_EMPTY;
}

void serial_putc(char c) {
    if (!present) {
        return;
    }
    if (c == '\n') {
        serial_putc('\r');
    }
    while (!transmit_empty()) {
    }
    outb(REG_DATA, (uint8_t)c);
}

int serial_tx_room(void) {
    if (!present) {
        return 1 << 20;
    }
    return transmit_empty() ? fifo_depth : 0;
}

void serial_tx_put(char c) {
    if (!present) {
        return;
    }
    if (c == '\n') {
        outb(REG_DATA, (uint8_t)'\r');
        if (fifo_depth < 2) {
            while (!transmit_empty()) {
            }
        }
    }
    outb(REG_DATA, (uint8_t)c);
}
