#include "pc_speaker.h"

#include "architecture/x86_64/io.h"
#include "pit.h"

#define PIT_CHANNEL2_DATA 0x42
#define PIT_COMMAND       0x43
#define PIT_BASE_FREQ     1193182u

#define PIT_CMD_CHANNEL2     0x80
#define PIT_CMD_LOHI         0x30
#define PIT_CMD_MODE3_SQUARE 0x06

#define SPEAKER_PORT       0x61
#define SPEAKER_GATE_BITS  0x03

static uint64_t off_at_tick;
static int playing;
static int muted;

void pcspk_init(void) {
    pcspk_off();
}

void pcspk_set_muted(int m) {
    muted = m;
    if (muted) {
        pcspk_off();
    }
}

void pcspk_off(void) {
    outb(SPEAKER_PORT, (uint8_t)(inb(SPEAKER_PORT) & (uint8_t)~SPEAKER_GATE_BITS));
    playing = 0;
    off_at_tick = 0;
}

void pcspk_tone(uint32_t freq_hz, uint32_t ms) {
    if (muted || freq_hz == 0 || ms == 0) {
        pcspk_off();
        return;
    }
    uint32_t divisor = PIT_BASE_FREQ / freq_hz;
    if (divisor < 2) {
        divisor = 2;
    }
    if (divisor > 0xFFFFu) {
        divisor = 0xFFFFu;
    }

    outb(PIT_COMMAND, PIT_CMD_CHANNEL2 | PIT_CMD_LOHI | PIT_CMD_MODE3_SQUARE);
    outb(PIT_CHANNEL2_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    uint8_t gate = inb(SPEAKER_PORT);
    if ((gate & SPEAKER_GATE_BITS) != SPEAKER_GATE_BITS) {
        outb(SPEAKER_PORT, (uint8_t)(gate | SPEAKER_GATE_BITS));
    }

    uint64_t ticks = ((uint64_t)ms * PIT_HZ + 999) / 1000;
    if (ticks == 0) {
        ticks = 1;
    }
    off_at_tick = pit_get_ticks() + ticks;
    playing = 1;
}

void pcspk_tick(void) {
    if (playing && pit_get_ticks() >= off_at_tick) {
        pcspk_off();
    }
}
