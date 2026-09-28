#pragma once

#include <stdint.h>
#include <stddef.h>

void fake_physical_memory_reset(void);
void fake_physical_memory_fail_after(int64_t n);
uint64_t fake_physical_memory_outstanding(void);
uint64_t fake_physical_memory_total_allocs(void);

void fake_virtual_memory_reset(void);
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

void fake_arch_reset(void);
void fake_arch_set_cpu(int cpu);
void fake_arch_reset_unguarded_cpu_reads(void);
int fake_arch_unguarded_cpu_reads(void);
int fake_arch_broadcasts(void);
uint64_t fake_arch_switches(void);
void fake_arch_stand_on(uint64_t sp);
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
