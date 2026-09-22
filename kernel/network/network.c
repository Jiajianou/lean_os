#include "network.h"

#include "arp.h"
#include "dhcp.h"
#include "drivers/kernel_log.h"
#include "drivers/pit.h"
#include "drivers/rtl8139.h"
#include "scheduler/scheduler.h"
#include "socket.h"
#include "tcp.h"
#include "library/spinlock.h"
#include "architecture/x86_64/io.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"

static uint32_t local_ip   = NET_FALLBACK_IP;
static uint32_t subnet     = NET_FALLBACK_MASK;
static uint32_t gateway_ip = NET_FALLBACK_GATEWAY;
static uint32_t dns_ip     = NET_FALLBACK_DNS;
static int leased;
static int have_nic;

static spinlock_t net_lock;
static volatile int net_lock_owner_cpu = -1;
static int net_lock_depth;
static uint64_t net_lock_flags;

void net_lock_acquire(void) {
    uint64_t f = irq_save_disable();
    int cpu = smp_current_cpu();
    if (net_lock_owner_cpu == cpu) {
        net_lock_depth++;
        return;
    }
    spin_lock(&net_lock);
    net_lock_owner_cpu = cpu;
    net_lock_depth = 1;
    net_lock_flags = f;
}

void net_lock_release(void) {
    if (--net_lock_depth > 0) {
        return;
    }
    uint64_t f = net_lock_flags;
    net_lock_owner_cpu = -1;
    spin_unlock_irqrestore(&net_lock, f);
}

static void log_ip(const char *label, uint32_t ip) {
    kernel_log_puts(label);
    for (int shift = 24; shift >= 0; shift -= 8) {
        kernel_log_put_dec((ip >> shift) & 0xFF);
        if (shift) {
            kernel_log_putc('.');
        }
    }
}

static void tcp_timer_thread(void *arg) {
    (void)arg;
    for (;;) {
        /* Blocking rather than pit_sleep_ms, which halts in a loop and stays
           runnable: this task sleeps 100 ms and works for microseconds, so
           on a busy machine the halting is what it mostly does - 29 seconds
           of CPU during a 58-second self-test stage, measured. M170. */
        scheduler_sleep_ms(TCP_TICK_MS);
        tcp_tick();
    }
}

int net_init(void) {
    if (!rtl8139_init()) {
        return 0;
    }
    have_nic = 1;
    arp_init();
    socket_init();
    tcp_init();

    task_t *timer = task_spawn("tcp-timer", tcp_timer_thread, (void *)0);
    if (timer) {
        timer->parent_id = -1;
    }

    dhcp_configure();

    log_ip("[net] ", local_ip);
    kernel_log_puts("/");
    int bits = 0;
    for (uint32_t m = subnet; m & 0x80000000u; m <<= 1) {
        bits++;
    }
    kernel_log_put_dec((uint32_t)bits);
    log_ip(" via ", gateway_ip);
    log_ip(", DNS ", dns_ip);
    kernel_log_puts(leased ? " (DHCP lease)\n" : " (no DHCP answer - fallback configuration)\n");
    return 1;
}

const uint8_t *net_local_mac(void) {
    return rtl8139_mac();
}

uint32_t net_local_ip(void) { return local_ip; }
uint32_t net_gateway_ip(void) { return gateway_ip; }
uint32_t net_subnet_mask(void) { return subnet; }
uint32_t net_dns_ip(void) { return dns_ip; }
int net_config_is_leased(void) { return leased; }
int net_have_nic(void) { return have_nic; }

void net_set_config(uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns) {
    local_ip = ip;
    subnet = mask ? mask : NET_FALLBACK_MASK;
    gateway_ip = gateway;
    dns_ip = dns ? dns : NET_FALLBACK_DNS;
    leased = 1;
}

int net_is_local_ip(uint32_t ip) {
    return ip == local_ip ||
           ip == NET_BROADCAST_IP ||
           (ip & NET_LOOPBACK_MASK) == NET_LOOPBACK_NET;
}
