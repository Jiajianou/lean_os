#pragma once

#define HARDWARE_INVENTORY_BRAND_MAX 49

void hardware_inventory_report(void);

int hardware_inventory_cpu_brand(char out[HARDWARE_INVENTORY_BRAND_MAX]);
