#pragma once

#include <stdint.h>

#define MEMFD_MAX 32

#define MEMFD_MAX_PAGES 4096

#define MEMFD_NAME_MAX 24

#define MEMFD_SEAL_SEAL   0x0001
#define MEMFD_SEAL_SHRINK 0x0002
#define MEMFD_SEAL_GROW   0x0004
#define MEMFD_SEAL_WRITE  0x0008

struct memfd;

void memfd_init(void);

struct memfd *memfd_create_object(const char *name);

void memfd_reference(struct memfd *m);
void memfd_unref(struct memfd *m);

uint8_t memfd_slot(const struct memfd *m);
uint16_t memfd_generation(const struct memfd *m);

struct memfd *memfd_by_tag(uint8_t slot, uint16_t generation);

int memfd_truncate(struct memfd *m, uint64_t size);

uint64_t memfd_size(const struct memfd *m);

uint64_t memfd_frame(const struct memfd *m, uint32_t index);

int memfd_add_seals(struct memfd *m, uint32_t seals);
uint32_t memfd_get_seals(const struct memfd *m);

int memfd_may_write(const struct memfd *m);

void memfd_region_reference(struct memfd *m);
void memfd_region_unref(struct memfd *m);

const char *memfd_name(const struct memfd *m);

const char *memfd_first_live_name(void);

int memfd_in_use(void);
uint32_t memfd_pages_held(void);
