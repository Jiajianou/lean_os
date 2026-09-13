#pragma once

#include <stdint.h>

#define TIMERFD_MAX 64

#define TIMERFD_CLOCK_REALTIME  0
#define TIMERFD_CLOCK_MONOTONIC 1

struct timerfd;

void timerfd_init(void);

struct timerfd *timerfd_create(int clockid);

void timerfd_reference(struct timerfd *t);
void timerfd_unref(struct timerfd *t);

int timerfd_clock(const struct timerfd *t);

int timerfd_settime(struct timerfd *t, uint64_t now_ns, int absolute,
                    uint64_t value_ns, uint64_t interval_ns,
                    uint64_t *old_value_ns, uint64_t *old_interval_ns);

void timerfd_gettime(const struct timerfd *t, uint64_t now_ns,
                     uint64_t *value_ns, uint64_t *interval_ns);

int timerfd_read(struct timerfd *t, uint64_t now_ns, uint64_t *out);

int timerfd_readable(struct timerfd *t, uint64_t now_ns);

long timerfd_next_ms(struct timerfd *t, uint64_t now_ns);

int timerfd_in_use(void);
