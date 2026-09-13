#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} wm_fb_info_t;

#define WM_REQUEST_PIPE  "wm_req"
#define WM_RESPONSE_PIPE "wm_resp"

#define WM_TITLE_MAX 16

#define WM_PANEL_NONE   0
#define WM_PANEL_BOTTOM 1

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t panel_dock_h;
    uint8_t panel;
    uint8_t translucent;
    uint8_t desktop;
    char title[WM_TITLE_MAX];
    int32_t client_pid;
    uint8_t confirm_close;
} wm_create_request_t;

typedef struct {
    int32_t window_id;
    int32_t shm_id;
    uint32_t width, height;
    int32_t compositor_pid;
    int32_t client_pid;
} wm_create_response_t;

typedef enum {
    WM_EVENT_KEY = 1,
    WM_EVENT_MOUSE_MOVE = 2,
    WM_EVENT_MOUSE_BUTTON = 3,
    WM_EVENT_FOCUS = 4,
    WM_EVENT_UNFOCUS = 5,
    WM_EVENT_MOUSE_WHEEL = 7,
    WM_EVENT_DRAG_MOTION = 8,
    WM_EVENT_DROP = 9,
    WM_EVENT_EXPOSE = 10,
    WM_EVENT_QUERY_SHUTDOWN = 12,
    WM_EVENT_DISPLAY_CHANGED = 11,
    WM_EVENT_CLOSE_REQUEST = 6,
} wm_event_type_t;

typedef struct {
    uint32_t type;
    int32_t x, y;
    uint8_t buttons;
    char ch;
    uint32_t time_ms;
    uint8_t mods;
    int32_t wheel;
} wm_event_t;

#define WM_EVENT_PIPE_PREFIX "wm_evt"
#define WM_MAX_ROUTABLE_WINDOWS 12

#define WM_EVENT_PIPE_NAME_LEN 9

static inline void wm_event_pipe_name(int window_id, char out[WM_EVENT_PIPE_NAME_LEN]) {
    out[0] = WM_EVENT_PIPE_PREFIX[0];
    out[1] = WM_EVENT_PIPE_PREFIX[1];
    out[2] = WM_EVENT_PIPE_PREFIX[2];
    out[3] = WM_EVENT_PIPE_PREFIX[3];
    out[4] = WM_EVENT_PIPE_PREFIX[4];
    out[5] = WM_EVENT_PIPE_PREFIX[5];
    out[6] = (char)('0' + (window_id / 10) % 10);
    out[7] = (char)('0' + window_id % 10);
    out[8] = '\0';
}

#define WM_QUERY_PIPE       "wm_query"
#define WM_QUERY_RESP_PIPE  "wm_query_resp"
#define WM_ACTION_PIPE      "wm_action"

typedef struct {
    int32_t window_id;
    int32_t x, y, w, h;
    uint8_t focused;
    uint8_t minimized;
    uint8_t maximized;
    uint8_t is_panel;
    uint8_t is_desktop;
    int32_t z_index;
    int32_t workspace;
    char title[WM_TITLE_MAX];
} wm_window_info_t;

#define WM_WORKSPACE_COUNT 4

typedef struct {
    int32_t count;
    int32_t current_workspace;
    wm_window_info_t windows[WM_MAX_ROUTABLE_WINDOWS];
} wm_query_response_t;

typedef enum {
    WM_ACTION_FOCUS = 1,
    WM_ACTION_TOGGLE_MINIMIZE = 2,
    WM_ACTION_CLOSE = 3,
    WM_ACTION_MAXIMIZE = 4,
    WM_ACTION_RESTORE = 5,
    WM_ACTION_TOGGLE_LAUNCHER = 6,
    WM_ACTION_SNAP_LEFT = 7,
    WM_ACTION_SNAP_RIGHT = 8,
    WM_ACTION_KILL = 9,
    WM_ACTION_SET_PANEL_OVERHANG = 10,
    WM_ACTION_SET_MODE = 11,
    WM_ACTION_SET_TASKBAR_SLOT = 13,
    WM_ACTION_VETO_SHUTDOWN = 14,
    WM_ACTION_CONFIRM_MODE = 12,
    WM_ACTION_PRESENT = 15,
} wm_action_type_t;

static inline int32_t wm_pack_mode(uint32_t width, uint32_t height) {
    return (int32_t)(((width & 0xFFFFu) << 16) | (height & 0xFFFFu));
}
static inline uint32_t wm_mode_width(int32_t value)  { return ((uint32_t)value >> 16) & 0xFFFFu; }
static inline uint32_t wm_mode_height(int32_t value) { return (uint32_t)value & 0xFFFFu; }

#define WM_MODE_REVERT_MS 10000

typedef struct {
    int32_t window_id;
    uint32_t action;
    int32_t value;
} wm_action_request_t;

#define WM_DRAG_PIPE      "wm_drag"
#define WM_DRAG_DATA_PIPE "wm_drag_data"

#define WM_DRAG_PAYLOAD_MAX 28

typedef struct {
    char payload[WM_DRAG_PAYLOAD_MAX];
} wm_drag_request_t;

#define WM_NOTIFY_PIPE "wm_notify"

#define WM_NOTIFY_TITLE_MAX 28
#define WM_NOTIFY_BODY_MAX  40

#define WM_NOTIFY_INFO  0
#define WM_NOTIFY_WARN  1
#define WM_NOTIFY_ERROR 2

typedef struct {
    uint32_t level;
    char title[WM_NOTIFY_TITLE_MAX];
    char body[WM_NOTIFY_BODY_MAX];
} wm_notify_request_t;

#define WM_SETTINGS_PIPE            "wm_settings"
#define WM_SETTINGS_QUERY_PIPE      "wm_set_query"
#define WM_SETTINGS_QUERY_RESP_PIPE "wm_set_qresp"

typedef struct {
    uint32_t volume;
    uint32_t animations;
    uint32_t bg_color;
    uint32_t accent_color;
    uint32_t wallpaper;
} wm_settings_request_t;

#ifdef __cplusplus
}
#endif
