#include "intel_wireless_transport.h"

#include "drivers/intel_wireless_registers.h"
#include "memory_management/physical_memory.h"
#ifndef LEANOS_HOST_TEST
#include "architecture/x86_64/timestamp_counter.h"
#include "scheduler/scheduler.h"
#endif

#define HBUS_TARG_MEM_RADDR 0x40C
#define HBUS_TARG_MEM_RDAT  0x41C

#define POLL_INTERVAL_US 10
#define PAGE_BYTES 4096u

#define RX_MASK (INTEL_WIRELESS_RX_RING - 1)

typedef struct {
    volatile uint8_t *registers;

    intel_wireless_context_info_t *context;
    uint64_t context_physical;

    uint64_t *free_ring;
    uint32_t *used_ring;
    intel_wireless_rb_status_t *rb_status;
    uint8_t *rx_buffer[INTEL_WIRELESS_RX_RING];
    uint16_t spare_ids[INTEL_WIRELESS_RX_RING];
    uint32_t spare_count;
    uint32_t rx_read;
    uint32_t rx_write;
    int rx_stocked;

    intel_wireless_tfd_t *tfds;
    uint8_t *command_buffer[INTEL_WIRELESS_COMMAND_SLOTS];
    uint32_t command_write;
    uint32_t command_read;
    int waiting_index;
    int waiting_answered;
    uint8_t *waiting_response;
    uint32_t waiting_capacity;
    uint32_t waiting_length;

    uint8_t *lmac_copy[INTEL_WIRELESS_MAX_SECTIONS];
    uint32_t lmac_pages[INTEL_WIRELESS_MAX_SECTIONS];
    uint32_t lmac_count;
    uint8_t *umac_copy[INTEL_WIRELESS_MAX_SECTIONS];
    uint32_t umac_pages[INTEL_WIRELESS_MAX_SECTIONS];
    uint32_t umac_count;
    uint8_t *paging_copy[INTEL_WIRELESS_MAX_SECTIONS];
    uint32_t paging_pages[INTEL_WIRELESS_MAX_SECTIONS];
    uint32_t paging_count;

    intel_wireless_tfd_t *data_tfds;
    uint16_t *byte_counts;
    uint8_t *data_buffer[INTEL_WIRELESS_DATA_SLOTS];
    int data_queue;
    uint32_t data_write;
    uint32_t data_read;

    uint64_t next_cause_check;

    int error_seen;
    intel_wireless_packet_handler_t handler;
    intel_wireless_transport_statistics_t statistics;
} transport_t;

static transport_t transport = {.data_queue = -1};

#ifdef LEANOS_HOST_TEST
/* The device is a model in the host tests; the registers are calls into it,
   and so is time, so that a poll loop waiting on the firmware lets the model
   take its next step instead of spinning on a value nothing will change. */
uint32_t intel_wireless_host_read32(uint32_t offset);
void intel_wireless_host_write32(uint32_t offset, uint32_t value);
void intel_wireless_host_delay(uint32_t microseconds);

uint32_t intel_wireless_read32(uint32_t offset) {
    return intel_wireless_host_read32(offset);
}

void intel_wireless_write32(uint32_t offset, uint32_t value) {
    intel_wireless_host_write32(offset, value);
}

void intel_wireless_delay_us(uint32_t microseconds) {
    intel_wireless_host_delay(microseconds);
}

void intel_wireless_sleep_ms(uint32_t milliseconds) {
    intel_wireless_host_delay(milliseconds * 1000u);
}
#else
uint32_t intel_wireless_read32(uint32_t offset) {
    return *(volatile uint32_t *)(transport.registers + offset);
}

void intel_wireless_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(transport.registers + offset) = value;
}

void intel_wireless_delay_us(uint32_t microseconds) {
    uint64_t start = tsc_read();
    while (tsc_to_us(tsc_read() - start) < microseconds) {
        __asm__ volatile("pause");
    }
}

/* Waits that are long enough to give the processor away do: the driver's
   task is the only thing waiting on the radio, and nothing else should pay
   for a firmware taking its time. */
void intel_wireless_sleep_ms(uint32_t milliseconds) {
    scheduler_sleep_ms(milliseconds);
}
#endif

static void write8(uint32_t offset, uint8_t value) {
    uint32_t aligned = offset & ~3u;
    uint32_t shift = (offset & 3u) * 8;
    uint32_t word = intel_wireless_read32(aligned);
    word = (word & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    intel_wireless_write32(aligned, word);
}

static void set_bits(uint32_t offset, uint32_t bits) {
    intel_wireless_write32(offset, intel_wireless_read32(offset) | bits);
}

static void clear_bits(uint32_t offset, uint32_t bits) {
    intel_wireless_write32(offset, intel_wireless_read32(offset) & ~bits);
}

static int poll_bits(uint32_t offset, uint32_t bits, uint32_t mask, uint32_t timeout_us) {
    uint32_t waited = 0;
    do {
        if ((intel_wireless_read32(offset) & mask) == (bits & mask)) {
            return 1;
        }
        intel_wireless_delay_us(POLL_INTERVAL_US);
        waited += POLL_INTERVAL_US;
    } while (waited < timeout_us);
    return 0;
}

static int is_error_value(uint32_t value) {
    return (value & ~0xFu) == 0xA5A5A5A0u || (value & ~0xFu) == 0x5A5A5A50u;
}

void intel_wireless_transport_attach(volatile uint8_t *registers) {
    transport.registers = registers;
}

static uint32_t read_periphery_no_grab(uint32_t address) {
    intel_wireless_write32(HBUS_TARG_PRPH_RADDR, (address & PERIPHERY_ADDRESS_MASK) | (3u << 24));
    return intel_wireless_read32(HBUS_TARG_PRPH_RDAT);
}

static void write_periphery_no_grab(uint32_t address, uint32_t value) {
    intel_wireless_write32(HBUS_TARG_PRPH_WADDR, (address & PERIPHERY_ADDRESS_MASK) | (3u << 24));
    intel_wireless_write32(HBUS_TARG_PRPH_WDAT, value);
}

/* The periphery is behind the device's own clock domain, which sleeps; a
   request wakes it and the clock-ready bit says when it has. */
static int grab_access(void) {
    set_bits(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
    intel_wireless_delay_us(2);
    return poll_bits(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                     CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY | CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP, 15000);
}

static void release_access(void) {
    clear_bits(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}

uint32_t intel_wireless_read_periphery(uint32_t address) {
    if (!grab_access()) {
        release_access();
        return 0x5A5A5A5Au;
    }
    uint32_t value = read_periphery_no_grab(address);
    release_access();
    return value;
}

void intel_wireless_write_periphery(uint32_t address, uint32_t value) {
    if (!grab_access()) {
        release_access();
        return;
    }
    write_periphery_no_grab(address, value);
    release_access();
}

static void set_periphery_bits(uint32_t address, uint32_t bits) {
    if (grab_access()) {
        write_periphery_no_grab(address, read_periphery_no_grab(address) | bits);
    }
    release_access();
}

static void clear_periphery_bits(uint32_t address, uint32_t bits) {
    if (grab_access()) {
        write_periphery_no_grab(address, read_periphery_no_grab(address) & ~bits);
    }
    release_access();
}

int intel_wireless_read_device_memory(uint32_t address, uint32_t *out, uint32_t count) {
    if (!grab_access()) {
        release_access();
        return 0;
    }
    intel_wireless_write32(HBUS_TARG_MEM_RADDR, address);
    for (uint32_t i = 0; i < count; i++) {
        out[i] = intel_wireless_read32(HBUS_TARG_MEM_RDAT);
    }
    release_access();
    return 1;
}

static int set_hardware_ready(void) {
    set_bits(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_PCI_OWN_SET);
    int ready = poll_bits(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_PCI_OWN_SET, CSR_HW_IF_CONFIG_REG_PCI_OWN_SET,
                          50);
    if (ready) {
        set_bits(CSR_MBOX_SET_REG, CSR_MBOX_SET_REG_OS_ALIVE);
    }
    return ready;
}

/* The device is shared with the platform's management engine, which may be
   holding it; asking for ownership and being told yes is how this side knows
   it may go on. */
int intel_wireless_prepare_card(void) {
    if (set_hardware_ready()) {
        return INTEL_WIRELESS_OK;
    }
    set_bits(CSR_DBG_LINK_PWR_MGMT_REG, CSR_RESET_LINK_PWR_MGMT_DISABLED);
    intel_wireless_delay_us(1000);
    for (int attempt = 0; attempt < 10; attempt++) {
        set_bits(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_WAKE_ME);
        for (uint32_t waited = 0; waited < 150000; waited += 200) {
            if (set_hardware_ready()) {
                return INTEL_WIRELESS_OK;
            }
            intel_wireless_delay_us(200);
        }
        intel_wireless_sleep_ms(25);
    }
    return INTEL_WIRELESS_NOT_READY;
}

static int software_reset(void) {
    set_bits(CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET);
    intel_wireless_sleep_ms(6);
    return intel_wireless_prepare_card();
}

static int activate(void) {
    set_bits(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
    return poll_bits(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                     25000)
               ? INTEL_WIRELESS_OK
               : INTEL_WIRELESS_NO_CLOCK;
}

static int power_management_init(void) {
    set_bits(CSR_GIO_CHICKEN_BITS, CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);
    set_bits(CSR_DBG_HPET_MEM_REG, CSR_DBG_HPET_MEM_REG_VAL);
    set_bits(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_HAP_WAKE);
    set_bits(CSR_GIO_REG, CSR_GIO_REG_VAL_L0S_DISABLED);
    return activate();
}

static void disable_interrupts(void) {
    intel_wireless_write32(CSR_INT_MASK, 0);
    intel_wireless_write32(CSR_INT, 0xFFFFFFFFu);
    intel_wireless_write32(CSR_FH_INT_STATUS, 0xFFFFFFFFu);
}

int intel_wireless_radio_switch_off(void) {
    return !(intel_wireless_read32(CSR_GP_CNTRL) & CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW);
}

/* What Linux calls starting the hardware: own the device, reset it, and on
   an integrated part force a power-gating cycle the boot ROM expects before
   it will load anything. */
int intel_wireless_start_hardware(void) {
    int result = intel_wireless_prepare_card();
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }

    uint32_t hpm = read_periphery_no_grab(HPM_DEBUG);
    if (!is_error_value(hpm) && (hpm & HPM_PERSISTENCE_BIT)) {
        uint32_t protection = read_periphery_no_grab(PREG_PRPH_WPROT_22000);
        if (protection & PREG_WFPM_ACCESS) {
            return INTEL_WIRELESS_PERSISTENCE;
        }
        write_periphery_no_grab(HPM_DEBUG, hpm & ~HPM_PERSISTENCE_BIT);
    }

    result = software_reset();
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }

    result = activate();
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    set_periphery_bits(HPM_HIPM_GEN_CFG, HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE);
    intel_wireless_delay_us(20);
    set_periphery_bits(HPM_HIPM_GEN_CFG, HPM_HIPM_GEN_CFG_CR_PG_EN | HPM_HIPM_GEN_CFG_CR_SLP_EN);
    intel_wireless_delay_us(20);
    clear_periphery_bits(HPM_HIPM_GEN_CFG, HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE);
    result = software_reset();
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }

    result = power_management_init();
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    intel_wireless_write32(CSR_INT_MASK, CSR_INT_BIT_RF_KILL);
    return INTEL_WIRELESS_OK;
}

static void *dma_pages(uint32_t pages) {
    uint64_t physical = physical_memory_try_alloc_contiguous(pages);
    if (!physical) {
        return 0;
    }
    uint8_t *bytes = (uint8_t *)(uintptr_t)physical;
    for (uint32_t i = 0; i < pages * PAGE_BYTES; i++) {
        bytes[i] = 0;
    }
    return bytes;
}

static void free_pages(void *pointer, uint32_t pages) {
    if (pointer) {
        physical_memory_free_contiguous((uint64_t)(uintptr_t)pointer, pages);
    }
}

static uint64_t physical(const void *pointer) {
    return (uint64_t)(uintptr_t)pointer;
}

static uint32_t pages_for(uint32_t bytes) {
    return (bytes + PAGE_BYTES - 1) / PAGE_BYTES;
}

static int rx_allocate(void) {
    if (!transport.free_ring) {
        transport.free_ring = dma_pages(pages_for(INTEL_WIRELESS_RX_RING * sizeof(uint64_t)));
        transport.used_ring = dma_pages(pages_for(INTEL_WIRELESS_RX_RING * sizeof(uint32_t)));
        transport.rb_status = dma_pages(1);
        if (!transport.free_ring || !transport.used_ring || !transport.rb_status) {
            return INTEL_WIRELESS_NO_MEMORY;
        }
        for (uint32_t i = 0; i < INTEL_WIRELESS_RX_RING; i++) {
            transport.rx_buffer[i] = dma_pages(INTEL_WIRELESS_RX_BUFFER / PAGE_BYTES);
            if (!transport.rx_buffer[i]) {
                return INTEL_WIRELESS_NO_MEMORY;
            }
        }
    }
    for (uint32_t i = 0; i < INTEL_WIRELESS_RX_RING; i++) {
        transport.free_ring[i] = 0;
        transport.used_ring[i] = 0;
        transport.spare_ids[i] = (uint16_t)(INTEL_WIRELESS_RX_RING - i);
    }
    transport.spare_count = INTEL_WIRELESS_RX_RING;
    transport.rb_status->closed_rb_number = 0;
    transport.rx_read = 0;
    transport.rx_write = 0;
    transport.rx_stocked = 0;
    return INTEL_WIRELESS_OK;
}

static uint32_t rx_space(void) {
    return (transport.rx_read - transport.rx_write - 1) & RX_MASK;
}

/* A buffer is named to the device by its address with a twelve-bit id in the
   low bits, which a page-aligned buffer leaves free; the id is what comes
   back in the used ring, and it starts at one because zero means nothing. */
static void rx_restock(void) {
    while (rx_space() > 0 && transport.spare_count > 0) {
        uint16_t id = transport.spare_ids[--transport.spare_count];
        transport.free_ring[transport.rx_write] = physical(transport.rx_buffer[id - 1]) | id;
        transport.rx_write = (transport.rx_write + 1) & RX_MASK;
    }
    __asm__ volatile("" ::: "memory");
    intel_wireless_write32(RFH_Q0_FRBDCB_WIDX_TRG, transport.rx_write & ~7u);
}

static int command_queue_allocate(void) {
    if (!transport.tfds) {
        transport.tfds = dma_pages(pages_for(INTEL_WIRELESS_COMMAND_SLOTS * sizeof(intel_wireless_tfd_t)));
        if (!transport.tfds) {
            return INTEL_WIRELESS_NO_MEMORY;
        }
        for (uint32_t i = 0; i < INTEL_WIRELESS_COMMAND_SLOTS; i++) {
            transport.command_buffer[i] = dma_pages(1);
            if (!transport.command_buffer[i]) {
                return INTEL_WIRELESS_NO_MEMORY;
            }
        }
    }
    for (uint32_t i = 0; i < INTEL_WIRELESS_COMMAND_SLOTS; i++) {
        intel_wireless_tfd_t *tfd = &transport.tfds[i];
        uint8_t *bytes = (uint8_t *)tfd;
        for (uint32_t b = 0; b < sizeof(*tfd); b++) {
            bytes[b] = 0;
        }
    }
    transport.command_write = 0;
    transport.command_read = 0;
    transport.waiting_index = -1;
    return INTEL_WIRELESS_OK;
}

static void free_sections(uint8_t **copies, uint32_t *pages, uint32_t *count) {
    for (uint32_t i = 0; i < *count; i++) {
        free_pages(copies[i], pages[i]);
        copies[i] = 0;
    }
    *count = 0;
}

static int copy_sections(const intel_wireless_section_t *sections, uint32_t count, uint8_t **copies,
                         uint32_t *pages, uint32_t *copied, uint32_t map_offset) {
    uint8_t *map = (uint8_t *)transport.context + map_offset;
    for (uint32_t i = 0; i < count; i++) {
        pages[i] = pages_for(sections[i].length);
        copies[i] = dma_pages(pages[i]);
        if (!copies[i]) {
            return INTEL_WIRELESS_NO_MEMORY;
        }
        *copied = i + 1;
        for (uint32_t b = 0; b < sections[i].length; b++) {
            copies[i][b] = sections[i].data[b];
        }
        uint64_t address = physical(copies[i]);
        for (int b = 0; b < 8; b++) {
            map[i * 8 + (uint32_t)b] = (uint8_t)(address >> (8 * b));
        }
    }
    return INTEL_WIRELESS_OK;
}

static int log2_of(uint32_t value) {
    int bits = 0;
    while ((1u << (bits + 1)) <= value) {
        bits++;
    }
    return bits;
}

/* The 22000 family loads itself: the boot ROM reads one block of host memory
   that says where every firmware section is, where the receive ring is and
   where the command queue is, and copies the sections in on its own. */
static int context_info_init(const intel_wireless_firmware_t *firmware, uint32_t hardware_revision) {
    if (firmware->lmac_count > INTEL_WIRELESS_DRAM_ENTRIES || firmware->umac_count > INTEL_WIRELESS_DRAM_ENTRIES ||
        firmware->paging_count > INTEL_WIRELESS_DRAM_ENTRIES) {
        return INTEL_WIRELESS_TOO_LARGE;
    }
    transport.context = dma_pages(1);
    if (!transport.context) {
        return INTEL_WIRELESS_NO_MEMORY;
    }
    transport.context_physical = physical(transport.context);
    intel_wireless_context_info_t *context = transport.context;
    context->mac_id = (uint16_t)hardware_revision;
    context->version = 0;
    context->size = (uint16_t)(sizeof(*context) / 4);
    context->control_flags = IWL_CTXT_INFO_TFD_FORMAT_LONG |
                             ((uint32_t)log2_of(INTEL_WIRELESS_RX_RING) << IWL_CTXT_INFO_RB_CB_SIZE_POSITION) |
                             (IWL_CTXT_INFO_RB_SIZE_4K << IWL_CTXT_INFO_RB_SIZE_POSITION);
    context->free_rbd_address = physical(transport.free_ring);
    context->used_rbd_address = physical(transport.used_ring);
    context->status_write_pointer = physical(transport.rb_status);
    context->command_queue_address = physical(transport.tfds);
    context->command_queue_size = (uint8_t)(log2_of(INTEL_WIRELESS_COMMAND_SLOTS) - 3);

    int result = copy_sections(firmware->lmac, firmware->lmac_count, transport.lmac_copy, transport.lmac_pages,
                               &transport.lmac_count, offsetof(intel_wireless_context_info_t, lmac_image));
    if (result == INTEL_WIRELESS_OK) {
        result = copy_sections(firmware->umac, firmware->umac_count, transport.umac_copy, transport.umac_pages,
                               &transport.umac_count, offsetof(intel_wireless_context_info_t, umac_image));
    }
    if (result == INTEL_WIRELESS_OK) {
        result = copy_sections(firmware->paging, firmware->paging_count, transport.paging_copy,
                               transport.paging_pages, &transport.paging_count,
                               offsetof(intel_wireless_context_info_t, virtual_image));
    }
    return result;
}

static void radio_configuration(const intel_wireless_firmware_t *firmware, uint32_t hardware_revision) {
    uint32_t radio_type = firmware->phy_config & 0x3u;
    uint32_t radio_step = (firmware->phy_config >> 2) & 0x3u;
    uint32_t radio_dash = (firmware->phy_config >> 4) & 0x3u;
    uint32_t value = (hardware_revision & CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP_DASH) |
                     (radio_type << CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE) |
                     (radio_step << CSR_HW_IF_CONFIG_REG_POS_PHY_STEP) |
                     (radio_dash << CSR_HW_IF_CONFIG_REG_POS_PHY_DASH);
    uint32_t mask = CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP_DASH | CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE |
                    CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP | CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH |
                    CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI | CSR_HW_IF_CONFIG_REG_BIT_MAC_SI;
    intel_wireless_write32(CSR_HW_IF_CONFIG_REG, (intel_wireless_read32(CSR_HW_IF_CONFIG_REG) & ~mask) | value);
}

int intel_wireless_start_firmware(const intel_wireless_firmware_t *firmware, uint32_t hardware_revision) {
    int result = intel_wireless_prepare_card();
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    disable_interrupts();

    intel_wireless_write32(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
    intel_wireless_write32(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
    intel_wireless_write32(CSR_INT, 0xFFFFFFFFu);

    result = power_management_init();
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    radio_configuration(firmware, hardware_revision);

    write8(CSR_INT_COALESCING, IWL_HOST_INT_TIMEOUT_DEF);
    result = rx_allocate();
    if (result == INTEL_WIRELESS_OK) {
        result = command_queue_allocate();
    }
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    set_bits(CSR_MAC_SHADOW_REG_CTRL, 0x800FFFFFu);

    result = context_info_init(firmware, hardware_revision);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    transport.error_seen = 0;
    intel_wireless_write32(CSR_INT_MASK, CSR_INT_BIT_ALIVE | CSR_INT_BIT_FH_RX);
    __asm__ volatile("" ::: "memory");
    intel_wireless_write32(CSR_CTXT_INFO_BA, (uint32_t)transport.context_physical);
    intel_wireless_write32(CSR_CTXT_INFO_BA + 4, (uint32_t)(transport.context_physical >> 32));

    uint32_t ltr = CSR_LTR_LONG_VAL_AD_NO_SNOOP_REQ | (CSR_LTR_LONG_VAL_AD_SCALE_USEC << 26) | (250u << 16) |
                   CSR_LTR_LONG_VAL_AD_SNOOP_REQ | (CSR_LTR_LONG_VAL_AD_SCALE_USEC << 10) | 250u;
    intel_wireless_write_periphery(HPM_MAC_LTR_CSR, HPM_MAC_LRT_ENABLE_ALL);
    intel_wireless_write_periphery(HPM_UMAC_LTR, ltr);

    intel_wireless_write_periphery(UREG_CPU_INIT_RUN, 1);
    return INTEL_WIRELESS_OK;
}

void intel_wireless_set_packet_handler(intel_wireless_packet_handler_t handler) {
    transport.handler = handler;
}

static void complete_command(const intel_wireless_rx_packet_t *packet, const uint8_t *payload,
                             uint32_t payload_length) {
    uint32_t index = SEQUENCE_TO_INDEX(packet->sequence);
    transport.statistics.commands_answered++;
    transport.command_read = (index + 1) & (INTEL_WIRELESS_TFD_QUEUE_SIZE_MAX - 1);
    if (transport.waiting_index == (int)index) {
        uint32_t copy = payload_length < transport.waiting_capacity ? payload_length : transport.waiting_capacity;
        for (uint32_t i = 0; i < copy; i++) {
            transport.waiting_response[i] = payload[i];
        }
        transport.waiting_length = payload_length;
        transport.waiting_answered = 1;
        return;
    }
    if (transport.handler) {
        transport.handler(packet, payload, payload_length);
    }
}

/* One receive buffer can hold several packets, each 64-byte aligned, and the
   first word of each is its length; an invalid marker ends the run. */
static void handle_buffer(const uint8_t *buffer) {
    uint32_t offset = 0;
    while (offset + 4 + 4 < INTEL_WIRELESS_RX_BUFFER) {
        const intel_wireless_rx_packet_t *packet = (const intel_wireless_rx_packet_t *)(buffer + offset);
        if (packet->length_and_flags == FH_RSCSR_FRAME_INVALID) {
            break;
        }
        uint32_t frame = packet->length_and_flags & FH_RSCSR_FRAME_SIZE_MASK;
        uint32_t length = frame + 4;
        if (length < sizeof(intel_wireless_rx_packet_t)) {
            break;
        }
        uint32_t aligned = (length + FH_RSCSR_FRAME_ALIGN - 1) & ~(FH_RSCSR_FRAME_ALIGN - 1);
        if (offset + aligned > INTEL_WIRELESS_RX_BUFFER) {
            break;
        }
        uint32_t payload_length = frame - 4;
        transport.statistics.rx_packets++;
        int from_firmware = (packet->sequence & SEQ_RX_FRAME) != 0;
        if (!from_firmware && ((packet->sequence >> 8) & 0x1Fu) == INTEL_WIRELESS_COMMAND_QUEUE) {
            complete_command(packet, packet->data, payload_length);
        } else if (transport.handler) {
            transport.handler(packet, packet->data, payload_length);
        }
        offset += aligned;
    }
}

static void handle_receive_ring(void) {
    __asm__ volatile("" ::: "memory");
    uint32_t closed = ((volatile intel_wireless_rb_status_t *)transport.rb_status)->closed_rb_number & 0xFFFu;
    closed &= RX_MASK;
    while (transport.rx_read != closed) {
        uint32_t id = ((volatile uint32_t *)transport.used_ring)[transport.rx_read] & 0xFFFu;
        if (id >= 1 && id <= INTEL_WIRELESS_RX_RING) {
            uint8_t *buffer = transport.rx_buffer[id - 1];
            handle_buffer(buffer);
            *(uint32_t *)buffer = FH_RSCSR_FRAME_INVALID;
            transport.spare_ids[transport.spare_count++] = (uint16_t)id;
            transport.statistics.rx_buffers_handled++;
        }
        transport.rx_read = (transport.rx_read + 1) & RX_MASK;
    }
    if (transport.rx_stocked) {
        rx_restock();
    }
}

/* Reading a register on this device costs about a millisecond - measured on
   the laptop, where a scan that polled the interrupt cause every millisecond
   kept a processor 90% busy inside that one load. Everything the firmware
   sends arrives through the receive ring, whose progress the device writes
   into host memory; so once the ring is stocked, the cause register is read
   only now and then, to notice a firmware that has stopped. */
#define CAUSE_CHECK_US 100000u
#define CAUSE_CHECK_POLLS 16u

static int cause_check_due(void) {
#ifdef LEANOS_HOST_TEST
    return transport.statistics.polls % CAUSE_CHECK_POLLS == 0;
#else
    uint64_t now = tsc_to_us(tsc_read());
    if (now < transport.next_cause_check) {
        return 0;
    }
    transport.next_cause_check = now + CAUSE_CHECK_US;
    return 1;
#endif
}

int intel_wireless_poll(void) {
    transport.statistics.polls++;
    if (transport.rx_stocked && !cause_check_due()) {
        __asm__ volatile("" ::: "memory");
        uint32_t closed = ((volatile intel_wireless_rb_status_t *)transport.rb_status)->closed_rb_number & 0xFFFu;
        if ((closed & RX_MASK) != transport.rx_read) {
            handle_receive_ring();
        }
        return INTEL_WIRELESS_OK;
    }
    transport.statistics.cause_reads++;
    uint32_t causes = intel_wireless_read32(CSR_INT);
    if (causes == 0xFFFFFFFFu || is_error_value(causes)) {
        return INTEL_WIRELESS_HARDWARE_ERROR;
    }
    if (causes) {
        intel_wireless_write32(CSR_INT, causes);
        transport.statistics.interrupt_causes_seen++;
        transport.statistics.last_interrupt_causes = causes;
    }
    if (causes & (CSR_INT_BIT_FH_RX | CSR_INT_BIT_SW_RX)) {
        intel_wireless_write32(CSR_FH_INT_STATUS, CSR_FH_INT_RX_MASK);
    }
    if (causes & CSR_INT_BIT_ALIVE) {
        transport.rx_stocked = 1;
        rx_restock();
    }
    handle_receive_ring();
    if (causes & (CSR_INT_BIT_SW_ERR | CSR_INT_BIT_HW_ERR)) {
        transport.error_seen = 1;
        transport.statistics.firmware_errors++;
        return (causes & CSR_INT_BIT_HW_ERR) ? INTEL_WIRELESS_HARDWARE_ERROR : INTEL_WIRELESS_FIRMWARE_ERROR;
    }
    return INTEL_WIRELESS_OK;
}

static intel_wireless_alive_v6_t *alive_out;
static int alive_seen;

static void alive_handler(const intel_wireless_rx_packet_t *packet, const uint8_t *payload,
                          uint32_t payload_length) {
    if (packet->group > LONG_GROUP || packet->command != UCODE_ALIVE_NTFY) {
        return;
    }
    uint8_t *out = (uint8_t *)alive_out;
    uint32_t copy = payload_length < sizeof(*alive_out) ? payload_length : sizeof(*alive_out);
    for (uint32_t i = 0; i < sizeof(*alive_out); i++) {
        out[i] = i < copy ? payload[i] : 0;
    }
    alive_seen = payload_length >= sizeof(*alive_out) ? 1 : -1;
}

int intel_wireless_wait_alive(intel_wireless_alive_v6_t *alive, uint32_t timeout_ms) {
    intel_wireless_packet_handler_t saved = transport.handler;
    alive_out = alive;
    alive_seen = 0;
    transport.handler = alive_handler;
    int result = INTEL_WIRELESS_NO_ALIVE;
    for (uint32_t waited = 0; waited <= timeout_ms; waited++) {
        int polled = intel_wireless_poll();
        if (polled != INTEL_WIRELESS_OK) {
            result = polled;
            break;
        }
        if (alive_seen) {
            result = alive_seen > 0 ? INTEL_WIRELESS_OK : INTEL_WIRELESS_BAD_ALIVE;
            break;
        }
        intel_wireless_sleep_ms(1);
    }
    transport.handler = saved;
    return result;
}

/* Once the firmware is running it has copied what it needed out of the
   sections and the context block; the paging sections stay, because it goes
   on reading those for as long as it runs. */
void intel_wireless_firmware_alive(void) {
    free_pages(transport.context, 1);
    transport.context = 0;
    free_sections(transport.lmac_copy, transport.lmac_pages, &transport.lmac_count);
    free_sections(transport.umac_copy, transport.umac_pages, &transport.umac_count);
    intel_wireless_write32(CSR_INT_MASK, CSR_INI_SET_MASK);
}

int intel_wireless_send(uint8_t group, uint8_t command, uint8_t version, const void *payload, uint32_t length,
                        void *response, uint32_t capacity, uint32_t *response_length, uint32_t timeout_ms) {
    if (length + sizeof(intel_wireless_command_header_t) > PAGE_BYTES) {
        return INTEL_WIRELESS_TOO_LARGE;
    }
    uint32_t in_flight = (transport.command_write - transport.command_read) & (INTEL_WIRELESS_TFD_QUEUE_SIZE_MAX - 1);
    if (in_flight >= INTEL_WIRELESS_COMMAND_SLOTS - 1) {
        return INTEL_WIRELESS_QUEUE_FULL;
    }
    uint32_t write = transport.command_write;
    uint32_t slot = write & (INTEL_WIRELESS_COMMAND_SLOTS - 1);
    uint8_t *buffer = transport.command_buffer[slot];

    /* A command with no group goes out in the long group: the firmware takes
       the wide header for every command once it has one, and Linux names the
       legacy commands that way for the same reason. */
    intel_wireless_command_header_t *header = (intel_wireless_command_header_t *)buffer;
    header->command = command;
    header->group = group == LEGACY_GROUP ? LONG_GROUP : group;
    header->sequence = (uint16_t)(QUEUE_TO_SEQUENCE(INTEL_WIRELESS_COMMAND_QUEUE) | INDEX_TO_SEQUENCE(write));
    header->length = (uint16_t)length;
    header->reserved = 0;
    header->version = version;
    const uint8_t *bytes = (const uint8_t *)payload;
    for (uint32_t i = 0; i < length; i++) {
        buffer[sizeof(*header) + i] = bytes[i];
    }
    uint32_t total = (uint32_t)sizeof(*header) + length;

    intel_wireless_tfd_t *tfd = &transport.tfds[slot];
    uint8_t *tfd_bytes = (uint8_t *)tfd;
    for (uint32_t i = 0; i < sizeof(*tfd); i++) {
        tfd_bytes[i] = 0;
    }
    uint32_t first = total < INTEL_WIRELESS_FIRST_TB_SIZE ? total : INTEL_WIRELESS_FIRST_TB_SIZE;
    tfd->buffers[0].address = physical(buffer);
    tfd->buffers[0].length = (uint16_t)first;
    tfd->buffer_count = 1;
    if (total > first) {
        tfd->buffers[1].address = physical(buffer) + first;
        tfd->buffers[1].length = (uint16_t)(total - first);
        tfd->buffer_count = 2;
    }

    transport.waiting_index = (int)write;
    transport.waiting_answered = 0;
    transport.waiting_response = (uint8_t *)response;
    transport.waiting_capacity = response ? capacity : 0;
    transport.waiting_length = 0;

    transport.command_write = (write + 1) & (INTEL_WIRELESS_TFD_QUEUE_SIZE_MAX - 1);
    __asm__ volatile("" ::: "memory");
    intel_wireless_write32(HBUS_TARG_WRPTR, transport.command_write | ((uint32_t)INTEL_WIRELESS_COMMAND_QUEUE << 16));
    transport.statistics.commands_sent++;

    int result = INTEL_WIRELESS_TIMED_OUT;
    for (uint32_t waited = 0; waited <= timeout_ms * 10; waited++) {
        int polled = intel_wireless_poll();
        if (transport.waiting_answered) {
            result = INTEL_WIRELESS_OK;
            break;
        }
        if (polled != INTEL_WIRELESS_OK) {
            result = polled;
            break;
        }
        intel_wireless_delay_us(100);
    }
    if (response_length) {
        *response_length = transport.waiting_length;
    }
    transport.waiting_index = -1;
    return result;
}

int intel_wireless_data_queue_memory(uint64_t *tfds, uint64_t *byte_counts, uint32_t *cb_size) {
    if (!transport.data_tfds) {
        transport.data_tfds = dma_pages(pages_for(INTEL_WIRELESS_DATA_SLOTS * sizeof(intel_wireless_tfd_t)));
        transport.byte_counts = dma_pages(1);
        if (!transport.data_tfds || !transport.byte_counts) {
            return INTEL_WIRELESS_NO_MEMORY;
        }
        for (uint32_t i = 0; i < INTEL_WIRELESS_DATA_SLOTS; i++) {
            transport.data_buffer[i] = dma_pages(1);
            if (!transport.data_buffer[i]) {
                return INTEL_WIRELESS_NO_MEMORY;
            }
        }
    }
    uint8_t *bytes = (uint8_t *)transport.data_tfds;
    for (uint32_t i = 0; i < INTEL_WIRELESS_DATA_SLOTS * sizeof(intel_wireless_tfd_t); i++) {
        bytes[i] = 0;
    }
    for (uint32_t i = 0; i < INTEL_WIRELESS_BYTE_COUNT_ENTRIES; i++) {
        transport.byte_counts[i] = 0;
    }
    *tfds = physical(transport.data_tfds);
    *byte_counts = physical(transport.byte_counts);
    *cb_size = (uint32_t)log2_of(INTEL_WIRELESS_DATA_SLOTS) - 3;
    return INTEL_WIRELESS_OK;
}

void intel_wireless_data_queue_start(uint16_t queue, uint16_t write_pointer) {
    transport.data_queue = queue;
    transport.data_write = write_pointer & (INTEL_WIRELESS_TFD_QUEUE_SIZE_MAX - 1);
    transport.data_read = transport.data_write;
}

void intel_wireless_data_queue_stop(void) {
    transport.data_queue = -1;
}

int intel_wireless_data_queue_number(void) {
    return transport.data_queue;
}

uint32_t intel_wireless_data_in_flight(void) {
    return (transport.data_write - transport.data_read) & (INTEL_WIRELESS_TFD_QUEUE_SIZE_MAX - 1);
}

void intel_wireless_data_completed(uint32_t frames) {
    if (frames > intel_wireless_data_in_flight()) {
        frames = intel_wireless_data_in_flight();
    }
    transport.data_read = (transport.data_read + frames) & (INTEL_WIRELESS_TFD_QUEUE_SIZE_MAX - 1);
    transport.statistics.frames_completed += frames;
}

/* Linux's layout for this family, iwl_txq_gen2_build_tx: the first transfer
   buffer is exactly the first twenty bytes of the command header and TX
   command, the second the rest of them with the 802.11 header, padded to a
   word, and the third the payload. The byte-count table tells the scheduler
   the frame's length in words and how many 64-byte pieces of the TFD to
   fetch, less one. */
int intel_wireless_data_send(const intel_wireless_tx_command_t *command, const uint8_t *header,
                             uint32_t header_length, const uint8_t *payload, uint32_t payload_length) {
    if (transport.data_queue < 0) {
        return INTEL_WIRELESS_NOT_READY;
    }
    if (intel_wireless_data_in_flight() >= INTEL_WIRELESS_DATA_SLOTS - 2) {
        return INTEL_WIRELESS_QUEUE_FULL;
    }
    if (header_length > 32 || payload_length > PAGE_BYTES - INTEL_WIRELESS_DATA_PAYLOAD_OFFSET) {
        return INTEL_WIRELESS_TOO_LARGE;
    }
    uint32_t index = transport.data_write & (INTEL_WIRELESS_DATA_SLOTS - 1);
    uint8_t *buffer = transport.data_buffer[index];
    intel_wireless_short_header_t *short_header = (intel_wireless_short_header_t *)buffer;
    short_header->command = TX_CMD;
    short_header->group = 0;
    short_header->sequence = (uint16_t)(QUEUE_TO_SEQUENCE((uint32_t)transport.data_queue) | INDEX_TO_SEQUENCE(index));
    const uint8_t *command_bytes = (const uint8_t *)command;
    uint32_t at = sizeof(*short_header);
    for (uint32_t i = 0; i < sizeof(*command); i++) {
        buffer[at++] = command_bytes[i];
    }
    for (uint32_t i = 0; i < header_length; i++) {
        buffer[at++] = header[i];
    }
    while (at % 4) {
        buffer[at++] = 0;
    }
    for (uint32_t i = 0; i < payload_length; i++) {
        buffer[INTEL_WIRELESS_DATA_PAYLOAD_OFFSET + i] = payload[i];
    }

    intel_wireless_tfd_t *tfd = &transport.data_tfds[index];
    uint8_t *tfd_bytes = (uint8_t *)tfd;
    for (uint32_t i = 0; i < sizeof(*tfd); i++) {
        tfd_bytes[i] = 0;
    }
    tfd->buffers[0].address = physical(buffer);
    tfd->buffers[0].length = INTEL_WIRELESS_FIRST_TB_SIZE;
    tfd->buffers[1].address = physical(buffer) + INTEL_WIRELESS_FIRST_TB_SIZE;
    tfd->buffers[1].length = (uint16_t)(at - INTEL_WIRELESS_FIRST_TB_SIZE);
    uint32_t count = 2;
    if (payload_length) {
        tfd->buffers[2].address = physical(buffer) + INTEL_WIRELESS_DATA_PAYLOAD_OFFSET;
        tfd->buffers[2].length = (uint16_t)payload_length;
        count = 3;
    }
    tfd->buffer_count = (uint16_t)count;

    uint32_t filled = 2 + count * (uint32_t)sizeof(intel_wireless_transfer_buffer_t);
    uint32_t chunks = (filled + 63) / 64 - 1;
    uint32_t words = ((uint32_t)command->length + 3) / 4;
    transport.byte_counts[index] = (uint16_t)(words | chunks << 12);

    transport.data_write = (transport.data_write + 1) & (INTEL_WIRELESS_TFD_QUEUE_SIZE_MAX - 1);
    __asm__ volatile("" ::: "memory");
    intel_wireless_write32(HBUS_TARG_WRPTR, transport.data_write | ((uint32_t)transport.data_queue << 16));
    transport.statistics.frames_queued++;
    return INTEL_WIRELESS_OK;
}

/* Puts the device back in reset - its DMA stops before any memory it was
   given goes back - and gives everything back. A firmware that misbehaved
   must not be left holding addresses this kernel will reuse. */
void intel_wireless_stop(void) {
    if (!transport.registers && !transport.free_ring && !transport.tfds) {
        return;
    }
    disable_interrupts();
    set_bits(CSR_RESET, CSR_RESET_REG_FLAG_STOP_MASTER);
    poll_bits(CSR_RESET, CSR_RESET_REG_FLAG_MASTER_DISABLED, CSR_RESET_REG_FLAG_MASTER_DISABLED, 100);
    set_bits(CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET);
    intel_wireless_delay_us(5000);
    clear_bits(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
    transport.rx_stocked = 0;
    free_pages(transport.context, 1);
    transport.context = 0;
    free_sections(transport.lmac_copy, transport.lmac_pages, &transport.lmac_count);
    free_sections(transport.umac_copy, transport.umac_pages, &transport.umac_count);
    free_sections(transport.paging_copy, transport.paging_pages, &transport.paging_count);
    if (transport.free_ring) {
        free_pages(transport.free_ring, pages_for(INTEL_WIRELESS_RX_RING * sizeof(uint64_t)));
        free_pages(transport.used_ring, pages_for(INTEL_WIRELESS_RX_RING * sizeof(uint32_t)));
        free_pages(transport.rb_status, 1);
        for (uint32_t i = 0; i < INTEL_WIRELESS_RX_RING; i++) {
            free_pages(transport.rx_buffer[i], INTEL_WIRELESS_RX_BUFFER / PAGE_BYTES);
            transport.rx_buffer[i] = 0;
        }
        transport.free_ring = 0;
        transport.used_ring = 0;
        transport.rb_status = 0;
    }
    if (transport.data_tfds) {
        free_pages(transport.data_tfds, pages_for(INTEL_WIRELESS_DATA_SLOTS * sizeof(intel_wireless_tfd_t)));
        free_pages(transport.byte_counts, 1);
        for (uint32_t i = 0; i < INTEL_WIRELESS_DATA_SLOTS; i++) {
            free_pages(transport.data_buffer[i], 1);
            transport.data_buffer[i] = 0;
        }
        transport.data_tfds = 0;
        transport.byte_counts = 0;
    }
    transport.data_queue = -1;
    if (transport.tfds) {
        free_pages(transport.tfds, pages_for(INTEL_WIRELESS_COMMAND_SLOTS * sizeof(intel_wireless_tfd_t)));
        for (uint32_t i = 0; i < INTEL_WIRELESS_COMMAND_SLOTS; i++) {
            free_pages(transport.command_buffer[i], 1);
            transport.command_buffer[i] = 0;
        }
        transport.tfds = 0;
    }
}

const intel_wireless_transport_statistics_t *intel_wireless_transport_statistics(void) {
    return &transport.statistics;
}
