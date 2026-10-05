#pragma once

#include <stdint.h>
#include <stddef.h>

void fake_physical_memory_reset(void);
void fake_physical_memory_fail_after(int64_t n);
uint64_t fake_physical_memory_outstanding(void);
uint64_t fake_physical_memory_total_allocs(void);

void fake_virtual_memory_reset(void);
void fake_heap_ensure(void);
uint64_t fake_virtual_memory_mapped_pages(void);
void fake_virtual_memory_fail_map_after(int n);

void kernel_log_capture_reset(void);
const char *kernel_log_capture(void);
int kernel_log_capture_contains(const char *needle);

void fake_block_device_reset(uint32_t sectors);
void fake_block_device_free(void);
void fake_block_device_reset_counters(void);
uint64_t fake_block_device_reads(void);
uint64_t fake_block_device_read_calls(void);
uint64_t fake_block_device_writes(void);
uint8_t *fake_block_device_sector(uint32_t lba);
uint32_t fake_block_device_sector_count(void);
void fake_block_device_fail_writes_after(int64_t n);
void fake_block_device_fail_writes_silently_after(int64_t n);
void fake_block_device_fail_reads_after(int64_t n);
void fake_block_device_write_this_os_boot_sector(void);

void fake_net_reset(void);
int fake_net_tx_count(void);
const uint8_t *fake_net_tx_frame(int i, uint32_t *length_out);

void fake_pit_set(uint64_t t);
void fake_pit_advance(uint64_t t);

void fake_rtc_set(uint32_t t);
void fake_rtc_advance(uint32_t secs);

void fake_fwcfg_reset(void);
void fake_fwcfg_set_item(uint16_t selector, const uint8_t *data, uint32_t length);
void fake_fwcfg_append_item(uint16_t selector, const uint8_t *data, uint32_t length);

void fake_pci_reset(void);
uint32_t fake_pci_config_reads(void);
int fake_pci_add(uint8_t bus, uint8_t slot, uint8_t func, uint16_t vendor, uint16_t device,
                 uint8_t class_code, uint8_t subclass, uint8_t prog_if);
void fake_pci_set_config(int handle, uint8_t offset, uint32_t value);
uint32_t fake_pci_get_config(int handle, uint8_t offset);
void fake_pci_set_bar(int handle, int index, uint32_t value, uint32_t size_mask);

void fake_socket_reset(void);
int fake_socket_delivered_count(void);
const uint8_t *fake_socket_delivered(int i, uint32_t *length_out, uint16_t *destination_port_out);
uint16_t fake_socket_delivered_source_port(int i);
uint32_t fake_socket_delivered_source_ip(int i);

void fake_spinlock_release_all(void);
int fake_spinlock_order_pairs(void);
/* The next acquire of a lock that is held runs this first, as the other
   processor that holds it finishing and letting go (fake_spinlock.c). */
void fake_spinlock_on_contention(void (*hook)(void));
/* M225: runs after every lock release while installed (never inside its own
   run) - the moment another processor could act on what the lock guarded.
   Cleared by passing 0, and at the end of every test. */
void fake_spinlock_on_release(void (*hook)(void));
/* M225: `clear` runs at the end of every test from now on (deduplicated) -
   how a one-shot hook makes sure a failed test does not leave it armed. */
void fake_spinlock_at_test_end(void (*clear)(void));
/* M225: while on, a spinlock is a real one - an atomic exchange, waited on
   with sched_yield - for a test that runs kernel code on several host
   threads at once. The single-threaded checks (recursion, lock order, the
   hooks) are off meanwhile: they keep state no lock protects. Off again at
   the end of every test. */
void fake_spinlock_threaded(int on);

void fake_arch_reset(void);
void fake_arch_set_cpu(int cpu);
void fake_arch_set_smp_initialized(int on);
int fake_arch_reschedules_sent(int cpu);
void fake_arch_reset_reschedules(void);
int fake_vmm_other_cpu_flushes(void);
void fake_arch_reset_unguarded_cpu_reads(void);
int fake_arch_unguarded_cpu_reads(void);
int fake_arch_broadcasts(void);
uint64_t fake_arch_switches(void);
void fake_arch_stand_on(uint64_t sp);
void fake_arch_raise_interrupt(void (*handler)(void));
void fake_lapic_timer_reset(int present);
int fake_lapic_timer_arms(void);
uint64_t fake_lapic_timer_last_delay_ns(void);
unsigned long long fake_cpu_last_msr(unsigned int msr);
uint8_t *fake_arch_fpu_scratch(void);
void fake_arch_fire_tick_hook(void);

void fake_objects_reset(void);
int fake_objects_pipe_read_refs(void);
int fake_objects_pipe_write_refs(void);
int fake_objects_file_refs(void);
int fake_objects_socket_refs(void);
int fake_objects_address_spaces_destroyed(void);
int fake_objects_shared_memory_frees(void);
/* M225: count a pipe-write reference taken while the count was at or below
   `floor` - a reference on an object its last holder already let go of. */
void fake_objects_pipe_write_floor(int floor);
int fake_objects_pipe_write_revivals(void);
/* M225 (fd-use-holds): a pipe write end USED - read, written, asked whether
   it is ready - by a caller that believes it holds it. Counted as dead when
   the count is at or below the floor at that moment: the object's last
   holder had already let it go, so on the machine the use was of freed
   memory. */
void fake_objects_pipe_write_use(void);
int fake_objects_pipe_write_dead_uses(void);
/* M225 (fd-use-holds): the one fake pipe address pipe_is_persistent says yes
   to - a named pipe, never counted down; null (the reset) is none. */
struct pipe;
void fake_objects_pipe_persistent(const struct pipe *p);
/* Runs once, inside the next shared_memory_free_by_owner - which is the first
   thing task_exit_with_code does. */
void fake_objects_on_shared_memory_free(void (*hook)(int owner_task_id));
/* M225: runs after each entry an mmap region walk has looked at and passed
   over, until cleared (0) - the other core changing the table under an
   unlocked reader. */
void fake_objects_on_region_walk_step(void (*hook)(void *task, uint32_t index));

/* M225: runs once, at the start of the next frame allocation. */
void fake_physical_memory_on_alloc(void (*hook)(void));

/* M225: user pages, modelled. Off (the default), the fake answers every
   user range as mapped, as it always has; on, mappings outside the kernel
   heap's reserved region are remembered per page, so a fault path can be
   followed end to end. Turning it on or off forgets them all. */
void fake_virtual_memory_model_user_pages(int on);
/* Whether `virt` is mapped in the model, and to what (either may be 0). */
int fake_virtual_memory_user_page(uint64_t virt, uint64_t *phys_out, uint64_t *flags_out);

void fake_user_heap_reset(void);
void fake_user_sbrk_refuse(int on);
size_t fake_user_heap_used(void);

uint64_t fake_virtual_memory_cow_breaks(void);
uint64_t fake_virtual_memory_unmaps_in(void);
void fake_virtual_memory_set_rss_peak(uint64_t pages);

void fake_user_fs_reset(void);
int fake_user_fs_mkdir(const char *guest);
int fake_user_fs_write(const char *guest, size_t bytes);
int fake_user_fs_exists(const char *guest);
int fake_user_fs_write_text(const char *guest, const char *text);
int fake_user_fs_read_text(const char *guest, char *out, size_t capacity);
int fake_user_fs_symlink(const char *target, const char *guest);
int fake_user_fs_open_count(void);

#define FAKE_USER_FS_FIRST_DESCRIPTOR 1000
int fake_user_fs_owns(int fd);
long fake_user_fs_read(int fd, void *buffer, size_t length);
long fake_user_fs_write_descriptor(int fd, const void *buffer, size_t length);
long fake_user_fs_close(int fd);
extern long fake_user_fs_write_limit;

#define FAKE_DNS_SILENT   0
#define FAKE_DNS_ANSWER   1
#define FAKE_DNS_NXDOMAIN 2
#define FAKE_DNS_GARBAGE  3

void fake_user_net_reset(void);
void fake_user_net_add_server(uint32_t ip, int behaviour, uint32_t answer);
void fake_user_net_set_dhcp_dns(uint32_t ip);
void fake_user_net_set_netconf_fails(int fails);
void fake_user_net_set_resolv_conf(const char *text, long length);
void fake_user_net_no_resolv_conf(void);
void fake_user_net_set_socket_fails(int fails);
void fake_user_net_set_bind_fails(int fails);
int fake_user_net_queries_seen(uint32_t ip);
