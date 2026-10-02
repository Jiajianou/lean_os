#include "network/wireless_manager.h"

#include "device/random.h"
#include "drivers/kernel_log.h"
#include "drivers/pit.h"
#include "file_system/virtual_file_system.h"
#include "memory_management/heap.h"
#include "network/dhcp.h"
#include "network/ethernet.h"
#include "network/network.h"
#include "scheduler/scheduler.h"

/* The manager's seams into the rest of the kernel: the file it remembers
   networks in, the IP stack it hands frames to, and the task loop a radio's
   driver runs it from. */

/* A joined network is serviced every two milliseconds - that is the latency
   a received frame waits, and it costs a read of host memory, not of the
   device. Idle, the loop only has to notice a request from a person. */
#define BUSY_PERIOD_MS 2
#define IDLE_PERIOD_MS 20

/* Four discovers and four requests, two seconds apart: sixteen seconds at
   the most, inside the manager's twenty. */
#define ADDRESSING_ATTEMPTS 4

void wireless_run(const wireless_backend_t *backend) {
    wireless_manager_reset(backend);
    for (;;) {
        wireless_step(clock_monotonic_ms());
        if (wireless_manager_failed()) {
            return;
        }
        scheduler_sleep_ms(wireless_manager_busy() ? BUSY_PERIOD_MS : IDLE_PERIOD_MS);
    }
}

int wireless_storage_read(char *buffer, uint32_t capacity) {
    leanfs_stat_t st;
    if (virtual_file_system_stat(WIRELESS_KNOWN_PATH, &st) != 0 || st.is_directory || st.size > capacity) {
        return -1;
    }
    int64_t n = virtual_file_system_read(WIRELESS_KNOWN_PATH, buffer, capacity);
    return n < 0 ? -1 : (int)n;
}

int wireless_storage_write(const char *buffer, uint32_t length) {
    if (!virtual_file_system_is_directory("/etc/wireless")) {
        virtual_file_system_mkdir("/etc/wireless");
    }
    if (virtual_file_system_write(WIRELESS_KNOWN_PATH, buffer, length) != 0) {
        return -1;
    }
    virtual_file_system_sync();
    return 0;
}

void *wireless_allocate(uint32_t length) {
    return kmalloc(length);
}

void wireless_random(void *out, uint32_t length) {
    random_bytes(out, length);
}

void wireless_deliver_ip(const uint8_t *frame, uint32_t length) {
    if (length <= ETH_MAX_FRAME) {
        eth_receive(frame, (uint16_t)length);
    }
}

static net_link_t wireless_link = {"wlan0", {0}, wireless_link_send};

/* DHCP blocks for seconds and needs the radio's task to keep running while
   it waits - the answer arrives through it - so it runs on a task of its own.
   A generation number makes an answer that arrives after the network was
   left an answer about nothing. */
static volatile uint32_t addressing_generation;
static volatile int addressing_result;

/* dhcp.c holds one exchange's state, so one task asks at a time: a task for
   a network joined again waits for the last one, which notices it has been
   left within one poll and gives up. */
static volatile int addressing_busy;
static volatile uint32_t addressing_asking;

static int addressing_abandoned(void) {
    return addressing_asking != addressing_generation;
}

static void addressing_task(void *argument) {
    uint32_t generation = (uint32_t)(uintptr_t)argument;
    while (__atomic_exchange_n(&addressing_busy, 1, __ATOMIC_ACQUIRE)) {
        if (generation != addressing_generation) {
            return;
        }
        scheduler_sleep_ms(10);
    }
    addressing_asking = generation;
    int ok = generation == addressing_generation && dhcp_configure_retrying(ADDRESSING_ATTEMPTS, addressing_abandoned);
    __atomic_store_n(&addressing_busy, 0, __ATOMIC_RELEASE);
    if (generation == addressing_generation) {
        addressing_result = ok ? 1 : -1;
        if (ok) {
            net_log_configuration();
        }
    }
}

int wireless_addressing_start(const uint8_t address[6]) {
    for (int i = 0; i < 6; i++) {
        wireless_link.address[i] = address[i];
    }
    if (!net_attach_link(&wireless_link)) {
        return 0;
    }
    addressing_result = 0;
    uint32_t generation = ++addressing_generation;
    task_t *task = task_spawn("wifi-dhcp", addressing_task, (void *)(uintptr_t)generation);
    if (!task) {
        addressing_result = -1;
        return 1;
    }
    task->parent_id = -1;
    return 1;
}

int wireless_addressing_poll(uint32_t *ip) {
    int result = addressing_result;
    if (result > 0) {
        *ip = net_local_ip();
    }
    return result;
}

void wireless_addressing_stop(void) {
    addressing_generation++;
    addressing_result = 0;
    net_detach_link(&wireless_link);
}
