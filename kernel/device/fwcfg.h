#pragma once

#include <stdint.h>

void fwcfg_init(void);

int fwcfg_present(void);

int fwcfg_read_file(const char *name, void *destination, uint32_t max);

int boot_selftests_enabled(void);

int boot_ioapic_enabled(void);

int boot_bootstrap_enabled(void);

/* Fork out of a process with sibling threads is only interesting on a machine
   with more than one core, and the graded battery boots one. This switch runs
   that one self-test and nothing else, so it can be booted with four without
   waiting for - or tripping over - everything else in the battery. */
int boot_forksmp_enabled(void);

int boot_pytest_enabled(void);

int boot_pybuild_enabled(void);
