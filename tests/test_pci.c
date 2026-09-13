#include "check.h"
#include "fakes.h"

#include "drivers/pci.h"

#define AHCI_CLASS 0x01
#define AHCI_SUB   0x06
#define AHCI_PROG  0x01

static void reset(void) {
    fake_pci_reset();
}

TEST(pci, a_class_scan_finds_a_device_no_vendor_scan_could) {
    reset();
    fake_pci_add(0, 3, 0, 0xDEAD, 0xBEEF, AHCI_CLASS, AHCI_SUB, AHCI_PROG);

    pci_device_t dev;
    CHECK_EQ(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 0, &dev), 1);
    CHECK_EQ(dev.bus, 0);
    CHECK_EQ(dev.slot, 3);
    CHECK_EQ(dev.vendor_id, 0xDEAD);
}

TEST(pci, the_prog_if_is_part_of_the_match) {
    reset();
    fake_pci_add(0, 1, 0, 0x8086, 0x2922, 0x01, 0x01, 0x80);
    fake_pci_add(0, 2, 0, 0x8086, 0x2922, 0x01, 0x06, 0x01);

    pci_device_t dev;
    CHECK_EQ(pci_find_class(0x01, 0x06, 0x01, 0, &dev), 1);
    CHECK_EQ(dev.slot, 2);
    CHECK_EQ(pci_find_class(0x01, 0x06, 0x02, 0, &dev), 0);
}

TEST(pci, prog_if_any_matches_whatever_is_there) {
    reset();
    fake_pci_add(0, 4, 0, 0x1B36, 0x000D, 0x0C, 0x03, 0x30);

    pci_device_t dev;
    CHECK_EQ(pci_find_class(0x0C, 0x03, PCI_PROG_IF_ANY, 0, &dev), 1);
    CHECK_EQ(dev.slot, 4);
}

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
    CHECK_EQ(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 3, &dev), 0);
}

TEST(pci, a_machine_with_no_such_device_says_so) {
    reset();
    pci_device_t dev;
    CHECK_EQ(pci_find_class(0x01, 0x08, 0x02, 0, &dev), 0);
    CHECK_EQ(pci_find_device(0x1AF4, 0x1001, &dev), 0);
}

TEST(pci, a_32_bit_memory_bar_decodes_to_its_base) {
    reset();
    int h = fake_pci_add(0, 3, 0, 0x8086, 0x2922, AHCI_CLASS, AHCI_SUB, AHCI_PROG);
    fake_pci_set_bar(h, 5, 0xFEBF0000u, 0xFFFFF000u);

    pci_device_t dev;
    REQUIRE(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 0, &dev) == 1);
    CHECK_EQ(pci_bar_memory_base(&dev, 5), 0xFEBF0000ULL);
    CHECK_EQ(pci_bar_memory_size(&dev, 5), 0x1000ULL);
}

TEST(pci, a_64_bit_bar_joins_its_two_registers) {
    reset();
    int h = fake_pci_add(0, 4, 0, 0x1B36, 0x0010, 0x01, 0x08, 0x02);
    fake_pci_set_bar(h, 0, 0x00000004u, 0xFFFFC000u);
    fake_pci_set_config(h, 0x14, 0x000000C0u);

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_bar_memory_base(&dev, 0), 0x000000C000000000ULL);
}

TEST(pci, the_upper_half_of_a_64_bit_bar_is_not_a_bar) {
    reset();
    int h = fake_pci_add(0, 4, 0, 0x1B36, 0x0010, 0x01, 0x08, 0x02);
    fake_pci_set_bar(h, 0, 0x00000004u, 0xFFFFC000u);
    fake_pci_set_config(h, 0x14, 0x000000C0u);

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_bar_memory_base(&dev, 1), 0ULL);
    CHECK_EQ(pci_bar_memory_size(&dev, 1), 0ULL);
}

TEST(pci, an_io_bar_is_not_a_memory_bar) {
    reset();
    int h = fake_pci_add(0, 6, 0, 0x10EC, 0x8139, 0x02, 0x00, 0x00);
    fake_pci_set_bar(h, 0, 0x0000C501u, 0xFFFFFF01u);

    pci_device_t dev;
    REQUIRE(pci_find_device(0x10EC, 0x8139, &dev) == 1);
    CHECK_EQ(pci_bar_memory_base(&dev, 0), 0ULL);
    CHECK_EQ(pci_bar_memory_size(&dev, 0), 0ULL);
    CHECK_EQ(pci_bar0_io_base(&dev), 0xC500);
}

TEST(pci, an_unimplemented_bar_decodes_to_nothing) {
    reset();
    int h = fake_pci_add(0, 7, 0, 0x1234, 0x5678, 0x01, 0x06, 0x01);
    (void)h;
    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x06, 0x01, 0, &dev) == 1);
    CHECK_EQ(pci_bar_memory_base(&dev, 2), 0ULL);
    CHECK_EQ(pci_bar_memory_size(&dev, 2), 0ULL);
}

TEST(pci, an_out_of_range_bar_index_is_refused) {
    reset();
    fake_pci_add(0, 7, 0, 0x1234, 0x5678, 0x01, 0x06, 0x01);
    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x06, 0x01, 0, &dev) == 1);
    CHECK_EQ(pci_bar_memory_base(&dev, 6), 0ULL);
    CHECK_EQ(pci_bar_memory_base(&dev, 200), 0ULL);
}

TEST(pci, sizing_a_bar_restores_its_value) {
    reset();
    int h = fake_pci_add(0, 3, 0, 0x8086, 0x2922, AHCI_CLASS, AHCI_SUB, AHCI_PROG);
    fake_pci_set_bar(h, 5, 0xFEBF0000u, 0xFFFF0000u);

    pci_device_t dev;
    REQUIRE(pci_find_class(AHCI_CLASS, AHCI_SUB, AHCI_PROG, 0, &dev) == 1);
    CHECK_EQ(pci_bar_memory_size(&dev, 5), 0x10000ULL);
    CHECK_EQ(fake_pci_get_config(h, 0x14 + 4 * 4), 0xFEBF0000u);
    CHECK_EQ(pci_bar_memory_base(&dev, 5), 0xFEBF0000ULL);
}

TEST(pci, sizing_a_64_bit_bar_restores_both_registers) {
    reset();
    int h = fake_pci_add(0, 4, 0, 0x1B36, 0x0010, 0x01, 0x08, 0x02);
    fake_pci_set_bar(h, 0, 0x00000004u, 0xFFFFC000u);
    fake_pci_set_config(h, 0x14, 0x000000C0u);

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_bar_memory_size(&dev, 0), 0x4000ULL);
    CHECK_EQ(fake_pci_get_config(h, 0x10), 0x00000004u);
    CHECK_EQ(fake_pci_get_config(h, 0x14), 0x000000C0u);
    CHECK_EQ(pci_bar_memory_base(&dev, 0), 0x000000C000000000ULL);
}

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
    fake_pci_set_config(h, 0x04, (1u << 4) << 16);
    fake_pci_set_config(h, 0x34, 0x40);
    fake_pci_set_config(h, 0x40, 0x00005001u);
    fake_pci_set_config(h, 0x50, 0x00006005u);
    fake_pci_set_config(h, 0x60, 0x00000011u);

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_find_capability(&dev, PCI_CAP_ID_MSI), 0x50);
    CHECK_EQ(pci_find_capability(&dev, PCI_CAP_ID_MSIX), 0x60);
    CHECK_EQ(pci_find_capability(&dev, 0x01), 0x40);
    CHECK_EQ(pci_find_capability(&dev, 0x99), 0);
}

TEST(pci, a_capability_list_that_loops_terminates) {
    reset();
    int h = fake_pci_add(0, 4, 0, 0x1B36, 0x0010, 0x01, 0x08, 0x02);
    fake_pci_set_config(h, 0x04, (1u << 4) << 16);
    fake_pci_set_config(h, 0x34, 0x40);
    fake_pci_set_config(h, 0x40, 0x00004009u);

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_find_capability(&dev, PCI_CAP_ID_MSIX), 0);
    CHECK_EQ(pci_find_capability(&dev, 0x09), 0x40);
}

TEST(pci, a_capability_pointer_into_the_header_is_refused) {
    reset();
    int h = fake_pci_add(0, 4, 0, 0x1B36, 0x0010, 0x01, 0x08, 0x02);
    fake_pci_set_config(h, 0x04, (1u << 4) << 16);
    fake_pci_set_config(h, 0x34, 0x10);

    pci_device_t dev;
    REQUIRE(pci_find_class(0x01, 0x08, 0x02, 0, &dev) == 1);
    CHECK_EQ(pci_find_capability(&dev, PCI_CAP_ID_MSIX), 0);
}
