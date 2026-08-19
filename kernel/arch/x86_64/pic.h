/* kernel/arch/x86_64/pic.h
 *
 * 8259 PIC driver: remaps IRQ0-15 off the CPU-exception vectors they
 * collide with at boot (0x08-0x0F) onto 0x20-0x2F, and gives drivers a
 * mask/unmask/EOI interface. pic_remap() masks every line - M6's drivers
 * unmask their own IRQ as they come online.
 */
#pragma once

#include <stdint.h>

#define PIC_IRQ_VECTOR_OFFSET 0x20

void pic_remap(void);
void pic_send_eoi(uint8_t irq);
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);
