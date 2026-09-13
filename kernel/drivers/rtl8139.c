#include "rtl8139.h"
#include "rtl8139_ring.h"
#include "device/random.h"

#include "architecture/x86_64/io.h"
#include "architecture/x86_64/interrupt_service_routines.h"
#include "architecture/x86_64/pic.h"
#include "kernel_log.h"
#include "library/kernel_library.h"
#include "memory_management/physical_memory.h"
#include "network/ethernet.h"
#include "panic.h"
#include "pci.h"

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

#define REG_IDR0    0x00
#define REG_TSD0    0x10
#define REG_TSAD0   0x20
#define REG_RBSTART 0x30
#define REG_CMD     0x37
#define REG_CAPR    0x38
#define REG_IMR     0x3C
#define REG_ISR     0x3E
#define REG_TCR     0x40
#define REG_RCR     0x44
#define REG_CONFIG1 0x52

#define CMD_TE   (1 << 2)
#define CMD_RE   (1 << 3)
#define CMD_RST  (1 << 4)
#define CMD_BUFE (1 << 0)

#define ISR_ROK (1 << 0)
#define ISR_TOK (1 << 2)

#define RCR_AAP  (1 << 0)
#define RCR_APM  (1 << 1)
#define RCR_AM   (1 << 2)
#define RCR_AB   (1 << 3)
#define RCR_WRAP (1 << 7)

#define TSD_OWN (1u << 13)

#define RX_BUFFER_SIZE (RTL8139_RING_LEN + RTL8139_RING_PAD)
#define RX_BUFFER_FRAMES ((RX_BUFFER_SIZE + 4095) / 4096)

#define TX_DESCRIPTORS 4

static uint16_t io_base;
static uint8_t mac[6];
static uint8_t *rx_buffer;
static uint32_t rx_read_offset;
static uint8_t *tx_buffer[TX_DESCRIPTORS];
static int tx_next_descriptor;

#define TX_POLL_LIMIT 1000000

static void rtl8139_irq(isr_regs_t *regs) {
    (void)regs;
    uint16_t status = inw(io_base + REG_ISR);
    outw(io_base + REG_ISR, status);

    if (status & ISR_ROK) {
        while (!(inb(io_base + REG_CMD) & CMD_BUFE)) {
            rtl8139_rx_t rx;
            rx_read_offset = rtl8139_ring_take(rx_buffer, rx_read_offset, &rx);
            if (rx.len) {
                eth_receive(rx.frame, rx.len);
            }

            outw(io_base + REG_CAPR, (uint16_t)(rx_read_offset - 16));
        }
    }
}

static uint32_t tx_errors;

uint32_t rtl8139_tx_error_count(void) {
    return tx_errors;
}

static int rtl8139_reset(void) {
    outb(io_base + REG_CMD, CMD_RST);
    for (int i = 0; i < TX_POLL_LIMIT; i++) {
        if (!(inb(io_base + REG_CMD) & CMD_RST)) {
            return 1;
        }
    }
    kernel_log_puts("[rtl8139] reset did not complete - carrying on without a network.\n");
    return 0;
}

int rtl8139_init(void) {
    pci_device_t dev;
    if (!pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, &dev)) {
        return 0;
    }
    pci_enable_device(&dev);
    io_base = pci_bar0_io_base(&dev);
    if (io_base == 0) {
        kernel_log_puts("[rtl8139] BAR0 is memory-mapped - this driver speaks port I/O only.\n");
        return 0;
    }

    outb(io_base + REG_CONFIG1, 0x00);
    if (!rtl8139_reset()) {
        return 0;
    }

    for (int i = 0; i < 6; i++) {
        mac[i] = inb(io_base + REG_IDR0 + i);
    }

    uint64_t rx_phys = physical_memory_alloc_contiguous(RX_BUFFER_FRAMES);
    rx_buffer = (uint8_t *)(uintptr_t)rx_phys;
    rx_read_offset = 0;
    outl(io_base + REG_RBSTART, (uint32_t)rx_phys);

    for (int i = 0; i < TX_DESCRIPTORS; i++) {
        uint64_t tx_phys = physical_memory_alloc_contiguous(1);
        tx_buffer[i] = (uint8_t *)(uintptr_t)tx_phys;
    }
    tx_next_descriptor = 0;

    outl(io_base + REG_TCR, 0);
    outl(io_base + REG_RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_WRAP);

    outw(io_base + REG_IMR, ISR_ROK | ISR_TOK);
    outb(io_base + REG_CMD, CMD_RE | CMD_TE);

    irq_register_handler(dev.irq_line, rtl8139_irq);
    irq_enable_line(dev.irq_line);

    random_feed(mac, sizeof(mac));
    kernel_log_puts("[net] rtl8139 found at PCI ");
    kernel_log_put_hex32(dev.bus);
    kernel_log_puts(":");
    kernel_log_put_hex32(dev.slot);
    kernel_log_puts(":");
    kernel_log_put_hex32(dev.func);
    kernel_log_puts(", io_base=0x");
    kernel_log_put_hex32(io_base);
    kernel_log_puts(", irq=");
    kernel_log_put_hex32(dev.irq_line);
    kernel_log_puts(", mac=0x");
    uint64_t mac_packed = 0;
    for (int i = 0; i < 6; i++) {
        mac_packed = (mac_packed << 8) | mac[i];
    }
    kernel_log_put_hex64(mac_packed);
    kernel_log_putc('\n');
    return 1;
}

const uint8_t *rtl8139_mac(void) {
    return mac;
}

int rtl8139_send(const uint8_t *frame, uint16_t len) {
    if (len > RTL8139_MAX_FRAME) {
        tx_errors++;
        return -1;
    }

    int slot = tx_next_descriptor;
    tx_next_descriptor = (tx_next_descriptor + 1) % TX_DESCRIPTORS;

    k_memcpy(tx_buffer[slot], frame, len);
    outl(io_base + REG_TSAD0 + (uint32_t)slot * 4, (uint32_t)(uintptr_t)tx_buffer[slot]);
    outl(io_base + REG_TSD0 + (uint32_t)slot * 4, len);

    for (int i = 0; i < TX_POLL_LIMIT; i++) {
        if (inl(io_base + REG_TSD0 + (uint32_t)slot * 4) & TSD_OWN) {
            return 0;
        }
    }
    tx_errors++;
    return -1;
}
