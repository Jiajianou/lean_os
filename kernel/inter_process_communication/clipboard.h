#pragma once

#include <stddef.h>

#define CLIPBOARD_MAX 256

void clipboard_set(const void *buffer, size_t length);

size_t clipboard_get(void *buffer, size_t maxlen);
