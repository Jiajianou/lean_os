#include "gdt.h"

#include <stdint.h>

#include "cpu.h"

/* Standard 8-byte segment descriptor. Base/limit are meaningless for the
 * 64-bit code/data segments below (the CPU runs them flat regardless) but
 * are still encoded for completeness/documentation. */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

/* TSS descriptors are 16 bytes in long mode (need a full 64-bit base),
 * i.e. two GDT slots wide. */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} tss_descriptor_t;

typedef struct __attribute__((packed)) {
    gdt_entry_t      null;
    gdt_entry_t      kernel_code;
    gdt_entry_t      kernel_data;
    tss_descriptor_t tss[MAX_CPUS];
    gdt_entry_t      user_code;
    gdt_entry_t      user_data;
} gdt_table_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} table_ptr_t;

/* x86_64 TSS: only the RSP0/IST slots are used. IST1 gives the
 * double-fault handler its own known-good stack, so a fault caused by a
 * corrupt/overflowed kernel stack doesn't turn into a silent triple fault
 * when the CPU tries to push the exception frame. RSP0 is the stack the
 * CPU switches to on any ring3->ring0 transition (M9) - see
 * tss_set_rsp0, updated per-task by the scheduler. */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss_t;

#define GDT_ACCESS_KERNEL_CODE 0x9A /* present, ring0, code, exec/read */
#define GDT_ACCESS_KERNEL_DATA 0x92 /* present, ring0, data, read/write */
#define GDT_ACCESS_TSS         0x89 /* present, ring0, 64-bit TSS (available) */
#define GDT_ACCESS_USER_CODE   0xFA /* present, ring3, code, exec/read */
#define GDT_ACCESS_USER_DATA   0xF2 /* present, ring3, data, read/write */
#define GDT_GRAN_LONG_MODE     0x20 /* L bit: 64-bit code segment */

#define DOUBLE_FAULT_STACK_SIZE 4096

static gdt_table_t gdt;
static tss_t tss[MAX_CPUS]; /* one per possible CPU - see gdt.h's header comment */
static table_ptr_t gdtp;
static uint8_t double_fault_stack[MAX_CPUS][DOUBLE_FAULT_STACK_SIZE] __attribute__((aligned(16)));

extern void gdt_flush(table_ptr_t *ptr);
extern void tss_flush(uint16_t selector);

static void gdt_set_entry(gdt_entry_t *e, uint8_t access, uint8_t granularity) {
    e->limit_low = 0;
    e->base_low = 0;
    e->base_mid = 0;
    e->access = access;
    e->granularity = granularity;
    e->base_high = 0;
}

static void tss_set_descriptor(tss_descriptor_t *d, uint64_t base, uint32_t limit) {
    d->limit_low = limit & 0xFFFF;
    d->base_low = base & 0xFFFF;
    d->base_mid = (base >> 16) & 0xFF;
    d->access = GDT_ACCESS_TSS;
    d->granularity = (limit >> 16) & 0x0F;
    d->base_high = (base >> 24) & 0xFF;
    d->base_upper = (uint32_t)(base >> 32);
    d->reserved = 0;
}

void gdt_init(void) {
    gdt_set_entry(&gdt.null, 0, 0);
    gdt_set_entry(&gdt.kernel_code, GDT_ACCESS_KERNEL_CODE, GDT_GRAN_LONG_MODE);
    gdt_set_entry(&gdt.kernel_data, GDT_ACCESS_KERNEL_DATA, 0);

    for (int i = 0; i < MAX_CPUS; i++) {
        for (uint64_t b = 0; b < sizeof(tss[i]); b++) {
            ((uint8_t *)&tss[i])[b] = 0;
        }
        tss[i].ist1 = (uint64_t)&double_fault_stack[i][DOUBLE_FAULT_STACK_SIZE];
        tss[i].iomap_base = sizeof(tss_t); /* no I/O bitmap: place it past the TSS limit */
        tss_set_descriptor(&gdt.tss[i], (uint64_t)&tss[i], sizeof(tss_t) - 1);
    }

    gdt_set_entry(&gdt.user_code, GDT_ACCESS_USER_CODE, GDT_GRAN_LONG_MODE);
    gdt_set_entry(&gdt.user_data, GDT_ACCESS_USER_DATA, 0);

    gdtp.limit = sizeof(gdt_table_t) - 1;
    gdtp.base = (uint64_t)&gdt;

    gdt_flush(&gdtp);
    tss_flush(gdt_tss_selector(0));
}

void gdt_init_ap(int cpu_id) {
    gdt_flush(&gdtp); /* per-CPU register (GDTR), same shared table gdt_init already built */
    tss_flush(gdt_tss_selector(cpu_id));
}

void tss_set_rsp0(int cpu_id, uint64_t rsp0) {
    tss[cpu_id].rsp0 = rsp0;
}

void gdt_get_table_ptr(uint16_t *limit_out, uint64_t *base_out) {
    *limit_out = gdtp.limit;
    *base_out = gdtp.base;
}
