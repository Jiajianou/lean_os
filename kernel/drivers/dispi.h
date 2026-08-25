/* kernel/drivers/dispi.h
 *
 * M58: runtime mode setting for the Bochs/QEMU "stdvga" display adapter,
 * through its VBE DISPI register pair at 0x1CE/0x1CF.
 *
 * Why this exists at all, stated plainly because it is the whole shape of
 * the milestone: the resolution this desktop runs at was chosen by
 * kernel/boot/uefi/boot.c through the UEFI Graphics Output Protocol, and
 * GOP is a *boot services* protocol - build_e820_and_exit_boot_services()
 * is the last moment in this machine's life when anything can call
 * SetMode. So a Settings pane that changes resolution is not a pane with
 * a small driver behind it; it is a native mode-setting driver with a
 * pane in front of it, and this file is the driver.
 *
 * Where it stops working, said out loud: DISPI is a *device* interface
 * that Bochs and QEMU implement, not something any real GPU does. On real
 * hardware dispi_probe() finds nothing, dispi_mode_count() is zero, and
 * the honest behaviour is the mode the firmware picked - a Display pane
 * that shows it and offers nothing. Real mode setting on real hardware is
 * a GPU driver per vendor and is not a thing this project will do. That
 * is one of the three things (alongside the ramdisk root and a panic that
 * paints) the deferred real-hardware work has to answer for.
 */
#pragma once

#include <stdint.h>

#include "display.h" /* system_api/include/display.h - display_mode_t, DISPLAY_MAX_MODES */

/* Probes for the adapter and builds the validated mode list. Safe to call
 * on hardware that has no such device: it simply finds none, and every
 * function below then reports "nothing on offer" rather than failing. */
void dispi_init(void);

/* 1 if a Bochs/QEMU DISPI adapter answered the probe. */
int dispi_available(void);

/* Video memory the device reports, in bytes - the real ceiling on which
 * modes can be offered, and read from the device rather than assumed. */
uint32_t dispi_vram_bytes(void);

/* The curated, *validated* mode list: standard sizes, each one already
 * checked against the device's own XRES/YRES limits and against how much
 * video memory it reports. A list that included a mode the device will
 * refuse is a list that loses somebody their desktop, which is why the
 * checking happens here, once, rather than at the moment somebody clicks.
 * Copies at most `max` entries into `out` and returns how many exist. */
int dispi_mode_count(void);
int dispi_get_modes(display_mode_t *out, int max);

/* Programs the device for w x h at 32bpp with the linear framebuffer
 * enabled, then *reads back* the scanline stride the device actually
 * chose and stores it in *out_pitch. Reading it back is the point: fb.h
 * has documented since M16 that pitch "is not necessarily width * 4", and
 * assuming otherwise is how a mode change turns into a sheared screen.
 * Returns 0 on success, -1 if the mode is not one this device will take.
 *
 * Does not touch fb.c - see display_set_mode() in kernel/drivers/fb.h's
 * neighbourhood for the caller that ties the two together. */
int dispi_set_mode(uint32_t w, uint32_t h, uint32_t *out_pitch);
