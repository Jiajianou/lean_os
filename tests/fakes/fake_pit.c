#include "drivers/pit.h"

#include <stdint.h>

static uint64_t ticks;

void fake_pit_set(uint64_t t);
void fake_pit_advance(uint64_t t);

void fake_pit_set(uint64_t t) { ticks = t; }
void fake_pit_advance(uint64_t t) { ticks += t; }

void pit_init(void) { ticks = 0; }
uint64_t pit_get_ticks(void) {
    return ticks++;
}
void pit_sleep_ms(uint32_t ms) { ticks += ms; }
