#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t events;
    uint32_t reserved;
    uint64_t data;
} os_epoll_event_t;

typedef struct {
    uint64_t value_ns;
    uint64_t interval_ns;
} os_itimer_t;

#define OS_EFD_SEMAPHORE 0x1
#define OS_TFD_ABSTIME   0x1
#define OS_FILE_DESCRIPTOR_NONBLOCK   0x800
#define OS_FILE_DESCRIPTOR_CLOEXEC    0x80000

#define OS_CLOCK_REALTIME  0
#define OS_CLOCK_MONOTONIC 1

#ifdef __cplusplus
}
#endif
