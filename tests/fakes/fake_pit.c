/* tests/fakes/fake_pit.c - Q4: a tick counter a test can drive.
 *
 * The real one counts timer interrupts. Here it counts only when a test
 * says so, which is what turns "wait up to two seconds for an ARP reply"
 * into something that finishes instantly and deterministically instead of
 * taking two real seconds, or - worse - taking a different number of
 * iterations on a loaded machine. */
#include "drivers/pit.h"

#include <stdint.h>

static uint64_t ticks;

void fake_pit_set(uint64_t t);
void fake_pit_advance(uint64_t t);

void fake_pit_set(uint64_t t) { ticks = t; }
void fake_pit_advance(uint64_t t) { ticks += t; }

void pit_init(void) { ticks = 0; }
uint64_t pit_get_ticks(void) {
    /* Every read advances it. The kernel's deadline loops are
     * `while (pit_get_ticks() < deadline)`, and a clock that never moves
     * turns each of them into an infinite loop - a hang in a test suite
     * rather than a failure. Moving on every read means every bounded
     * wait terminates, which is the property the tests need from a clock
     * they are not otherwise interested in. */
    return ticks++;
}
void pit_sleep_ms(uint32_t ms) { ticks += ms; }
