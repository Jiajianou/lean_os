/* tests/fakes/fakes.h - Q2: the test-visible controls the fakes expose.
 *
 * Everything here is a knob the real kernel does not have and could not
 * have: a way to make an allocation fail on demand, a count of what has
 * not been given back, a tally of what reached the disk. These are the
 * instruments the whole host tier exists for. */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* ---- fake_pmm --------------------------------------------------------- */
void fake_pmm_reset(void);
/* Make allocation number n (0-based) and every one after it fail.
 * -1 restores the default of never failing. */
void fake_pmm_fail_after(int64_t n);
uint64_t fake_pmm_outstanding(void);
uint64_t fake_pmm_total_allocs(void);

/* ---- fake_vmm --------------------------------------------------------- */
void fake_vmm_reset(void);
uint64_t fake_vmm_mapped_pages(void);

/* ---- fake_klog -------------------------------------------------------- */
void klog_capture_reset(void);
const char *klog_capture(void);
int klog_capture_contains(const char *needle);

/* ---- fake_blk --------------------------------------------------------- */
/* A RAM-backed disk of `sectors` 512-byte sectors. */
void fake_blk_reset(uint32_t sectors);
void fake_blk_free(void);
/* Device traffic since the last counter reset - the numbers that let a
 * test regress an *algorithm* (how many reads does this cost?) and not
 * only an outcome. */
void fake_blk_reset_counters(void);
uint64_t fake_blk_reads(void);
uint64_t fake_blk_writes(void);
/* Direct access, for corrupting the image on purpose. */
uint8_t *fake_blk_sector(uint32_t lba);
uint32_t fake_blk_sector_count(void);
/* Make every write from the n'th onward silently do nothing, which is
 * what a disk that has stopped accepting writes looks like from above. */
void fake_blk_fail_writes_after(int64_t n);

/* ---- fake_net --------------------------------------------------------- */
/* Frames the stack under test tried to transmit. */
void fake_net_reset(void);
int fake_net_tx_count(void);
const uint8_t *fake_net_tx_frame(int i, uint32_t *len_out);

/* ---- fake_pit --------------------------------------------------------- */
void fake_pit_set(uint64_t t);
void fake_pit_advance(uint64_t t);

/* ---- fake_rtc --------------------------------------------------------- */
void fake_rtc_set(uint32_t t);
void fake_rtc_advance(uint32_t secs);

/* ---- fake_fwcfg ------------------------------------------------------- */
/* A scripted QEMU firmware-config device. An item nobody sets reads back
 * 0xFF, which is what an unclaimed port does. */
void fake_fwcfg_reset(void);
void fake_fwcfg_set_item(uint16_t selector, const uint8_t *data, uint32_t len);
void fake_fwcfg_append_item(uint16_t selector, const uint8_t *data, uint32_t len);

/* ---- fake_socket ------------------------------------------------------ */
/* What UDP handed up to the socket layer - the assertion that separates
 * "it did not crash" from "it delivered exactly these bytes to exactly
 * this port", which is the difference between a fuzz check and a test. */
void fake_socket_reset(void);
int fake_socket_delivered_count(void);
const uint8_t *fake_socket_delivered(int i, uint32_t *len_out, uint16_t *dst_port_out);
uint16_t fake_socket_delivered_src_port(int i);
uint32_t fake_socket_delivered_src_ip(int i);
