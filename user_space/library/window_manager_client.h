#pragma once

#include <stdint.h>

#include "graphics.h"
#include "window_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t window_id;
    uint32_t width, height;
    graphics_context_t graphics;
    int evt_file_descriptor;

    uint32_t request_width, request_height, request_panel_dock_h;
    uint8_t request_panel, request_translucent, request_desktop, request_confirm_close;
    char request_title[WINDOW_MANAGER_TITLE_MAX];
    int32_t compositor_pid;
    int32_t shared_memory_id;
    unsigned long shared_memory_bytes;
} window_manager_window_t;

int window_manager_connect(uint32_t width, uint32_t height, const char *title, window_manager_window_t *out);

int window_manager_connect_panel(uint32_t height, uint32_t dock_h, window_manager_window_t *out);

int window_manager_connect_desktop(window_manager_window_t *out);

int window_manager_connect_confirm_close(uint32_t width, uint32_t height, const char *title, window_manager_window_t *out);

int window_manager_connect_popup(int32_t parent_window_id, int32_t x, int32_t y, uint32_t width, uint32_t height, window_manager_window_t *out);

int window_manager_set_capture(window_manager_window_t *win, int captured);

int window_manager_query_windows(window_manager_query_response_t *out);

int window_manager_send_action(int32_t window_id, uint32_t action);

int window_manager_send_action_value(int32_t window_id, uint32_t action, int32_t value);

int window_manager_set_panel_overhang(int32_t window_id, int32_t rows);

int window_manager_notify(uint32_t level, const char *title, const char *body);

int window_manager_drag_begin(const char *payload);

int window_manager_drag_payload(char *out, uint32_t max);

int window_manager_toggle_launcher(void);

int window_manager_veto_shutdown(int32_t window_id);

int window_manager_set_display_mode(uint32_t width, uint32_t height);
int window_manager_confirm_display_mode(void);

int window_manager_set_theme(uint32_t bg_color, uint32_t accent_color, uint32_t wallpaper);

int window_manager_set_settings(const window_manager_settings_request_t *in);

int window_manager_set_taskbar_slot(int32_t window_id, int32_t x, int32_t width);

int window_manager_query_settings(window_manager_settings_request_t *out);

int window_manager_wait_event(window_manager_window_t *win, window_manager_event_t *out);

int window_manager_poll_event(window_manager_window_t *win, window_manager_event_t *out);

#define WINDOW_MANAGER_WAIT_CAP_MS 250
int window_manager_wait_ms(window_manager_window_t *win, const int *extra_file_descriptors, int n_extra, int timeout_ms);

int window_manager_present(window_manager_window_t *win);
/* M200: present only the part of the window that changed - a caret, a
   spinner, a hover - so the compositor recomposites that and not the whole
   window. */
int window_manager_present_rect(window_manager_window_t *win, int32_t x, int32_t y, int32_t w, int32_t h);
int window_manager_send_damage(int32_t window_id, int32_t x, int32_t y, int32_t w, int32_t h);

int window_manager_reconnect_if_needed(window_manager_window_t *win);

#ifdef __cplusplus
}
#endif
