#include "hardware_inventory.h"

#include <stdint.h>

#include "architecture/x86_64/ioapic.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"
#include "boot/boot_options.h"
#include "drivers/framebuffer.h"
#include "drivers/i2c_touchpad.h"
#include "drivers/kernel_log.h"
#include "drivers/mouse.h"
#include "drivers/pci.h"
#include "drivers/ps2_controller.h"
#include "memory_management/physical_memory.h"

static void put_hex_byte(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    kernel_log_putc(digits[(value >> 4) & 0xF]);
    kernel_log_putc(digits[value & 0xF]);
}

static void put_hex_word(uint16_t value) {
    put_hex_byte((uint8_t)(value >> 8));
    put_hex_byte((uint8_t)value);
}

static const char *class_name(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    switch (class_code) {
        case 0x01:
            switch (subclass) {
                case 0x01: return "IDE disk controller";
                case 0x06: return (prog_if == 0x01) ? "AHCI disk controller" : "SATA controller";
                case 0x08: return "NVM Express disk controller";
                default: return "mass storage controller";
            }
        case 0x02: return "network controller";
        case 0x03: return "display controller";
        case 0x04: return "multimedia controller";
        case 0x06: return "bridge";
        case 0x07: return "communication controller";
        case 0x08: return "system peripheral";
        case 0x09: return "input device controller";
        case 0x0C:
            switch (subclass) {
                case 0x03: return (prog_if == 0x30) ? "xHCI USB controller" : "USB controller";
                case 0x05: return "SMBus controller";
                case 0x80: return "serial bus controller (I2C on Intel LPSS)";
                default: return "serial bus controller";
            }
        case 0x0D: return "wireless controller";
        case 0x11: return "signal processing controller";
        default: return "device";
    }
}

static void report_pci_device(const pci_device_t *device, void *context) {
    (void)context;
    kernel_log_puts("[inventory] pci ");
    put_hex_byte(device->bus);
    kernel_log_puts(":");
    put_hex_byte(device->slot);
    kernel_log_puts(".");
    kernel_log_put_dec(device->func);
    kernel_log_puts("  ");
    put_hex_word(device->vendor_id);
    kernel_log_puts(":");
    put_hex_word(device->device_id);
    kernel_log_puts("  class ");
    put_hex_byte(device->class_code);
    put_hex_byte(device->subclass);
    put_hex_byte(device->prog_if);
    kernel_log_puts("  ");
    kernel_log_puts(class_name(device->class_code, device->subclass, device->prog_if));
    kernel_log_putc('\n');
}

static void put_cpu_brand(void) {
    uint32_t brand[13];
    for (uint32_t leaf = 0; leaf < 3; leaf++) {
        uint32_t a = 0x80000002u + leaf, b = 0, c = 0, d = 0;
        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
        brand[leaf * 4 + 0] = a;
        brand[leaf * 4 + 1] = b;
        brand[leaf * 4 + 2] = c;
        brand[leaf * 4 + 3] = d;
    }
    brand[12] = 0;

    const char *text = (const char *)brand;
    while (*text == ' ') {
        text++;
    }
    kernel_log_puts(text);
}

static int cpu_supports_brand_string(void) {
    uint32_t a = 0x80000000u, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return a >= 0x80000004u;
}

void hardware_inventory_report(void) {
    const boot_options_t *options = boot_options_active();

    kernel_log_puts("\n[inventory] ---- what this machine turned out to be ----\n");

    kernel_log_puts("[inventory] processor: ");
    if (cpu_supports_brand_string()) {
        put_cpu_brand();
    } else {
        kernel_log_puts("(no brand string)");
    }
    kernel_log_puts(", ");
    kernel_log_put_dec((uint32_t)smp_cpu_count);
    kernel_log_puts(" started of ");
    kernel_log_put_dec((uint32_t)MAX_CPUS);
    kernel_log_puts(" this kernel can hold.\n");

    kernel_log_puts("[inventory] memory: ");
    kernel_log_put_dec((uint32_t)(physical_memory_total_frame_count() / 256));
    kernel_log_puts(" MiB usable, ");
    kernel_log_put_dec((uint32_t)(physical_memory_free_frame_count() / 256));
    kernel_log_puts(" MiB free.\n");

    kernel_log_puts("[inventory] screen: ");
    kernel_log_put_dec(framebuffer_width());
    kernel_log_puts("x");
    kernel_log_put_dec(framebuffer_height());
    kernel_log_puts(" at 0x");
    kernel_log_put_hex64(framebuffer_phys_address());
    kernel_log_puts(", chosen by ");
    kernel_log_puts(options->video_selection == BOOT_VIDEO_LARGEST
                        ? "video=native"
                        : (options->video_selection == BOOT_VIDEO_EXACT ? "video=<width>x<height>"
                                                                        : "the built-in default"));
    kernel_log_puts(".\n");

    kernel_log_puts("[inventory] modes the firmware offers:");
    for (uint32_t i = 0; i < options->offered_count; i++) {
        kernel_log_puts(" ");
        kernel_log_put_dec(options->offered[i][0]);
        kernel_log_puts("x");
        kernel_log_put_dec(options->offered[i][1]);
    }
    kernel_log_puts(options->offered_count ? ".\n" : " none recorded.\n");

    kernel_log_puts("[inventory] interrupts: ");
    kernel_log_puts(ioapic_available() ? "I/O APIC" : "8259 PIC");
    kernel_log_puts(".\n");

    kernel_log_puts("[inventory] pointing and typing: PS/2 controller ");
    uint32_t ports = ps2_controller_ports();
    kernel_log_puts((ports & PS2_CONTROLLER_PRESENT) ? "present" : "absent");
    kernel_log_puts(", keyboard port ");
    kernel_log_puts((ports & PS2_KEYBOARD_PORT_PRESENT) ? "yes" : "no");
    kernel_log_puts(", auxiliary device ");
    kernel_log_puts(mouse_is_present() ? "yes" : "no");
    kernel_log_puts(", I2C touchpad ");
    kernel_log_puts(i2c_touchpad_present() ? "yes" : "no");
    kernel_log_puts(".\n");

    pci_enumerate(report_pci_device, 0);

    if (options->unknown_keys > 0) {
        kernel_log_puts("[inventory] ");
        kernel_log_put_dec(options->unknown_keys);
        kernel_log_puts(" line(s) of \\EFI\\BOOT\\lean_os.cfg were not understood.\n");
    }

    kernel_log_puts("[inventory] ---- end ----\n\n");
}
