#pragma once

#include <stddef.h>

#define CLIPBOARD_MAX 256

void clipboard_set(const void *buf, size_t len);

size_t clipboard_get(void *buf, size_t maxlen);
