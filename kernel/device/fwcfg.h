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

/* Chromium's own browser, rendering a page and nothing else. The battery it
   lives in is 470 seconds before it reaches the browser at all, and what a
   browser needs graded - did a page come out the far side as pixels - is one
   run of one program. This switch is that run. */
int boot_browser_enabled(void);

/* What a read costs, on its own. M170: a boot that runs one benchmark and
   stops is a minute; the battery that found the regression is twelve. */
int boot_readbench_enabled(void);
