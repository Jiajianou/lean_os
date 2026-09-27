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

typedef struct {
    int count;
    pci_device_t seen[4];
} visit_log_t;

static void remember(const pci_device_t *device, void *context) {
    visit_log_t *log = (visit_log_t *)context;
    if (log->count < 4) {
        log->seen[log->count] = *device;
    }
    log->count++;
}

TEST(pci, enumeration_visits_every_function_in_order_with_its_class) {
    reset();
    fake_pci_add(1, 0, 0, 0x3333, 0x0003, 0x0C, 0x03, 0x30);
    fake_pci_add(0, 5, 2, 0x2222, 0x0002, 0x02, 0x00, 0x00);
    fake_pci_add(0, 2, 0, 0x1111, 0x0001, AHCI_CLASS, AHCI_SUB, AHCI_PROG);

    visit_log_t log = {0};
    pci_enumerate(remember, &log);
    REQUIRE(log.count == 3);
    CHECK_EQ(log.seen[0].vendor_id, 0x1111);
    CHECK_EQ(log.seen[0].class_code, AHCI_CLASS);
    CHECK_EQ(log.seen[0].subclass, AHCI_SUB);
    CHECK_EQ(log.seen[0].prog_if, AHCI_PROG);
    CHECK_EQ(log.seen[1].vendor_id, 0x2222);
    CHECK_EQ(log.seen[1].slot, 5);
    CHECK_EQ(log.seen[1].func, 2);
    CHECK_EQ(log.seen[2].vendor_id, 0x3333);
    CHECK_EQ(log.seen[2].bus, 1);
    CHECK_EQ(log.seen[2].device_id, 0x0003);
    CHECK_EQ(log.seen[2].prog_if, 0x30);
}

TEST(pci, enumeration_of_an_empty_bus_visits_nothing) {
    reset();
    visit_log_t log = {0};
    pci_enumerate(remember, &log);
    CHECK_EQ(log.count, 0);
}

TEST(pci, a_device_in_d3_is_brought_to_d0_and_keeps_its_other_bits) {
    reset();
    int h = fake_pci_add(0, 21, 0, 0x8086, 0xA0E8, 0x0C, 0x80, 0x00);
    fake_pci_set_config(h, 0x04, (1u << 4) << 16);
    fake_pci_set_config(h, 0x34, 0x40);
    fake_pci_set_config(h, 0x40, 0x00000001u);
    fake_pci_set_config(h, 0x44, 0x00000103u);

    pci_device_t dev;
    REQUIRE(pci_find_device(0x8086, 0xA0E8, &dev) == 1);
    pci_set_power_state_d0(&dev);
    CHECK_EQ(fake_pci_get_config(h, 0x44), 0x00000100u);
}

TEST(pci, a_device_without_power_management_is_left_alone) {
    reset();
    int h = fake_pci_add(0, 21, 0, 0x8086, 0xA0E8, 0x0C, 0x80, 0x00);
    fake_pci_set_config(h, 0x44, 0x00000003u);

    pci_device_t dev;
    REQUIRE(pci_find_device(0x8086, 0xA0E8, &dev) == 1);
    pci_set_power_state_d0(&dev);
    CHECK_EQ(fake_pci_get_config(h, 0x44), 0x00000003u);
}

static int add_unplaced_i2c(void) {
    int h = fake_pci_add(0, 0x15, 0, 0x8086, 0xA0E8, 0x0C, 0x80, 0x00);
    fake_pci_set_bar(h, 0, 0x00000004u, 0xFFFFF000u);
    return h;
}

static int add_graphics_aperture(void) {
    int h = fake_pci_add(0, 2, 0, 0x8086, 0x9A49, 0x03, 0x00, 0x00);
    fake_pci_set_bar(h, 0, 0x0000000Cu, 0xF000000Cu);
    fake_pci_set_config(h, 0x14, 0x00000040u);
    fake_pci_set_config(h, 0x04, 0x00000007u);
    return h;
}

TEST(pci, an_unplaced_bar_goes_just_past_the_highest_one_already_placed) {
    reset();
    add_graphics_aperture();
    int i2c = add_unplaced_i2c();

    pci_device_t dev;
    REQUIRE(pci_find_device(0x8086, 0xA0E8, &dev) == 1);
    CHECK_EQ(pci_bar_memory_base(&dev, 0), 0ULL);
    CHECK_EQ(pci_assign_memory_bar(&dev, 0, 1ULL << 39), 0x4010000000ULL);
    CHECK_EQ(pci_bar_memory_base(&dev, 0), 0x4010000000ULL);
    CHECK_EQ(fake_pci_get_config(i2c, 0x10), 0x10000004u);
    CHECK_EQ(fake_pci_get_config(i2c, 0x14), 0x00000040u);
}

TEST(pci, a_bridge_window_above_every_bar_is_respected) {
    reset();
    add_graphics_aperture();
    int bridge = fake_pci_add(0, 7, 0, 0x8086, 0x9A25, 0x06, 0x04, 0x00);
    fake_pci_set_config(bridge, 0x0C, 0x00010000u);
    fake_pci_set_config(bridge, 0x20, 0x0000FFF0u);
    fake_pci_set_config(bridge, 0x24, 0x1BF10001u);
    fake_pci_set_config(bridge, 0x28, 0x00000060u);
    fake_pci_set_config(bridge, 0x2C, 0x00000060u);
    add_unplaced_i2c();

    pci_device_t dev;
    REQUIRE(pci_find_device(0x8086, 0xA0E8, &dev) == 1);
    CHECK_EQ(pci_assign_memory_bar(&dev, 0, 1ULL << 39), 0x601C000000ULL);
}

TEST(pci, a_closed_bridge_window_claims_nothing) {
    reset();
    add_graphics_aperture();
    int bridge = fake_pci_add(0, 7, 0, 0x8086, 0x9A25, 0x06, 0x04, 0x00);
    fake_pci_set_config(bridge, 0x0C, 0x00010000u);
    fake_pci_set_config(bridge, 0x20, 0x0000FFF0u);
    fake_pci_set_config(bridge, 0x24, 0x0001FFF1u);
    fake_pci_set_config(bridge, 0x28, 0x000000FFu);
    fake_pci_set_config(bridge, 0x2C, 0x00000000u);
    add_unplaced_i2c();

    pci_device_t dev;
    REQUIRE(pci_find_device(0x8086, 0xA0E8, &dev) == 1);
    CHECK_EQ(pci_assign_memory_bar(&dev, 0, 1ULL << 39), 0x4010000000ULL);
}

TEST(pci, no_bar_above_four_gigabytes_means_no_window_to_guess_at) {
    reset();
    int ahci = fake_pci_add(0, 3, 0, 0x8086, 0x2922, AHCI_CLASS, AHCI_SUB, AHCI_PROG);
    fake_pci_set_bar(ahci, 5, 0xFEBF0000u, 0xFFFFF000u);
    int i2c = add_unplaced_i2c();

    pci_device_t dev;
    REQUIRE(pci_find_device(0x8086, 0xA0E8, &dev) == 1);
    CHECK_EQ(pci_assign_memory_bar(&dev, 0, 1ULL << 39), 0ULL);
    CHECK_EQ(fake_pci_get_config(i2c, 0x10), 0x00000004u);
}

TEST(pci, a_placement_past_the_address_limit_is_refused) {
    reset();
    add_graphics_aperture();
    int i2c = add_unplaced_i2c();

    pci_device_t dev;
    REQUIRE(pci_find_device(0x8086, 0xA0E8, &dev) == 1);
    CHECK_EQ(pci_assign_memory_bar(&dev, 0, 0x4010000800ULL), 0ULL);
    CHECK_EQ(fake_pci_get_config(i2c, 0x10), 0x00000004u);
}

TEST(pci, a_bar_already_placed_is_left_where_it_is) {
    reset();
    add_graphics_aperture();
    int i2c = add_unplaced_i2c();
    fake_pci_set_config(i2c, 0x10, 0xFE010004u);

    pci_device_t dev;
    REQUIRE(pci_find_device(0x8086, 0xA0E8, &dev) == 1);
    CHECK_EQ(pci_assign_memory_bar(&dev, 0, 1ULL << 39), 0xFE010000ULL);
    CHECK_EQ(fake_pci_get_config(i2c, 0x10), 0xFE010004u);
}

TEST(pci, the_scan_gives_every_device_its_decode_back) {
    reset();
    int graphics = add_graphics_aperture();
    add_unplaced_i2c();

    pci_device_t dev;
    REQUIRE(pci_find_device(0x8086, 0xA0E8, &dev) == 1);
    pci_assign_memory_bar(&dev, 0, 1ULL << 39);
    CHECK_EQ(fake_pci_get_config(graphics, 0x04), 0x00000007u);
    CHECK_EQ(fake_pci_get_config(graphics, 0x10), 0x0000000Cu);
    CHECK_EQ(fake_pci_get_config(graphics, 0x14), 0x00000040u);
}
