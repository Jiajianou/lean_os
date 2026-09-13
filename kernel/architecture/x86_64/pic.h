#pragma once

#include <stdint.h>

#define PIC_IRQ_VECTOR_OFFSET 0x20

void pic_remap(void);
void pic_send_eoi(uint8_t irq);
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);
