#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_MAX_MODES 12

typedef struct {
    uint32_t width;
    uint32_t height;
} display_mode_t;

#ifdef __cplusplus
}
#endif
