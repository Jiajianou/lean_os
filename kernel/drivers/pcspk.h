/* kernel/drivers/pcspk.h
 *
 * M62: the PC speaker - PIT channel 2 gated onto port 0x61. Tens of
 * lines, and the reason it is here rather than only an AC'97 driver is
 * that it works on machines with no sound device at all: a square wave
 * at a frequency for a duration, on hardware every PC-compatible machine
 * has had since 1981.
 *
 * Channel 2 is a different counter from channel 0 (pit.h's system tick),
 * so programming it disturbs nothing - the two share only the 8254's
 * command port, and each command names its own channel.
 *
 * Nothing here blocks. A tone is started and a deadline recorded; the
 * timer tick turns it off. A sound that held a task for its own duration
 * would be a sound that stops the compositor drawing, which is the wrong
 * trade for something that exists to accompany a toast.
 */
#pragma once

#include <stdint.h>

void pcspk_init(void);

/* Starts a tone and returns immediately. `ms` is how long it will last;
 * a second call replaces whatever was playing rather than queueing, which
 * is what "there is one speaker" means. A frequency of 0 (or a volume of
 * 0 - see pcspk_set_muted) is silence. */
void pcspk_tone(uint32_t freq_hz, uint32_t ms);

/* Stops any tone immediately. */
void pcspk_off(void);

/* M62: the speaker has no volume - it is one bit, on or off - so the
 * only thing a volume control can honestly do to it is mute it. Said
 * here rather than pretended around: a "quiet beep" is not something
 * this hardware can produce. */
void pcspk_set_muted(int muted);

/* Called from the timer tick. Turns a tone off once its deadline has
 * passed; costs one comparison on every tick and nothing else. */
void pcspk_tick(void);
