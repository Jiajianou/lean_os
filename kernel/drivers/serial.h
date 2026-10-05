#pragma once

void serial_init(void);

/* One character, waiting for the transmitter first: what a panic's last
   words use, with nothing else running. */
void serial_putc(char c);

/* How many characters the transmitter takes right now without a wait: its
   whole FIFO (16 on a 16550A, 1 on a part without one) when it is empty,
   none while it is still sending. A machine with no port takes any number,
   because they go nowhere. The log asks this with interrupts ON and only
   then turns them off to hand over what fits, so a slow port is waited for
   with the machine still answering interrupts. */
int serial_tx_room(void);

/* One character into the transmitter without waiting for room the caller
   was told about by serial_tx_room. '\n' goes out as "\r\n", two of it; on
   a part with a one-character FIFO the '\n' then waits one character time. */
void serial_tx_put(char c);
