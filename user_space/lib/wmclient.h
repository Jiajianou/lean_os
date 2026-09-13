#pragma once

#include <stdint.h>

#include "gfx.h"
#include "wm.h"

typedef struct {
    int32_t window_id;
    uint32_t width, height;
    gfx_ctx_t gfx;
    int evt_fd;

    uint32_t req_width, req_height, req_panel_dock_h;
    uint8_t req_panel, req_translucent, req_desktop, req_confirm_close;
    char req_title[WM_TITLE_MAX];
    int32_t compositor_pid;
    int32_t shm_id;
    unsigned long shm_bytes;
} wm_window_t;

int wm_connect(uint32_t width, uint32_t height, const char *title, wm_window_t *out);

int wm_connect_panel(uint32_t height, uint32_t dock_h, wm_window_t *out);

int wm_connect_desktop(wm_window_t *out);

int wm_connect_confirm_close(uint32_t width, uint32_t height, const char *title, wm_window_t *out);

int wm_query_windows(wm_query_response_t *out);

int wm_send_action(int32_t window_id, uint32_t action);

int wm_send_action_value(int32_t window_id, uint32_t action, int32_t value);

int wm_set_panel_overhang(int32_t window_id, int32_t rows);

int wm_notify(uint32_t level, const char *title, const char *body);

int wm_drag_begin(const char *payload);

int wm_drag_payload(char *out, uint32_t max);

int wm_toggle_launcher(void);

int wm_veto_shutdown(int32_t window_id);

int wm_set_display_mode(uint32_t width, uint32_t height);
int wm_confirm_display_mode(void);

int wm_set_theme(uint32_t bg_color, uint32_t accent_color, uint32_t wallpaper);

int wm_set_settings(const wm_settings_request_t *in);

int wm_set_taskbar_slot(int32_t window_id, int32_t x, int32_t width);

int wm_query_settings(wm_settings_request_t *out);

int wm_wait_event(wm_window_t *win, wm_event_t *out);

int wm_poll_event(wm_window_t *win, wm_event_t *out);

#define WM_WAIT_CAP_MS 250
int wm_wait_ms(wm_window_t *win, const int *extra_fds, int n_extra, int timeout_ms);

int wm_present(wm_window_t *win);

int wm_reconnect_if_needed(wm_window_t *win);
