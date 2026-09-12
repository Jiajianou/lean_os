/* tests/test_pci.c - M107
 *
 * Base Address Register decoding, against a modelled configuration space.
 *
 * Three drivers arrived in M107 whose registers are only reachable
 * through a memory BAR, and one of them - NVMe, under QEMU - is handed a
 * 64-bit BAR at 768 GiB. Everything this file checks is arithmetic whose
 * wrong answers are *plausible*: a 64-bit BAR read as two independent
 * 32-bit ones gives an address that is a real address, in the low 4 GiB,
 * belonging to something else. A driver would map it, write a doorbell
 * into it, and the machine would do something unrelated and wrong.
 *
 * The size probe is the other half. It writes all ones into a live BAR
 * and must put the original value back: a BAR left holding the probe
 * value is a device that has moved somewhere the rest of the system does
 * not know about, and on a machine with a working IOMMU that is a fault
 * at some later, unrelated moment.
 */
#include "check.h"
#include "fakes.h"

#include "drivers/pci.h"

/* An AHCI controller as QEMU's ich9-ahci presents one: a 32-bit memory
 * BAR at index 5. */
#define AHCI_CLASS 0x01
#define AHCI_SUB   0x06
#define AHCI_PROG  0x01

static void reset(void) {
    fake_pci_reset();
}

TEST(pci, a_class_scan_finds_a_device_no_vendor_scan_could) {
    reset();
    /* A controller nobody has a vendor id for: the point of scanning by
     * class is that the driver does not need one. */
    fake_pci_add(0, 3, 0, 0xDEAD, 0xBEEF, AHCI_CLASS, AHCI_SUB, AHCI_PROG);

    pci_device_t dev;
    CHECK_EQ(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 0, &dev), 1);
    CHECK_EQ(dev.bus, 0);
    CHECK_EQ(dev.slot, 3);
    CHECK_EQ(dev.vendor_id, 0xDEAD);
}

/* The prog-if is the field that separates an AHCI controller from the
 * same silicon in legacy IDE mode, and xHCI from EHCI. A scan that
 * ignored it would bind the wrong driver to the right chip. */
TEST(pci, the_prog_if_is_part_of_the_match) {
    reset();
    fake_pci_add(0, 1, 0, 0x8086, 0x2922, 0x01, 0x01, 0x80); /* IDE mode */
    fake_pci_add(0, 2, 0, 0x8086, 0x2922, 0x01, 0x06, 0x01); /* AHCI mode */

    pci_device_t dev;
    CHECK_EQ(pci_find_class(0x01, 0x06, 0x01, 0, &dev), 1);
    CHECK_EQ(dev.slot, 2);
    /* And nothing matches a prog-if that is not there. */
    CHECK_EQ(pci_find_class(0x01, 0x06, 0x02, 0, &dev), 0);
}

TEST(pci, prog_if_any_matches_whatever_is_there) {
    reset();
    fake_pci_add(0, 4, 0, 0x1B36, 0x000D, 0x0C, 0x03, 0x30); /* xHCI */

    pci_device_t dev;
    CHECK_EQ(pci_find_class(0x0C, 0x03, PCI_PROG_IF_ANY, 0, &dev), 1);
    CHECK_EQ(dev.slot, 4);
}

/* A machine with two controllers of the same class is ordinary, and one
 * of them having no disk on it is ordinary too. The index is what lets a
 * driver try the second. */
TEST(pci, the_index_walks_every_match_in_order) {
    reset();
    fake_pci_add(0, 2, 0, 0x1111, 0x1, AHCI_CLASS, AHCI_SUB, AHCI_PROG);
    fake_pci_add(0, 5, 0, 0x2222, 0x2, AHCI_CLASS, AHCI_SUB, AHCI_PROG);
    fake_pci_add(1, 0, 0, 0x3333, 0x3, AHCI_CLASS, AHCI_SUB, AHCI_PROG);

    pci_device_t dev;
    CHECK_EQ(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 0, &dev), 1);
    CHECK_EQ(dev.vendor_id, 0x1111);
    CHECK_EQ(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 1, &dev), 1);
    CHECK_EQ(dev.vendor_id, 0x2222);
    CHECK_EQ(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 2, &dev), 1);
    CHECK_EQ(dev.vendor_id, 0x3333);
    /* And the fourth call must say there is no fourth device rather than
     * hand back the third again - a driver loop ends on this. */
    CHECK_EQ(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 3, &dev), 0);
}

TEST(pci, a_machine_with_no_such_device_says_so) {
    reset();
    pci_device_t dev;
    CHECK_EQ(pci_find_class(0x01, 0x08, 0x02, 0, &dev), 0);
    CHECK_EQ(pci_find_device(0x1AF4, 0x1001, &dev), 0);
}

/* ---- BARs -------------------------------------------------------------- */

TEST(pci, a_32_bit_memory_bar_decodes_to_its_base) {
    reset();
    int h = fake_pci_add(0, 3, 0, 0x8086, 0x2922, AHCI_CLASS, AHCI_SUB, AHCI_PROG);
    /* 0xFEBF0000, memory, 32-bit, non-prefetchable. */
    fake_pci_set_bar(h, 5, 0xFEBF0000u, 0xFFFFF000u);

    pci_device_t dev;
    REQUIRE(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 0, &dev) == 1);
    CHECK_EQ(pci_bar_mem_base(&dev, 5), 0xFEBF0000ULL);
    CHECK_EQ(pci_bar_mem_size(&dev, 5), 0x1000ULL);
}

/* The case that page-faulted this kernel: a 64-bit BAR above 4 GiB. */
TEST(pci, a_64_bit_bar_joins_its_two_registers) {
    reset();
    int h = fake_pci_add(0, 4, 0, 0x1B36, 0x0010, 0x01, 0x08, 0x02);
    /* 0x000000C000000000, memory, 64-bit (type bits 10), so BAR0 holds
     * the low half with the type bits and BAR1 the high half. */
    fake_pci_set_bar(h, 0, 0x00000004u, 0xFFFFC000u);
    fake_pci_set_config(h, 0x14, 0x000000C0u);

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_bar_mem_base(&dev, 0), 0x000000C000000000ULL);
}

/* The upper half of a 64-bit BAR is not a BAR. A driver that walked every
 * index looking for a memory region would otherwise find a second one at
 * an address made of the top half of the first. */
TEST(pci, the_upper_half_of_a_64_bit_bar_is_not_a_bar) {
    reset();
    int h = fake_pci_add(0, 4, 0, 0x1B36, 0x0010, 0x01, 0x08, 0x02);
    fake_pci_set_bar(h, 0, 0x00000004u, 0xFFFFC000u);
    fake_pci_set_config(h, 0x14, 0x000000C0u);

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_bar_mem_base(&dev, 1), 0ULL);
    CHECK_EQ(pci_bar_mem_size(&dev, 1), 0ULL);
}

/* An I/O BAR must not come back as a memory address. This is the mirror
 * of the check pci_bar0_io_base has made since Q16, and the reason is the
 * same: a driver that got a plausible number here would map a page that
 * is not a device. */
TEST(pci, an_io_bar_is_not_a_memory_bar) {
    reset();
    int h = fake_pci_add(0, 6, 0, 0x10EC, 0x8139, 0x02, 0x00, 0x00);
    fake_pci_set_bar(h, 0, 0x0000C501u, 0xFFFFFF01u); /* bit 0 set: I/O */

    pci_device_t dev;
    REQUIRE(pci_find_device(0x10EC, 0x8139, &dev) == 1);
    CHECK_EQ(pci_bar_mem_base(&dev, 0), 0ULL);
    CHECK_EQ(pci_bar_mem_size(&dev, 0), 0ULL);
    /* ...and the I/O accessor still reads it, with the flag bits off. */
    CHECK_EQ(pci_bar0_io_base(&dev), 0xC500);
}

TEST(pci, an_unimplemented_bar_decodes_to_nothing) {
    reset();
    int h = fake_pci_add(0, 7, 0, 0x1234, 0x5678, 0x01, 0x06, 0x01);
    (void)h;
    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x06, 0x01, 0, &dev) == 1);
    CHECK_EQ(pci_bar_mem_base(&dev, 2), 0ULL);
    CHECK_EQ(pci_bar_mem_size(&dev, 2), 0ULL);
}

TEST(pci, an_out_of_range_bar_index_is_refused) {
    reset();
    fake_pci_add(0, 7, 0, 0x1234, 0x5678, 0x01, 0x06, 0x01);
    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x06, 0x01, 0, &dev) == 1);
    CHECK_EQ(pci_bar_mem_base(&dev, 6), 0ULL);
    CHECK_EQ(pci_bar_mem_base(&dev, 200), 0ULL);
}

/* The probe must leave the BAR exactly as it found it. */
TEST(pci, sizing_a_bar_restores_its_value) {
    reset();
    int h = fake_pci_add(0, 3, 0, 0x8086, 0x2922, AHCI_CLASS, AHCI_SUB, AHCI_PROG);
    fake_pci_set_bar(h, 5, 0xFEBF0000u, 0xFFFF0000u);

    pci_device_t dev;
    REQUIRE(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 0, &dev) == 1);
    CHECK_EQ(pci_bar_mem_size(&dev, 5), 0x10000ULL);
    CHECK_EQ(fake_pci_get_config(h, 0x14 + 4 * 4), 0xFEBF0000u);
    /* And reading the base afterwards still works, which is the thing a
     * driver actually does next. */
    CHECK_EQ(pci_bar_mem_base(&dev, 5), 0xFEBF0000ULL);
}

TEST(pci, sizing_a_64_bit_bar_restores_both_registers) {
    reset();
    int h = fake_pci_add(0, 4, 0, 0x1B36, 0x0010, 0x01, 0x08, 0x02);
    fake_pci_set_bar(h, 0, 0x00000004u, 0xFFFFC000u);
    fake_pci_set_config(h, 0x14, 0x000000C0u);

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_bar_mem_size(&dev, 0), 0x4000ULL);
    CHECK_EQ(fake_pci_get_config(h, 0x10), 0x00000004u);
    CHECK_EQ(fake_pci_get_config(h, 0x14), 0x000000C0u);
    CHECK_EQ(pci_bar_mem_base(&dev, 0), 0x000000C000000000ULL);
}

/* ---- the capability list ----------------------------------------------- */

TEST(pci, a_device_with_no_capability_list_has_no_capabilities) {
    reset();
    int h = fake_pci_add(0, 3, 0, 0x8086, 0x2922, AHCI_CLASS, AHCI_SUB, AHCI_PROG);
    (void)h;
    pci_device_t dev;
    REQUIRE(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 0, &dev) == 1);
    CHECK_EQ(pci_find_capability(&dev, PCI_CAP_ID_MSIX), 0);
}

TEST(pci, the_capability_list_is_walked_to_the_end) {
    reset();
    int h = fake_pci_add(0, 4, 0, 0x1B36, 0x0010, 0x01, 0x08, 0x02);
    fake_pci_set_config(h, 0x04, (1u << 4) << 16); /* status: capability list */
    fake_pci_set_config(h, 0x34, 0x40);            /* first capability at 0x40 */
    /* A capability dword is: id in byte 0, the next pointer in byte 1.
     * Writing the next pointer into byte 2 - which is where it reads as
     * if it belonged - makes every list one entry long, and the first
     * draft of this test did exactly that and blamed the code. */
    fake_pci_set_config(h, 0x40, 0x00005001u);     /* id 0x01, next 0x50 */
    fake_pci_set_config(h, 0x50, 0x00006005u);     /* id 0x05 (MSI), next 0x60 */
    fake_pci_set_config(h, 0x60, 0x00000011u);     /* id 0x11 (MSI-X), end */

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_find_capability(&dev, PCI_CAP_ID_MSI), 0x50);
    CHECK_EQ(pci_find_capability(&dev, PCI_CAP_ID_MSIX), 0x60);
    CHECK_EQ(pci_find_capability(&dev, 0x01), 0x40);
    CHECK_EQ(pci_find_capability(&dev, 0x99), 0);
}

/* Firmware writes broken capability lists. One that points at itself must
 * end the walk rather than the boot - which is the failure mode this
 * bound exists for, and which no machine here can be made to reproduce. */
TEST(pci, a_capability_list_that_loops_terminates) {
    reset();
    int h = fake_pci_add(0, 4, 0, 0x1B36, 0x0010, 0x01, 0x08, 0x02);
    fake_pci_set_config(h, 0x04, (1u << 4) << 16);
    fake_pci_set_config(h, 0x34, 0x40);
    fake_pci_set_config(h, 0x40, 0x00004009u); /* id 0x09, next 0x40 - itself */

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_find_capability(&dev, PCI_CAP_ID_MSIX), 0);
    CHECK_EQ(pci_find_capability(&dev, 0x09), 0x40);
}

/* A capability pointer into the first 64 bytes is into the standard
 * header, which is not a capability - and following it would read a BAR
 * as a capability id. */
TEST(pci, a_capability_pointer_into_the_header_is_refused) {
    reset();
    int h = fake_pci_add(0, 4, 0, 0x1B36, 0x0010, 0x01, 0x08, 0x02);
    fake_pci_set_config(h, 0x04, (1u << 4) << 16);
    fake_pci_set_config(h, 0x34, 0x10); /* "the capability list starts at BAR0" */

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_find_capability(&dev, PCI_CAP_ID_MSIX), 0);
}
