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
} window_manager_framebuffer_info_t;

#define WINDOW_MANAGER_FRAMEBUFFER_PHYSICAL 1

#define WINDOW_MANAGER_REQUEST_PIPE  "wm_req"
#define WINDOW_MANAGER_RESPONSE_PIPE "wm_resp"

#define WINDOW_MANAGER_TITLE_MAX 16

#define WINDOW_MANAGER_PANEL_NONE   0
#define WINDOW_MANAGER_PANEL_BOTTOM 1

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t panel_dock_h;
    uint8_t panel;
    uint8_t translucent;
    uint8_t desktop;
    char title[WINDOW_MANAGER_TITLE_MAX];
    int32_t client_pid;
    uint8_t confirm_close;
    /* A popup is a window the CLIENT places: no title bar, no taskbar slot,
       no focus of its own, above every ordinary window, at an offset from
       its parent window's client origin, and gone with its parent. It is
       what a menu, a tooltip or a select's list is to a toolkit (M174). */
    uint8_t popup;
    int32_t parent_window_id;
    int32_t popup_x;
    int32_t popup_y;
} window_manager_create_request_t;

typedef struct {
    int32_t window_id;
    int32_t shared_memory_id;
    uint32_t width, height;
    int32_t compositor_pid;
    int32_t client_pid;
} window_manager_create_response_t;

typedef enum {
    WINDOW_MANAGER_EVENT_KEY = 1,
    WINDOW_MANAGER_EVENT_MOUSE_MOVE = 2,
    WINDOW_MANAGER_EVENT_MOUSE_BUTTON = 3,
    WINDOW_MANAGER_EVENT_FOCUS = 4,
    WINDOW_MANAGER_EVENT_UNFOCUS = 5,
    WINDOW_MANAGER_EVENT_MOUSE_WHEEL = 7,
    WINDOW_MANAGER_EVENT_DRAG_MOTION = 8,
    WINDOW_MANAGER_EVENT_DROP = 9,
    WINDOW_MANAGER_EVENT_EXPOSE = 10,
    WINDOW_MANAGER_EVENT_QUERY_SHUTDOWN = 12,
    WINDOW_MANAGER_EVENT_DISPLAY_CHANGED = 11,
    WINDOW_MANAGER_EVENT_CLOSE_REQUEST = 6,
} window_manager_event_type_t;

typedef struct {
    uint32_t type;
    int32_t x, y;
    uint8_t buttons;
    char ch;
    uint32_t time_ms;
    uint8_t mods;
    int32_t wheel;
} window_manager_event_t;

#define WINDOW_MANAGER_EVENT_PIPE_PREFIX "wm_evt"
#define WINDOW_MANAGER_MAX_ROUTABLE_WINDOWS 12

#define WINDOW_MANAGER_EVENT_PIPE_NAME_LENGTH 9

static inline void window_manager_event_pipe_name(int window_id, char out[WINDOW_MANAGER_EVENT_PIPE_NAME_LENGTH]) {
    out[0] = WINDOW_MANAGER_EVENT_PIPE_PREFIX[0];
    out[1] = WINDOW_MANAGER_EVENT_PIPE_PREFIX[1];
    out[2] = WINDOW_MANAGER_EVENT_PIPE_PREFIX[2];
    out[3] = WINDOW_MANAGER_EVENT_PIPE_PREFIX[3];
    out[4] = WINDOW_MANAGER_EVENT_PIPE_PREFIX[4];
    out[5] = WINDOW_MANAGER_EVENT_PIPE_PREFIX[5];
    out[6] = (char)('0' + (window_id / 10) % 10);
    out[7] = (char)('0' + window_id % 10);
    out[8] = '\0';
}

#define WINDOW_MANAGER_QUERY_PIPE       "wm_query"
#define WINDOW_MANAGER_QUERY_RESPONSE_PIPE  "wm_query_resp"
#define WINDOW_MANAGER_ACTION_PIPE      "wm_action"

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
    char title[WINDOW_MANAGER_TITLE_MAX];
} window_manager_window_info_t;

#define WINDOW_MANAGER_WORKSPACE_COUNT 4

typedef struct {
    int32_t count;
    int32_t current_workspace;
    window_manager_window_info_t windows[WINDOW_MANAGER_MAX_ROUTABLE_WINDOWS];
} window_manager_query_response_t;

typedef enum {
    WINDOW_MANAGER_ACTION_FOCUS = 1,
    WINDOW_MANAGER_ACTION_TOGGLE_MINIMIZE = 2,
    WINDOW_MANAGER_ACTION_CLOSE = 3,
    WINDOW_MANAGER_ACTION_MAXIMIZE = 4,
    WINDOW_MANAGER_ACTION_RESTORE = 5,
    WINDOW_MANAGER_ACTION_TOGGLE_LAUNCHER = 6,
    WINDOW_MANAGER_ACTION_SNAP_LEFT = 7,
    WINDOW_MANAGER_ACTION_SNAP_RIGHT = 8,
    WINDOW_MANAGER_ACTION_KILL = 9,
    WINDOW_MANAGER_ACTION_SET_PANEL_OVERHANG = 10,
    WINDOW_MANAGER_ACTION_SET_MODE = 11,
    WINDOW_MANAGER_ACTION_SET_TASKBAR_SLOT = 13,
    WINDOW_MANAGER_ACTION_VETO_SHUTDOWN = 14,
    WINDOW_MANAGER_ACTION_CONFIRM_MODE = 12,
    WINDOW_MANAGER_ACTION_PRESENT = 15,
    /* Every mouse event goes to this window, wherever the cursor is, with
       coordinates relative to it, until it is released - how a menu learns
       about a click outside itself. */
    WINDOW_MANAGER_ACTION_CAPTURE = 16,
    WINDOW_MANAGER_ACTION_RELEASE_CAPTURE = 17,
} window_manager_action_type_t;

static inline int32_t window_manager_pack_mode(uint32_t width, uint32_t height) {
    return (int32_t)(((width & 0xFFFFu) << 16) | (height & 0xFFFFu));
}
static inline uint32_t window_manager_mode_width(int32_t value)  { return ((uint32_t)value >> 16) & 0xFFFFu; }
static inline uint32_t window_manager_mode_height(int32_t value) { return (uint32_t)value & 0xFFFFu; }

#define WINDOW_MANAGER_MODE_REVERT_MS 10000

typedef struct {
    int32_t window_id;
    uint32_t action;
    int32_t value;
} window_manager_action_request_t;

#define WINDOW_MANAGER_DRAG_PIPE      "wm_drag"
#define WINDOW_MANAGER_DRAG_DATA_PIPE "wm_drag_data"

#define WINDOW_MANAGER_DRAG_PAYLOAD_MAX 28

typedef struct {
    char payload[WINDOW_MANAGER_DRAG_PAYLOAD_MAX];
} window_manager_drag_request_t;

#define WINDOW_MANAGER_NOTIFY_PIPE "wm_notify"

#define WINDOW_MANAGER_NOTIFY_TITLE_MAX 28
#define WINDOW_MANAGER_NOTIFY_BODY_MAX  40

#define WINDOW_MANAGER_NOTIFY_INFO  0
#define WINDOW_MANAGER_NOTIFY_WARN  1
#define WINDOW_MANAGER_NOTIFY_ERROR 2

typedef struct {
    uint32_t level;
    char title[WINDOW_MANAGER_NOTIFY_TITLE_MAX];
    char body[WINDOW_MANAGER_NOTIFY_BODY_MAX];
} window_manager_notify_request_t;

#define WINDOW_MANAGER_SETTINGS_PIPE            "wm_settings"
#define WINDOW_MANAGER_SETTINGS_QUERY_PIPE      "wm_set_query"
#define WINDOW_MANAGER_SETTINGS_QUERY_RESPONSE_PIPE "wm_set_qresp"

typedef struct {
    uint32_t volume;
    uint32_t animations;
    uint32_t bg_color;
    uint32_t accent_color;
    uint32_t wallpaper;
} window_manager_settings_request_t;

#ifdef __cplusplus
}
#endif
