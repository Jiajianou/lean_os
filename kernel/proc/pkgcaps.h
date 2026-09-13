#pragma once

#include <stdint.h>

#define PKG_REGISTRY_PATH "/pkg/db/caps"

#define PKG_REGISTRY_MAX 32768

uint32_t pkg_caps_for_path(const char *path);

uint32_t caps_for_spawn_path(const char *path);

void pkg_registry_invalidate(void);

int pkg_registry_count(void);
