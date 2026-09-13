#pragma once

#include <stdint.h>

#define EVENTFD_MAX 64

#define EVENTFD_MAX_COUNT 0xFFFFFFFFFFFFFFFEULL

struct eventfd;

void eventfd_init(void);

struct eventfd *eventfd_create(uint64_t initval, int semaphore);

void eventfd_reference(struct eventfd *e);
void eventfd_unref(struct eventfd *e);

int eventfd_read(struct eventfd *e, uint64_t *out);

int eventfd_write(struct eventfd *e, uint64_t v);

int eventfd_readable(const struct eventfd *e);

int eventfd_writable(const struct eventfd *e);

int eventfd_in_use(void);
