#include "xhci.h"

#include "drivers/keyboard.h"
#include "drivers/kernel_log.h"
#include "drivers/mouse.h"
#include "drivers/pci.h"
#include "drivers/usb_hid.h"
#include "drivers/xhci_ring.h"
#include "library/kernel_library.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"

#define XHCI_CLASS    0x0C
#define XHCI_SUBCLASS 0x03
#define XHCI_PROG_IF  0x30

#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_PAGESIZE 0x08
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38
#define OP_PORTSC(p) (0x400 + ((p) - 1) * 0x10)

#define USBCMD_RS    (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH   (1u << 0)
#define USBSTS_CNR   (1u << 11)

#define PORTSC_CCS  (1u << 0)
#define PORTSC_PED  (1u << 1)
#define PORTSC_PR   (1u << 4)
#define PORTSC_PP   (1u << 9)
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_PRC  (1u << 21)
#define PORTSC_RW1C_MASK (PORTSC_PED | (1u << 16) | (0x7Fu << 17))

#define RT_IMAN   0x20
#define RT_IMOD   0x24
#define RT_ERSTSZ 0x28
#define RT_ERSTBA 0x30
#define RT_ERDP   0x38

#define TRB_TYPE_SHIFT XHCI_TRB_TYPE_SHIFT
#define TRB_CYCLE      XHCI_TRB_CYCLE
#define TRB_IOC        (1u << 5)
#define TRB_IDT        (1u << 6)

#define TRB_NORMAL        1
#define TRB_SETUP_STAGE   2
#define TRB_DATA_STAGE    3
#define TRB_STATUS_STAGE  4
#define TRB_COMMAND_ENABLE_SLOT     9
#define TRB_COMMAND_ADDRESS_DEVICE  11
#define TRB_COMMAND_CONFIGURE_EP    12
#define TRB_COMMAND_EVALUATE_CONTEXT    13
#define TRB_EVENT_TRANSFER      32
#define TRB_EVENT_COMMAND_COMPLETE  33
#define TRB_EVENT_PORT_CHANGE   34

#define CC_SUCCESS       1
#define CC_SHORT_PACKET  13

#define SPEED_FULL  1
#define SPEED_LOW   2
#define SPEED_HIGH  3
#define SPEED_SUPER 4

#define USB_REQUEST_GET_DESCRIPTOR    0x06
#define USB_REQUEST_SET_CONFIGURATION 0x09
#define USB_HID_SET_PROTOCOL      0x0B
#define USB_HID_SET_IDLE          0x0A
#define USB_DESCRIPTOR_DEVICE 1
#define USB_DESCRIPTOR_CONFIG 2

#define HID_CLASS          3
#define HID_SUBCLASS_BOOT  1
#define HID_PROTO_KEYBOARD 1
#define HID_PROTO_MOUSE    2

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
} erst_entry_t;

#define MAX_HID_DEVICES 4

typedef struct {
    uint8_t slot;
    uint8_t proto;
    uint8_t ep_dci;
    uint8_t report_length;
    xhci_ring_t ring;
    uint8_t *report;
    uint64_t report_phys;
    usb_hid_state_t hid;
} hid_device_t;

static volatile uint8_t *cap_regs;
static volatile uint8_t *op_regs;
static volatile uint8_t *rt_regs;
static volatile uint32_t *doorbells;
static uint32_t max_ports;
static uint32_t context_size;
static uint64_t *dcbaa;
static xhci_ring_t command_ring;
static xhci_ring_t event_ring;
static uint64_t erdp_phys;
static int started;

static hid_device_t hid_devices[MAX_HID_DEVICES];
static int hid_count;
static uint64_t keyboard_reports, mouse_reports;

static uint8_t *enum_buffer;
static uint64_t enum_buffer_phys;

static uint32_t op_read(uint32_t off) { return *(volatile uint32_t *)(op_regs + off); }
static void op_write(uint32_t off, uint32_t v) { *(volatile uint32_t *)(op_regs + off) = v; }
static uint32_t cap_read(uint32_t off) { return *(volatile uint32_t *)(cap_regs + off); }
static uint32_t rt_read(uint32_t off) { return *(volatile uint32_t *)(rt_regs + off); }
static void rt_write(uint32_t off, uint32_t v) { *(volatile uint32_t *)(rt_regs + off) = v; }

static void op_write64(uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(op_regs + off) = (uint32_t)v;
    *(volatile uint32_t *)(op_regs + off + 4) = (uint32_t)(v >> 32);
}

static void rt_write64(uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(rt_regs + off) = (uint32_t)v;
    *(volatile uint32_t *)(rt_regs + off + 4) = (uint32_t)(v >> 32);
}

static int ring_init(xhci_ring_t *r) {
    uint64_t page = physical_memory_alloc_contiguous(1);
    if (!page) {
        return -1;
    }
    xhci_ring_reset(r, (void *)page, page);
    return 0;
}

static void doorbell(uint32_t slot, uint32_t target) {
    doorbells[slot] = target;
}

#define SPIN_LIMIT 40000000u

static int event_wait(xhci_trb_t *out) {
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        xhci_trb_t *e = &event_ring.trb[event_ring.index];
        if ((e->control & TRB_CYCLE) == event_ring.cycle) {
            *out = *e;
            event_ring.index++;
            if (event_ring.index == XHCI_RING_TRBS) {
                event_ring.index = 0;
                event_ring.cycle ^= 1;
            }
            rt_write64(RT_ERDP, (event_ring.phys + event_ring.index * sizeof(xhci_trb_t)) | 8);
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static int command_sync(uint64_t parameter, uint32_t status, uint32_t control, uint8_t *slot_out) {
    xhci_ring_push(&command_ring, parameter, status, control);
    doorbell(0, 0);

    xhci_trb_t ev;
    for (int tries = 0; tries < 8; tries++) {
        if (event_wait(&ev) != 0) {
            return -1;
        }
        uint32_t type = (ev.control >> TRB_TYPE_SHIFT) & 0x3F;
        if (type == TRB_EVENT_PORT_CHANGE) {
            continue;
        }
        if (type != TRB_EVENT_COMMAND_COMPLETE) {
            continue;
        }
        if (slot_out) {
            *slot_out = (uint8_t)((ev.control >> 24) & 0xFF);
        }
        return (int)((ev.status >> 24) & 0xFF);
    }
    return -1;
}

static uint32_t *context_at(void *base, uint32_t index) {
    return (uint32_t *)((uint8_t *)base + (uint64_t)index * context_size);
}

static uint32_t ep0_max_packet(uint32_t speed) {
    switch (speed) {
    case SPEED_LOW:   return 8;
    case SPEED_FULL:  return 8;
    case SPEED_HIGH:  return 64;
    case SPEED_SUPER: return 512;
    default:          return 8;
    }
}

static int control_transfer(uint8_t slot, xhci_ring_t *ring, uint8_t bm_request_type,
                            uint8_t request, uint16_t value, uint16_t index, uint16_t length) {
    uint64_t setup = (uint64_t)bm_request_type | ((uint64_t)request << 8) |
                     ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                     ((uint64_t)length << 48);
    uint32_t trt = length == 0 ? 0 : ((bm_request_type & 0x80) ? 3 : 2);
    xhci_ring_push(ring, setup, 8, (TRB_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IDT | (trt << 16));

    if (length > 0) {
        xhci_ring_push(ring, enum_buffer_phys, length,
                  (TRB_DATA_STAGE << TRB_TYPE_SHIFT) |
                      ((bm_request_type & 0x80) ? (1u << 16) : 0));
    }
    uint32_t status_directory = (length > 0 && (bm_request_type & 0x80)) ? 0 : (1u << 16);
    xhci_ring_push(ring, 0, 0, (TRB_STATUS_STAGE << TRB_TYPE_SHIFT) | TRB_IOC | status_directory);

    doorbell(slot, 1);

    xhci_trb_t ev;
    for (int tries = 0; tries < 8; tries++) {
        if (event_wait(&ev) != 0) {
            return -1;
        }
        uint32_t type = (ev.control >> TRB_TYPE_SHIFT) & 0x3F;
        if (type != TRB_EVENT_TRANSFER) {
            continue;
        }
        uint32_t code = (ev.status >> 24) & 0xFF;
        return (code == CC_SUCCESS || code == CC_SHORT_PACKET) ? 0 : -1;
    }
    return -1;
}

static int enumerate_port(uint32_t port) {
    uint32_t portsc = op_read(OP_PORTSC(port));
    if (!(portsc & PORTSC_CCS)) {
        return 0;
    }

    if (!(portsc & PORTSC_PED)) {
        op_write(OP_PORTSC(port), (portsc & ~PORTSC_RW1C_MASK) | PORTSC_PR);
        int ready = 0;
        for (uint32_t i = 0; i < SPIN_LIMIT && !ready; i++) {
            uint32_t v = op_read(OP_PORTSC(port));
            if (v & PORTSC_PRC) {
                op_write(OP_PORTSC(port), (v & ~PORTSC_RW1C_MASK) | PORTSC_PRC);
                ready = 1;
            }
            __asm__ volatile("pause");
        }
        if (!ready) {
            return 0;
        }
        portsc = op_read(OP_PORTSC(port));
        if (!(portsc & PORTSC_PED)) {
            return 0;
        }
    }

    uint32_t speed = (portsc >> PORTSC_SPEED_SHIFT) & 0x0F;

    uint8_t slot = 0;
    if (command_sync(0, 0, TRB_COMMAND_ENABLE_SLOT << TRB_TYPE_SHIFT, &slot) != CC_SUCCESS || slot == 0) {
        return 0;
    }

    uint64_t dev_context = physical_memory_alloc_contiguous(1);
    uint64_t in_context = physical_memory_alloc_contiguous(1);
    if (!dev_context || !in_context) {
        return 0;
    }
    k_memset((void *)dev_context, 0, 4096);
    k_memset((void *)in_context, 0, 4096);
    dcbaa[slot] = dev_context;

    xhci_ring_t ep0;
    if (ring_init(&ep0) != 0) {
        return 0;
    }

    uint32_t *icc = context_at((void *)in_context, 0);
    icc[1] = 0x3;

    uint32_t *slot_context = context_at((void *)in_context, 1);
    slot_context[0] = (1u << 27) | (speed << 20);
    slot_context[1] = port << 16;

    uint32_t *ep0_context = context_at((void *)in_context, 2);
    ep0_context[1] = (4u << 3) | (3u << 1) | (ep0_max_packet(speed) << 16);
    *(uint64_t *)&ep0_context[2] = ep0.phys | 1;
    ep0_context[4] = 8;

    if (command_sync(in_context, 0, (TRB_COMMAND_ADDRESS_DEVICE << TRB_TYPE_SHIFT) | ((uint32_t)slot << 24),
                     0) != CC_SUCCESS) {
        return 0;
    }

    k_memset(enum_buffer, 0, 64);
    if (control_transfer(slot, &ep0, 0x80, USB_REQUEST_GET_DESCRIPTOR,
                         (USB_DESCRIPTOR_DEVICE << 8), 0, 8) != 0) {
        return 0;
    }
    uint32_t real_mps = enum_buffer[7];
    if (speed == SPEED_FULL && real_mps != 0 && real_mps != ep0_max_packet(speed)) {
        k_memset((void *)in_context, 0, 4096);
        icc = context_at((void *)in_context, 0);
        icc[1] = 0x2;
        ep0_context = context_at((void *)in_context, 2);
        ep0_context[1] = (4u << 3) | (3u << 1) | (real_mps << 16);
        *(uint64_t *)&ep0_context[2] = ep0.phys | ep0.cycle;
        ep0_context[4] = 8;
        command_sync(in_context, 0,
                     (TRB_COMMAND_EVALUATE_CONTEXT << TRB_TYPE_SHIFT) | ((uint32_t)slot << 24), 0);
    }

    k_memset(enum_buffer, 0, 256);
    if (control_transfer(slot, &ep0, 0x80, USB_REQUEST_GET_DESCRIPTOR,
                         (USB_DESCRIPTOR_CONFIG << 8), 0, 255) != 0) {
        return 0;
    }

    uint16_t total = (uint16_t)(enum_buffer[2] | (enum_buffer[3] << 8));
    if (total > 255) {
        total = 255;
    }
    uint8_t config_value = enum_buffer[5];

    int proto = 0, interface_number = -1, ep_address = -1, ep_interval = 8;
    uint16_t ep_mps = 8;
    for (uint16_t off = 0; off + 1 < total;) {
        uint8_t dlen = enum_buffer[off];
        uint8_t dtype = enum_buffer[off + 1];
        if (dlen == 0) {
            break;
        }
        if (dtype == 4 && off + 8 < total) {
            if (enum_buffer[off + 5] == HID_CLASS && enum_buffer[off + 6] == HID_SUBCLASS_BOOT) {
                proto = enum_buffer[off + 7];
                interface_number = enum_buffer[off + 2];
                ep_address = -1;
            } else {
                proto = 0;
                interface_number = -1;
            }
        } else if (dtype == 5 && proto != 0 && ep_address < 0 && off + 6 < total) {
            uint8_t address = enum_buffer[off + 2];
            uint8_t attribute = enum_buffer[off + 3];
            if ((address & 0x80) && (attribute & 0x03) == 3) {
                ep_address = address;
                ep_mps = (uint16_t)(enum_buffer[off + 4] | (enum_buffer[off + 5] << 8));
                ep_interval = enum_buffer[off + 6];
            }
        }
        off = (uint16_t)(off + dlen);
    }

    if (proto != HID_PROTO_KEYBOARD && proto != HID_PROTO_MOUSE) {
        return 0;
    }
    if (ep_address < 0 || hid_count >= MAX_HID_DEVICES) {
        return 0;
    }

    if (control_transfer(slot, &ep0, 0x00, USB_REQUEST_SET_CONFIGURATION, config_value, 0, 0) != 0) {
        return 0;
    }

    hid_device_t *d = &hid_devices[hid_count];
    k_memset(d, 0, sizeof(*d));
    if (ring_init(&d->ring) != 0) {
        return 0;
    }
    uint64_t report_page = physical_memory_alloc_contiguous(1);
    if (!report_page) {
        return 0;
    }
    k_memset((void *)report_page, 0, 4096);
    d->report = (uint8_t *)report_page;
    d->report_phys = report_page;

    uint8_t dci = (uint8_t)(2 * (ep_address & 0x0F) + 1);
    k_memset((void *)in_context, 0, 4096);
    icc = context_at((void *)in_context, 0);
    icc[1] = 1u | (1u << dci);
    slot_context = context_at((void *)in_context, 1);
    slot_context[0] = ((uint32_t)dci << 27) | (speed << 20);
    slot_context[1] = port << 16;

    uint32_t *ep_context = context_at((void *)in_context, dci + 1);
    uint32_t interval;
    if (speed == SPEED_HIGH || speed == SPEED_SUPER) {
        interval = ep_interval > 0 ? (uint32_t)ep_interval - 1 : 3;
    } else {
        interval = 3;
        while ((1u << (interval - 3)) < (uint32_t)(ep_interval > 0 ? ep_interval : 8) &&
               interval < 10) {
            interval++;
        }
    }
    ep_context[0] = interval << 16;
    ep_context[1] = (7u << 3) | (3u << 1) | ((uint32_t)ep_mps << 16);
    *(uint64_t *)&ep_context[2] = d->ring.phys | 1;
    ep_context[4] = ep_mps | ((uint32_t)ep_mps << 16);

    if (command_sync(in_context, 0,
                     (TRB_COMMAND_CONFIGURE_EP << TRB_TYPE_SHIFT) | ((uint32_t)slot << 24),
                     0) != CC_SUCCESS) {
        return 0;
    }

    if (control_transfer(slot, &ep0, 0x21, USB_HID_SET_PROTOCOL, 0,
                         (uint16_t)(interface_number < 0 ? 0 : interface_number), 0) != 0) {
        kernel_log_puts("[usb] the device refused SET_PROTOCOL(boot) - skipping it.\n");
        return 0;
    }
    control_transfer(slot, &ep0, 0x21, USB_HID_SET_IDLE, 0,
                     (uint16_t)(interface_number < 0 ? 0 : interface_number), 0);

    d->slot = slot;
    d->proto = (uint8_t)proto;
    d->ep_dci = dci;
    d->report_length = proto == HID_PROTO_KEYBOARD ? 8 : 4;
    hid_count++;

    xhci_ring_push(&d->ring, d->report_phys, d->report_length,
              (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC);
    doorbell(slot, dci);

    kernel_log_puts("[usb] ");
    kernel_log_puts(proto == HID_PROTO_KEYBOARD ? "boot keyboard" : "boot mouse");
    kernel_log_puts(" on port ");
    kernel_log_put_dec(port);
    kernel_log_puts(", slot ");
    kernel_log_put_dec(slot);
    kernel_log_puts(", endpoint DCI ");
    kernel_log_put_dec(dci);
    kernel_log_putc('\n');
    return 1;
}

static void take_ownership(void) {
    uint32_t hcc = cap_read(CAP_HCCPARAMS1);
    uint32_t ecp = (hcc >> 16) & 0xFFFF;
    if (ecp == 0) {
        return;
    }
    volatile uint8_t *p = cap_regs + ecp * 4;
    for (int hops = 0; hops < 64; hops++) {
        uint32_t cap = *(volatile uint32_t *)p;
        uint8_t id = (uint8_t)(cap & 0xFF);
        uint8_t next = (uint8_t)((cap >> 8) & 0xFF);
        if (id == 1) {
            if (cap & (1u << 16)) {
                *(volatile uint32_t *)p = cap | (1u << 24);
                for (uint32_t i = 0; i < 10000000u; i++) {
                    if (!(*(volatile uint32_t *)p & (1u << 16))) {
                        break;
                    }
                    __asm__ volatile("pause");
                }
                kernel_log_puts("[xhci] took the controller from the firmware.\n");
            }
            *(volatile uint32_t *)(p + 4) = 0xE0000000u;
            return;
        }
        if (next == 0) {
            return;
        }
        p += next * 4;
    }
}

int xhci_init(void) {
    pci_device_t dev;
    for (uint32_t index = 0; index < 4; index++) {
        if (!pci_find_class(XHCI_CLASS, XHCI_SUBCLASS, XHCI_PROG_IF, index, &dev)) {
            return hid_count;
        }
        pci_enable_device(&dev);

        uint64_t bar = pci_bar_memory_base(&dev, 0);
        uint64_t bar_length = pci_bar_memory_size(&dev, 0);
        if (bar == 0 || bar_length == 0) {
            continue;
        }
        cap_regs = (volatile uint8_t *)virtual_memory_map_mmio(bar, bar_length);
        if (cap_regs == 0) {
            kernel_log_puts("[xhci] BAR0 is not mappable - declining this controller.\n");
            continue;
        }

        take_ownership();

        uint8_t caplength = *(volatile uint8_t *)cap_regs;
        op_regs = cap_regs + caplength;
        rt_regs = cap_regs + (cap_read(CAP_RTSOFF) & ~0x1Fu);
        doorbells = (volatile uint32_t *)(cap_regs + (cap_read(CAP_DBOFF) & ~0x3u));

        uint32_t hcs1 = cap_read(CAP_HCSPARAMS1);
        uint32_t max_slots = hcs1 & 0xFF;
        max_ports = (hcs1 >> 24) & 0xFF;
        context_size = (cap_read(CAP_HCCPARAMS1) & (1u << 2)) ? 64 : 32;

        op_write(OP_USBCMD, op_read(OP_USBCMD) & ~USBCMD_RS);
        for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
            if (op_read(OP_USBSTS) & USBSTS_HCH) {
                break;
            }
            __asm__ volatile("pause");
        }
        op_write(OP_USBCMD, USBCMD_HCRST);
        int ready = 0;
        for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
            if (!(op_read(OP_USBCMD) & USBCMD_HCRST) && !(op_read(OP_USBSTS) & USBSTS_CNR)) {
                ready = 1;
                break;
            }
            __asm__ volatile("pause");
        }
        if (!ready) {
            kernel_log_puts("[xhci] the controller never came out of reset - declining it.\n");
            continue;
        }

        op_write(OP_CONFIG, max_slots);

        uint64_t dcbaa_page = physical_memory_alloc_contiguous(1);
        if (!dcbaa_page) {
            continue;
        }
        k_memset((void *)dcbaa_page, 0, 4096);
        dcbaa = (uint64_t *)dcbaa_page;

        uint32_t hcs2 = cap_read(CAP_HCSPARAMS2);
        uint32_t scratchpads = ((hcs2 >> 21) & 0x1F) | (((hcs2 >> 27) & 0x1F) << 5);
        if (scratchpads > 0) {
            uint64_t array = physical_memory_alloc_contiguous(1);
            if (!array) {
                continue;
            }
            k_memset((void *)array, 0, 4096);
            uint64_t *slots = (uint64_t *)array;
            for (uint32_t i = 0; i < scratchpads && i < 512; i++) {
                uint64_t page = physical_memory_alloc_contiguous(1);
                if (!page) {
                    break;
                }
                k_memset((void *)page, 0, 4096);
                slots[i] = page;
            }
            dcbaa[0] = array;
        }
        op_write64(OP_DCBAAP, dcbaa_page);

        if (ring_init(&command_ring) != 0 || ring_init(&event_ring) != 0) {
            continue;
        }
        k_memset(event_ring.trb, 0, 4096);
        event_ring.cycle = 1;
        event_ring.index = 0;

        op_write64(OP_CRCR, command_ring.phys | 1);

        uint64_t erst_page = physical_memory_alloc_contiguous(1);
        if (!erst_page) {
            continue;
        }
        k_memset((void *)erst_page, 0, 4096);
        erst_entry_t *erst = (erst_entry_t *)erst_page;
        erst[0].base = event_ring.phys;
        erst[0].size = XHCI_RING_TRBS;
        rt_write(RT_ERSTSZ, 1);
        rt_write64(RT_ERDP, event_ring.phys | 8);
        rt_write64(RT_ERSTBA, erst_page);
        erdp_phys = event_ring.phys;
        rt_write(RT_IMAN, rt_read(RT_IMAN) & ~1u);

        uint64_t enum_page = physical_memory_alloc_contiguous(1);
        if (!enum_page) {
            continue;
        }
        k_memset((void *)enum_page, 0, 4096);
        enum_buffer = (uint8_t *)enum_page;
        enum_buffer_phys = enum_page;

        op_write(OP_USBCMD, op_read(OP_USBCMD) | USBCMD_RS);

        kernel_log_puts("[xhci] xHCI ");
        kernel_log_put_hex32(cap_read(0) >> 16);
        kernel_log_puts(", ");
        kernel_log_put_dec(max_ports);
        kernel_log_puts(" root ports, ");
        kernel_log_put_dec(max_slots);
        kernel_log_puts(" slots, ");
        kernel_log_put_dec(context_size);
        kernel_log_puts("-byte contexts\n");

        for (uint32_t i = 0; i < 30000000u; i++) {
            __asm__ volatile("pause");
        }

        for (uint32_t port = 1; port <= max_ports; port++) {
            enumerate_port(port);
        }

        started = 1;
    }
    return hid_count;
}

int xhci_device_count(void) {
    return hid_count;
}

static void deliver_keyboard(hid_device_t *d) {
    usb_hid_keys_t keys;
    usb_hid_decode_keyboard(&d->hid, d->report, &keys);
    for (int i = 0; i < keys.count; i++) {
        keyboard_inject(keys.ch[i], keys.mods[i]);
    }
    keyboard_reports++;
}

static void deliver_mouse(hid_device_t *d) {
    usb_hid_mouse_t m;
    usb_hid_decode_mouse(&d->hid, d->report, d->report_length, &m);
    if (m.deliver) {
        mouse_inject(m.dx, m.dy, m.buttons, m.wheel);
    }
    mouse_reports++;
}

void xhci_poll(void) {
    if (!started) {
        return;
    }
    for (int drained = 0; drained < XHCI_RING_TRBS; drained++) {
        xhci_trb_t *e = &event_ring.trb[event_ring.index];
        if ((e->control & TRB_CYCLE) != event_ring.cycle) {
            return;
        }
        uint32_t type = (e->control >> TRB_TYPE_SHIFT) & 0x3F;
        uint8_t slot = (uint8_t)((e->control >> 24) & 0xFF);
        uint8_t dci = (uint8_t)((e->control >> 16) & 0x1F);
        uint32_t code = (e->status >> 24) & 0xFF;

        event_ring.index++;
        if (event_ring.index == XHCI_RING_TRBS) {
            event_ring.index = 0;
            event_ring.cycle ^= 1;
        }
        rt_write64(RT_ERDP, (erdp_phys + event_ring.index * sizeof(xhci_trb_t)) | 8);

        if (type != TRB_EVENT_TRANSFER) {
            continue;
        }
        for (int i = 0; i < hid_count; i++) {
            hid_device_t *d = &hid_devices[i];
            if (d->slot != slot || d->ep_dci != dci) {
                continue;
            }
            if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
                if (d->proto == HID_PROTO_KEYBOARD) {
                    deliver_keyboard(d);
                } else {
                    deliver_mouse(d);
                }
            }
            k_memset(d->report, 0, 8);
            xhci_ring_push(&d->ring, d->report_phys, d->report_length,
                      (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC);
            doorbell(d->slot, d->ep_dci);
            break;
        }
    }
}
