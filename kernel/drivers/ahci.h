#pragma once

#include <stdint.h>

int ahci_init(void);


int ahci_read(uint64_t lba, uint32_t count, void *buffer);
int ahci_write(uint64_t lba, uint32_t count, const void *buffer);

