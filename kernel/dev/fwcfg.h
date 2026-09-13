#pragma once

#include <stdint.h>

void fwcfg_init(void);

int fwcfg_present(void);

int fwcfg_read_file(const char *name, void *dst, uint32_t max);

int boot_selftests_enabled(void);

int boot_ioapic_enabled(void);

int boot_bootstrap_enabled(void);

int boot_pytest_enabled(void);

int boot_pybuild_enabled(void);
