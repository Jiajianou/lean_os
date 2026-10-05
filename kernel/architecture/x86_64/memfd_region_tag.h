#pragma once

#include <stdint.h>

#include "inter_process_communication/memfd.h"

/* A mapped region names the memfd behind it by a tag rather than a pointer -
   its slot in the memfd table plus one (0 is "no memfd") and that slot's
   generation, so a region outliving its memfd finds nothing instead of
   whatever reuses the slot. The memfd table grows to MEMFD_MAX (65,536)
   entries; the slot was a uint8_t once, in every region's tag.

   M225: two of syscall.c's failure paths still narrowed it -
   memfd_by_tag((uint8_t)(id - 1), gen) - when an mmap of a memfd, or the
   split of a memfd region, could not get a region-table entry. Past 256
   memfds that gave the region's reference back to memfd (slot mod 256) if
   its generation happened to match - another program's shared memory freed
   under it - and otherwise to nobody, a leak. Every tag is resolved here,
   with all of its bits, so the narrowing has nowhere left to be written. */
static inline struct memfd *memfd_region_tag_resolve(uint32_t memfd_id, uint16_t memfd_gen) {
    if (memfd_id == 0) {
        return (struct memfd *)0;
    }
    return memfd_by_tag(memfd_id - 1, memfd_gen);
}

static inline void memfd_region_tag_reference(uint32_t memfd_id, uint16_t memfd_gen) {
    if (memfd_id) {
        memfd_region_reference(memfd_region_tag_resolve(memfd_id, memfd_gen));
    }
}

static inline void memfd_region_tag_unref(uint32_t memfd_id, uint16_t memfd_gen) {
    if (memfd_id) {
        memfd_region_unref(memfd_region_tag_resolve(memfd_id, memfd_gen));
    }
}
