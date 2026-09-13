#pragma once

#include <stdint.h>

int ahci_init(void);


int ahci_read(uint64_t lba, uint32_t count, void *buf);
int ahci_write(uint64_t lba, uint32_t count, const void *buf);

