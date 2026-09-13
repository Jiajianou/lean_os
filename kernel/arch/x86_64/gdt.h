#pragma once

#include <stdint.h>

#include "cpu.h"

#define GDT_KERNEL_CODE_SEL 0x08
#define GDT_KERNEL_DATA_SEL 0x10
#define GDT_TSS_SEL_BASE    0x18
#define GDT_USER_CODE_SEL   (GDT_TSS_SEL_BASE + MAX_CPUS * 16)
#define GDT_USER_DATA_SEL   (GDT_USER_CODE_SEL + 8)

void gdt_init(void);

void gdt_init_ap(int cpu_id);

static inline uint16_t gdt_tss_selector(int cpu_id) {
    return (uint16_t)(GDT_TSS_SEL_BASE + cpu_id * 16);
}

#if defined(__x86_64__)
static inline int gdt_current_cpu(void) {
    uint16_t sel;
    __asm__ volatile("str %0" : "=r"(sel));
    if (sel < GDT_TSS_SEL_BASE) {
        return 0;
    }
    return (int)((sel - GDT_TSS_SEL_BASE) / 16);
}
#else
int gdt_current_cpu(void);
#endif

void tss_set_rsp0(int cpu_id, uint64_t rsp0);
uint64_t tss_get_rsp0(int cpu_id);

void gdt_get_table_ptr(uint16_t *limit_out, uint64_t *base_out);
