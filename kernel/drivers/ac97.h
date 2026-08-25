/* kernel/drivers/ac97.h
 *
 * M62: the smallest real audio device QEMU offers, and the same shape of
 * work rtl8139.c already is - PCI enumeration that exists, I/O BARs, a
 * descriptor ring, an IRQ, and a buffer of PCM. The PC speaker
 * (pcspk.h) is what makes a sound on a machine with no sound device at
 * all; this is what makes a sound that is not a square wave.
 *
 * Two I/O regions, which is why pci.h grew a BAR1 accessor: BAR0 is the
 * *mixer* (volume, and the codec's own reset), BAR1 is the bus-master
 * block (the descriptor ring and the transfer state machine). Both are
 * I/O-mapped on the 82801AA QEMU emulates.
 *
 * Output only, and one stream. There is no mixing here and no second
 * channel: a desktop that has never made a sound does not need a sound
 * server, and the honest first step is "this can play a buffer of PCM".
 *
 * Absent hardware degrades rather than panics, exactly as M27 decided for
 * the NIC - ac97_available() is 0 and every call below is a no-op.
 */
#pragma once

#include <stdint.h>

/* Sample format this driver accepts and the device is programmed for:
 * 16-bit signed, stereo, 48 kHz. The one format AC'97 is guaranteed to
 * do without a variable-rate codec, which is why there is no argument
 * for it anywhere below. */
#define AC97_SAMPLE_RATE 48000
#define AC97_CHANNELS    2

void ac97_init(void);
int ac97_available(void);

/* Queues `frames` stereo sample-pairs (so `frames * 4` bytes) and starts
 * the device consuming them. Returns 0, or -1 if there is no device or
 * the buffer is larger than the one this driver owns.
 *
 * Non-blocking: it returns as soon as the ring is armed. Completion is an
 * interrupt, and ac97_completions() is how anything finds out - a play
 * that blocked would be a sound that stops the caller drawing, which is
 * the wrong trade for the same reason pcspk.h gives. */
int ac97_play(const int16_t *samples, uint32_t frames);

/* The largest buffer ac97_play will accept, in stereo frames. */
uint32_t ac97_max_frames(void);

/* How many buffers the device has finished since boot. The self-test's
 * assertion, and the only proof available headlessly that the hardware
 * actually consumed what it was given: QEMU hands no audio back, but the
 * device's own status register says when it has drained the ring.
 *
 * Reading this *polls* that register - see ac97.c's own note for why the
 * completion interrupt does not arrive on this machine and why polling is
 * the honest answer rather than a workaround waiting to be replaced. */
uint32_t ac97_completions(void);

/* 0-100. Applied to the codec's PCM-out and master mixer registers;
 * 0 sets the mixer's own mute bit rather than merely a low volume, which
 * is what makes "mute" mean the same thing on this device as it does on
 * a speaker that has no volume at all. */
void ac97_set_volume(uint32_t percent);

/* M62: what the bus-master block currently thinks it is doing - the
 * status register, the descriptor it is on, and how much of that
 * descriptor is left. Logged by the self-test when a buffer it queued
 * never completes, because "no interrupt arrived" has several very
 * different causes and these three registers tell them apart. */
void ac97_debug_dump(void);
