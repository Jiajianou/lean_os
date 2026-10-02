#pragma once

#include <stdint.h>

#include "boot/boot_options.h"

int boot_config_available(void);

int boot_config_startup(boot_options_t *out);

int boot_config_set_startup_mode(uint32_t width, uint32_t height);
