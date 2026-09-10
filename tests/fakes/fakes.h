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
/* M100: refuse the (n+1)th mapping and every one after it, modelling the
 * one way the real vmm_try_map_page_in returns -1 - no frame for a page
 * table. -1 restores "never refuse". Reset by fake_vmm_reset. */
void fake_vmm_fail_map_after(int n);

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
/* Q16: make every write from the n'th onward fail and SAY so, which is
 * what a disk that has stopped accepting writes looks like from above
 * now that kernel/drivers/blk.h has a return value. */
void fake_blk_fail_writes_after(int64_t n);
/* ...and the pre-Q16 version, kept because it is a different question:
 * a disk that drops writes and reports success asks whether the
 * filesystem is still consistent when nobody was told. */
void fake_blk_fail_writes_silently_after(int64_t n);
/* Q16: and the read side, which had no equivalent at all. */
void fake_blk_fail_reads_after(int64_t n);

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

/* ---- fake_spinlock ---------------------------------------------------- */
/* Every lock this process holds, dropped. The runner calls it between
 * tests; a test that deliberately provokes a panic has to call it too,
 * because CHECK_PANIC returns into the middle of the test with whatever
 * the panicking code was holding still held. */
void fake_spinlock_release_all(void);
/* Q9: how many distinct (outer, inner) lock pairs this run has observed.
 * A test asserts it is non-zero, because a lock-order checker that has
 * never seen a nested acquisition is a checker that cannot fail. */
int fake_spinlock_order_pairs(void);

/* ---- fake_arch -------------------------------------------------------- */
/* The machine kernel/sched/sched.c runs on, reduced to what it asks and
 * what a test wants to control. See fake_arch.c - especially its note on
 * what this tier can and cannot grade about a context switch. */
void fake_arch_reset(void);
void fake_arch_set_cpu(int cpu);
int fake_arch_broadcasts(void);
uint64_t fake_arch_switches(void);
/* Where this CPU is standing. M106's check panics unless it is inside
 * the outgoing task's kernel stack, and there is no honest default on a
 * host - a test says. */
void fake_arch_stand_on(uint64_t sp);
unsigned long long fake_cpu_last_msr(unsigned int msr);
uint8_t *fake_arch_fpu_scratch(void);
void fake_arch_fire_tick_hook(void);

/* ---- fake_kernel_objects ---------------------------------------------- */
/* Refcounts on the things a descriptor table points at, so "a task that
 * died gave back every reference it held" is an assertion. */
void fake_objects_reset(void);
int fake_objects_pipe_read_refs(void);
int fake_objects_pipe_write_refs(void);
int fake_objects_file_refs(void);
int fake_objects_socket_refs(void);
int fake_objects_address_spaces_destroyed(void);
int fake_objects_shm_frees(void);

/* ---- fake_user_syscalls, M98 -------------------------------------------
 *
 * The host-side sbrk/mmap under user_space/lib/malloc.c, so the
 * allocator can be tested off the machine. */
void fake_user_heap_reset(void);
void fake_user_sbrk_refuse(int on);
size_t fake_user_heap_used(void);

/* ---- fake_vmm, Q13 additions ------------------------------------------ */
uint64_t fake_vmm_cow_breaks(void);
uint64_t fake_vmm_unmaps_in(void);
/* M98: what vmm_rss_peak_pages answers for every address space here. */
void fake_vmm_set_rss_peak(uint64_t pages);

/* ---- fake_user_fs, M112 ------------------------------------------------
 *
 * The five filesystem calls user_space/lib/fsutil.c makes, backed by a
 * real directory tree under a per-reset mkdtemp. The point of backing
 * them with the host's own filesystem rather than a model is that "the
 * folder is gone" is then answered by the host's `stat` and not by the
 * code that deleted it - see the file's own header. */
void fake_user_fs_reset(void);
/* Building a tree to walk. Guest paths ("/a/b"), rewritten under the
 * temp root before they touch anything. */
int fake_user_fs_mkdir(const char *guest);
int fake_user_fs_write(const char *guest, size_t bytes);
int fake_user_fs_exists(const char *guest);

/* ---- fake_user_net, M114 ------------------------------------------------
 *
 * A network for user_space/lib/dns.c with no network in it. Servers are
 * addressed by IP and behave the way the milestone needed to reproduce -
 * above all FAKE_DNS_SILENT, the nameserver that accepts a query and
 * never answers, which is the failure M114 exists for and the one that
 * cannot be arranged against a real server. See the file's own header.
 *
 * Time is a counter this fake advances on every poll, so a three-second
 * timeout costs no seconds. */
#define FAKE_DNS_SILENT   0
#define FAKE_DNS_ANSWER   1
#define FAKE_DNS_NXDOMAIN 2
#define FAKE_DNS_GARBAGE  3

void fake_user_net_reset(void);
void fake_user_net_add_server(uint32_t ip, int behaviour, uint32_t answer);
void fake_user_net_set_dhcp_dns(uint32_t ip);
void fake_user_net_set_netconf_fails(int fails);
void fake_user_net_set_resolv_conf(const char *text, long len);
void fake_user_net_no_resolv_conf(void);
void fake_user_net_set_socket_fails(int fails);
void fake_user_net_set_bind_fails(int fails);
/* How many queries that address was actually sent - which is how "it
 * asked every server" and "it stopped asking after a definite no" are
 * told apart. Returns -1 for an address that was never registered. */
int fake_user_net_queries_seen(uint32_t ip);
