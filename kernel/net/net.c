#include "net.h"

#include "arp.h"
#include "dhcp.h"
#include "drivers/klog.h"
#include "drivers/pit.h"
#include "drivers/rtl8139.h"
#include "sched/sched.h"
#include "socket.h"
#include "tcp.h"
#include "lib/spinlock.h"
#include "arch/x86_64/io.h"   /* irq_save_disable */
#include "arch/x86_64/smp.h"  /* smp_current_cpu */

static uint32_t local_ip   = NET_FALLBACK_IP;
static uint32_t subnet     = NET_FALLBACK_MASK;
static uint32_t gateway_ip = NET_FALLBACK_GATEWAY;
static uint32_t dns_ip     = NET_FALLBACK_DNS;
static int leased;
static int have_nic;

/* See net.h for what this protects and against whom.
 *
 * Recursive, and the reason is worth stating because a recursive lock is
 * normally a smell. The socket table is not only reached from the net
 * syscalls: SYS_close unrefs a socket, SYS_spawn refs every one in the
 * fd table it copies, and task_exit_with_code closes the lot. Those are
 * not net entry points and cannot be bracketed as if they were - but
 * they are *also* reachable from inside a net syscall that already holds
 * this. The two ways out are to prove no path ever nests (a proof that
 * has to be redone every time anyone adds a caller) or to make nesting
 * free. This makes it free.
 *
 * Owner tracked by CPU id rather than by task, which is only sound
 * because the lock is always held with interrupts off: no preemption can
 * move a holder to another CPU mid-section, and no other CPU can be
 * inside it at all. Depth is plain, not atomic, for the same reason -
 * only the owning CPU ever touches it.
 *
 * The saved flags belong to the OUTERMOST acquire. An inner acquire
 * finds interrupts already off and has nothing to save; an inner release
 * must therefore restore nothing, or it would turn interrupts back on
 * half-way through a critical section. */
static spinlock_t net_lock;
static volatile int net_lock_owner_cpu = -1;
static int net_lock_depth;
static uint64_t net_lock_flags;

void net_lock_acquire(void) {
    uint64_t f = irq_save_disable();
    int cpu = smp_current_cpu();
    if (net_lock_owner_cpu == cpu) {
        /* Already ours. The comparison is safe without the lock: only the
         * owning CPU ever writes its own id here, so no other CPU's write
         * can be mistaken for this one's. */
        net_lock_depth++;
        return; /* f is discarded on purpose - IF was already 0 */
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
    klog_puts(label);
    for (int shift = 24; shift >= 0; shift -= 8) {
        klog_put_dec((ip >> shift) & 0xFF);
        if (shift) {
            klog_putc('.');
        }
    }
}

static void tcp_timer_thread(void *arg) {
    (void)arg;
    for (;;) {
        pit_sleep_ms(TCP_TICK_MS);
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

    /* M66: TCP's 100 ms clock, as a kernel thread rather than a second
     * PIT hook - the scheduler owns that hook, and a stack that competed
     * for it would be a networking milestone reaching into the
     * scheduler. The thread spends essentially all of its life halted
     * inside pit_sleep_ms, so what it costs is a task slot and a stack.
     *
     * Parentless, the same way the boot/idle identities are: that is the
     * one mark power_orderly_stop exempts, and this thread is kernel
     * plumbing rather than part of anyone's session - a shutdown that
     * SIGKILLed it would stop TCP retransmission for whatever the kernel
     * still does on the way down (and, in the boot self-tests, for the
     * rest of the session). */
    task_t *timer = task_spawn("tcp-timer", tcp_timer_thread, (void *)0);
    if (timer) {
        timer->parent_id = -1;
    }

    /* Ask before announcing. A DHCP exchange needs the NIC and ARP up
     * and nothing else - it is deliberately its own UDP-shaped path
     * rather than a socket, because it has to run before there is an
     * address for a socket to bind to. */
    dhcp_configure();

    log_ip("[net] ", local_ip);
    klog_puts("/");
    int bits = 0;
    for (uint32_t m = subnet; m & 0x80000000u; m <<= 1) {
        bits++;
    }
    klog_put_dec((uint32_t)bits);
    log_ip(" via ", gateway_ip);
    log_ip(", DNS ", dns_ip);
    klog_puts(leased ? " (DHCP lease)\n" : " (no DHCP answer - fallback configuration)\n");
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
    /* A server that offers no DNS is not a reason to have none: the
     * fallback is still more useful than zero, and a resolver pointed at
     * 0.0.0.0 fails in a much less obvious way. */
    dns_ip = dns ? dns : NET_FALLBACK_DNS;
    leased = 1;
}

int net_is_local_ip(uint32_t ip) {
    return ip == local_ip ||
           ip == NET_BROADCAST_IP ||
           (ip & NET_LOOPBACK_MASK) == NET_LOOPBACK_NET;
}
