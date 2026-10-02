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

/* M213: SYS_display(operation, a, b). One call, so the next operation is not
   a new number. STATUS fills a display_status_t (a = its address, b = its
   size); SET_SCALE takes a percent, 0 for the automatic choice; SET_STARTUP_MODE
   takes a width and a height, 0 and 0 for the panel's own mode, and rewrites
   video= in \EFI\BOOT\lean_os.cfg for the next boot. The two that change
   something need CAP_DISPLAY_MODE. */
#define DISPLAY_OPERATION_STATUS           0
#define DISPLAY_OPERATION_SET_SCALE        1
#define DISPLAY_OPERATION_SET_STARTUP_MODE 2

#define DISPLAY_SCALES_MAX         8
#define DISPLAY_FIRMWARE_MODES_MAX 32

#define DISPLAY_STARTUP_UNKNOWN  0
#define DISPLAY_STARTUP_BUILT_IN 1
#define DISPLAY_STARTUP_LARGEST  2
#define DISPLAY_STARTUP_EXACT    3
#define DISPLAY_STARTUP_FIRMWARE 4

typedef struct {
    uint32_t physical_width;
    uint32_t physical_height;
    uint32_t desktop_width;
    uint32_t desktop_height;
    uint32_t scale_percent;
    uint32_t scale_requested;
    uint32_t scale_automatic;
    uint32_t scale_count;
    uint32_t scales[DISPLAY_SCALES_MAX];
    uint32_t live_modes;
    uint32_t startup_writable;
    uint32_t startup_selection;
    uint32_t startup_width;
    uint32_t startup_height;
    uint32_t firmware_mode_count;
    display_mode_t firmware_modes[DISPLAY_FIRMWARE_MODES_MAX];
} display_status_t;

#ifdef __cplusplus
}
#endif
