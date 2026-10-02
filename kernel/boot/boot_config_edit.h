#pragma once

#include <stdint.h>

#define BOOT_CONFIG_HEADER "# lean_os boot options"

int boot_config_is_ours(const char *text, uint32_t length);

uint32_t boot_config_content_length(const char *text, uint32_t length);

int boot_config_set(char *text, uint32_t capacity, const char *key, const char *value);
