#include <stdio.h>
#include <string.h>

#include "desktop_applications.h"
#include "input.h"
#include "lvgl_theme.h"
#include "syscall_wrappers.h"
#include "wireless.h"

/* Wi-Fi: a wizard in three pages. Choose a network from the ones the radio
   hears; give its password, if it has one and this machine does not already
   know it; watch it join, one step at a time, ending either with the address
   the network gave or with what went wrong said plainly. A wrong password
   comes back to the password page rather than to a dead end. The kernel
   remembers a network once it has joined it, so the second time is one
   click, and the machine rejoins it by itself after a reboot. */

#define WIRELESS_WINDOW_WIDTH  460
#define WIRELESS_WINDOW_HEIGHT 500
#define WIRELESS_REFRESH_MS 400
#define WIRELESS_MINIMUM_DRAWN_PIXELS 15000
#define BAR_COUNT 4
#define STEP_COUNT 3
#define GEOMETRY_ROWS 6

static lvgl_window_t wireless_window;

static os_wireless_status_t status;
static os_wireless_network_t networks[WIRELESS_NETWORKS_MAX];
static int network_count;
static uint32_t shown_generation = 0xFFFFFFFFu;
static int shown_state = -1;

static lv_obj_t *list_page;
static lv_obj_t *password_page;
static lv_obj_t *progress_page;

static lv_obj_t *status_label;
static lv_obj_t *list;
static lv_obj_t *scan_button;
static lv_obj_t *disconnect_button;
static lv_obj_t *forget_button;

static lv_obj_t *password_title;
static lv_obj_t *password_caption;
static lv_obj_t *password_field;
static lv_obj_t *password_hint;
static lv_obj_t *show_password;
static lv_obj_t *remember_network;
static lv_obj_t *join_button;

static lv_obj_t *progress_title;
static lv_obj_t *progress_steps[STEP_COUNT];
static lv_obj_t *progress_spinner;
static lv_obj_t *progress_detail;
static lv_obj_t *progress_button;

static os_wireless_network_t chosen;
static int joining;
static int join_failed;

/* Something the person did that had no other visible effect - choosing a
   network this machine cannot join - is said in the status line, and stays
   there long enough to be read rather than until the next refresh. */
#define NOTICE_MS 4000
static char notice[96];
static long notice_until_ms;

static const char *security_text(uint8_t security) {
    if (security & WIRELESS_SECURITY_ENTERPRISE) {
        return "Enterprise";
    }
    if ((security & WIRELESS_SECURITY_WPA2_PSK) && (security & WIRELESS_SECURITY_WPA3_SAE)) {
        return "WPA2/WPA3";
    }
    if (security & WIRELESS_SECURITY_WPA3_SAE) {
        return "WPA3";
    }
    if (security & WIRELESS_SECURITY_WPA2_PSK) {
        return "WPA2";
    }
    if (security & WIRELESS_SECURITY_WPA_PSK) {
        return "WPA";
    }
    if (security & WIRELESS_SECURITY_WEP) {
        return "WEP";
    }
    return "Open";
}

static int is_secured(uint8_t security) {
    return (security & WIRELESS_SECURITY_OPEN) == 0;
}

/* Four bars, the way every phone draws it: -55 dBm and better is all four,
   and each 10 dB weaker takes one away. */
static int bars_for(int8_t signal) {
    if (signal >= -55) {
        return 4;
    }
    if (signal >= -65) {
        return 3;
    }
    if (signal >= -75) {
        return 2;
    }
    return 1;
}

static const char *error_text(int32_t error) {
    switch (error) {
        case WIRELESS_ERROR_NOT_FOUND:
            return "The network is out of range.";
        case WIRELESS_ERROR_REJECTED:
            return "The network turned this machine away.";
        case WIRELESS_ERROR_WRONG_PASSWORD:
            return "That password is not the network's.";
        case WIRELESS_ERROR_TIMED_OUT:
            return "The network stopped answering.";
        case WIRELESS_ERROR_NO_ADDRESS:
            return "Joined, but the network gave no address.";
        case WIRELESS_ERROR_UNSUPPORTED_SECURITY:
            return "This network's security is not one this machine speaks yet.";
        case WIRELESS_ERROR_DEVICE:
            return "The wireless card stopped working.";
        case WIRELESS_ERROR_BAD_PASSWORD_FORMAT:
            return "A password is 8 to 63 characters, or 64 hexadecimal digits.";
        case WIRELESS_ERROR_DISCONNECTED:
            return "The network disconnected this machine.";
        default:
            return "";
    }
}

static int hexadecimal(const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

static int password_acceptable(const char *password) {
    size_t length = strlen(password);
    return (length >= 8 && length <= 63) || (length == 64 && hexadecimal(password, length));
}

static void show(lv_obj_t *page) {
    lv_obj_t *pages[3] = {list_page, password_page, progress_page};
    for (int i = 0; i < 3; i++) {
        if (pages[i] == page) {
            lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void visible(lv_obj_t *object, int on) {
    if (on) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static void format_address(uint32_t ip, char *out, size_t length) {
    snprintf(out, length, "%u.%u.%u.%u", (unsigned)(ip >> 24) & 255, (unsigned)(ip >> 16) & 255,
             (unsigned)(ip >> 8) & 255, (unsigned)ip & 255);
}

static void set_button_text(lv_obj_t *button, const char *text) {
    lv_label_set_text(lv_obj_get_child(button, 0), text);
}

static void refresh_status_label(void) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    if (notice[0] && sys_uptime_ms() < notice_until_ms) {
        lv_label_set_text(status_label, notice);
        lv_obj_set_style_text_color(status_label, lvgl_theme_color(palette->danger), LV_PART_MAIN);
        return;
    }
    notice[0] = 0;
    char text[160];
    uint32_t color = palette->text_dim;
    switch (status.state) {
        case WIRELESS_STATE_ABSENT:
            snprintf(text, sizeof(text), "This machine has no wireless card this system can drive.");
            break;
        case WIRELESS_STATE_STARTING:
            snprintf(text, sizeof(text), "Starting the wireless card...");
            break;
        case WIRELESS_STATE_RADIO_OFF:
            snprintf(text, sizeof(text), "The radio is switched off.");
            color = palette->danger;
            break;
        case WIRELESS_STATE_SCANNING:
            snprintf(text, sizeof(text), "Looking for networks...");
            break;
        case WIRELESS_STATE_JOINING:
        case WIRELESS_STATE_SECURING:
        case WIRELESS_STATE_ADDRESSING:
            snprintf(text, sizeof(text), "Joining %s...", status.ssid);
            break;
        case WIRELESS_STATE_CONNECTED:
            if (status.ip) {
                char address[20];
                format_address(status.ip, address, sizeof(address));
                snprintf(text, sizeof(text), "Connected to %s  -  %s", status.ssid, address);
            } else {
                snprintf(text, sizeof(text), "Connected to %s", status.ssid);
            }
            color = palette->positive;
            break;
        case WIRELESS_STATE_FAILED:
            snprintf(text, sizeof(text), "%s",
                     error_text(status.error)[0] ? error_text(status.error) : "Not connected.");
            color = palette->danger;
            break;
        default:
            snprintf(text, sizeof(text), "Not connected. Choose a network.");
            break;
    }
    lv_label_set_text(status_label, text);
    lv_obj_set_style_text_color(status_label, lvgl_theme_color(color), LV_PART_MAIN);
}

static void on_network_clicked(lv_event_t *event);
static void report_password_page(void);

static void add_bars(lv_obj_t *row, int8_t signal, uint32_t lit_color, uint32_t unlit_color) {
    lv_obj_t *holder = lv_obj_create(row);
    lv_obj_set_size(holder, 22, 16);
    lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(holder, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(holder, 0, LV_PART_MAIN);
    lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    int lit = bars_for(signal);
    for (int i = 0; i < BAR_COUNT; i++) {
        lv_obj_t *bar = lv_obj_create(holder);
        int height = 4 + i * 4;
        lv_obj_set_size(bar, 4, height);
        lv_obj_set_pos(bar, i * 6, 16 - height);
        lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lvgl_theme_color(i < lit ? lit_color : unlit_color), LV_PART_MAIN);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
}

static lv_obj_t *row_label(lv_obj_t *row, const char *text, uint32_t color, int grow) {
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    if (grow) {
        lv_obj_set_flex_grow(label, 1);
    }
    lv_obj_set_style_text_color(label, lvgl_theme_color(color), LV_PART_MAIN);
    return label;
}

static void rebuild_list(void) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    lv_obj_clean(list);
    if (network_count == 0) {
        const char *empty = status.state == WIRELESS_STATE_ABSENT ? ""
                            : status.state == WIRELESS_STATE_SCANNING ? "Listening..."
                                                                      : "No networks heard yet.";
        row_label(list, empty, palette->text_dim, 0);
    }
    for (int i = 0; i < network_count; i++) {
        const os_wireless_network_t *n = &networks[i];
        int usable = n->supported;
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 36);
        lv_obj_set_style_radius(row, LVGL_THEME_RADIUS, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, n->connected ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lvgl_theme_color(palette->accent), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_30, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_network_clicked, LV_EVENT_CLICKED, (void *)(long)i);

        uint32_t ink = n->connected ? palette->accent_text : (usable ? palette->text : palette->text_dim);
        uint32_t quiet = n->connected ? palette->accent_text : palette->text_dim;
        add_bars(row, n->signal_dbm, ink, n->connected ? palette->accent : palette->outline);
        row_label(row, n->ssid, ink, 1);
        const char *tag = n->connected ? "Connected" : (n->known ? "Saved" : "");
        if (tag[0]) {
            row_label(row, tag, quiet, 0);
        }
        char security[32];
        snprintf(security, sizeof(security), "%s%s",
                 security_text(n->security), usable ? "" : " - not supported");
        row_label(row, security, quiet, 0);
        if (i < GEOMETRY_ROWS) {
            char name[16];
            snprintf(name, sizeof(name), "network%d", i);
            lvgl_window_report_geometry(name, row);
        }
    }
}

static void refresh_footer(void) {
    int connected = status.state == WIRELESS_STATE_CONNECTED;
    int known = 0;
    for (int i = 0; i < network_count; i++) {
        if (networks[i].connected && networks[i].known) {
            known = 1;
        }
    }
    visible(disconnect_button, connected);
    visible(forget_button, known);
    int radio = status.state != WIRELESS_STATE_ABSENT && status.state != WIRELESS_STATE_STARTING;
    visible(scan_button, radio);
    if (connected) {
        lvgl_window_report_geometry("disconnect", disconnect_button);
    }
    if (known) {
        lvgl_window_report_geometry("forget", forget_button);
    }
}

static void mark_step(int index, const char *symbol, uint32_t color, const char *text) {
    char line[96];
    snprintf(line, sizeof(line), "%s   %s", symbol, text);
    lv_label_set_text(progress_steps[index], line);
    lv_obj_set_style_text_color(progress_steps[index], lvgl_theme_color(color), LV_PART_MAIN);
}

static const char *step_text[STEP_COUNT] = {"Joining the network", "Checking the password", "Getting an address"};

/* Each step is waiting, under way, done or failed - an empty circle, the
   accent arrow, a tick, a cross. The current step is the state the kernel
   reports; everything before it is done. */
static void draw_steps(int current, int failed) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    for (int i = 0; i < STEP_COUNT; i++) {
        const char *text = step_text[i];
        if (i == 1 && !is_secured(chosen.security)) {
            text = "No password needed";
        }
        if (i < current) {
            mark_step(i, LV_SYMBOL_OK, palette->positive, text);
        } else if (i == current && failed) {
            mark_step(i, LV_SYMBOL_CLOSE, palette->danger, text);
        } else if (i == current) {
            mark_step(i, LV_SYMBOL_RIGHT, palette->accent, text);
        } else {
            mark_step(i, LV_SYMBOL_MINUS, palette->text_dim, text);
        }
    }
}

static int step_of(int32_t state) {
    switch (state) {
        case WIRELESS_STATE_SECURING:
            return 1;
        case WIRELESS_STATE_ADDRESSING:
            return 2;
        case WIRELESS_STATE_CONNECTED:
            return STEP_COUNT;
        default:
            return 0;
    }
}

static int failed_step;

static void refresh_progress(void) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    if (status.state == WIRELESS_STATE_JOINING || status.state == WIRELESS_STATE_SECURING ||
        status.state == WIRELESS_STATE_ADDRESSING) {
        failed_step = step_of(status.state);
        draw_steps(failed_step, 0);
        return;
    }
    if (status.state == WIRELESS_STATE_CONNECTED) {
        char text[96];
        char address[20];
        draw_steps(STEP_COUNT, 0);
        if (status.ip) {
            format_address(status.ip, address, sizeof(address));
            snprintf(text, sizeof(text), "Connected. This machine is %s on %s.", address, status.ssid);
        } else {
            snprintf(text, sizeof(text), "Connected to %s.", status.ssid);
        }
        lv_label_set_text(progress_detail, text);
        lv_obj_set_style_text_color(progress_detail, lvgl_theme_color(palette->positive), LV_PART_MAIN);
        snprintf(text, sizeof(text), "Connected to %s", status.ssid);
        lv_label_set_text(progress_title, text);
        set_button_text(progress_button, "Done");
        visible(progress_spinner, 0);
        joining = 0;
        return;
    }
    if (status.state == WIRELESS_STATE_FAILED) {
        joining = 0;
        if (status.error == WIRELESS_ERROR_WRONG_PASSWORD || status.error == WIRELESS_ERROR_BAD_PASSWORD_FORMAT) {
            lv_label_set_text(password_hint, error_text(status.error));
            lv_obj_set_style_text_color(password_hint, lvgl_theme_color(palette->danger), LV_PART_MAIN);
            lv_textarea_set_text(password_field, "");
            show(password_page);
            lv_group_focus_obj(password_field);
            report_password_page();
            return;
        }
        join_failed = 1;
        draw_steps(failed_step, 1);
        lv_label_set_text(progress_detail, error_text(status.error));
        lv_obj_set_style_text_color(progress_detail, lvgl_theme_color(palette->danger), LV_PART_MAIN);
        set_button_text(progress_button, "Back");
        visible(progress_spinner, 0);
    }
}

static void refresh(void) {
    os_wireless_status_t latest;
    if (sys_wireless(WIRELESS_OPERATION_STATUS, &latest, 0) != 0) {
        memset(&latest, 0, sizeof(latest));
        latest.state = WIRELESS_STATE_ABSENT;
    }
    status = latest;
    long count = sys_wireless(WIRELESS_OPERATION_NETWORKS, networks, WIRELESS_NETWORKS_MAX);
    if (count < 0) {
        count = 0;
    }
    if (status.scan_generation != shown_generation || status.state != shown_state || (int)count != network_count) {
        network_count = (int)count;
        shown_generation = status.scan_generation;
        shown_state = status.state;
        rebuild_list();
    }
    refresh_status_label();
    refresh_footer();
    if (joining) {
        refresh_progress();
    }
}

static void join(const char *password) {
    os_wireless_connect_t request;
    memset(&request, 0, sizeof(request));
    memcpy(request.ssid, chosen.ssid, chosen.ssid_length);
    request.ssid_length = chosen.ssid_length;
    snprintf(request.password, sizeof(request.password), "%s", password);
    request.remember = lv_obj_has_state(remember_network, LV_STATE_CHECKED) || !password[0];

    const desktop_palette_t *palette = lvgl_theme_palette();
    char title[80];
    snprintf(title, sizeof(title), "Joining %s", chosen.ssid);
    lv_label_set_text(progress_title, title);
    lv_label_set_text(progress_detail, "");
    lv_obj_set_style_text_color(progress_detail, lvgl_theme_color(palette->text_dim), LV_PART_MAIN);
    set_button_text(progress_button, "Cancel");
    visible(progress_spinner, 1);
    failed_step = 0;
    join_failed = 0;
    draw_steps(0, 0);
    show(progress_page);
    lv_obj_update_layout(progress_page);
    lvgl_window_report_geometry("progress_button", progress_button);
    joining = 1;
    if (sys_wireless(WIRELESS_OPERATION_CONNECT, &request, 0) != 0) {
        joining = 0;
        draw_steps(0, 1);
        lv_label_set_text(progress_detail, "The system would not start joining that network.");
        lv_obj_set_style_text_color(progress_detail, lvgl_theme_color(palette->danger), LV_PART_MAIN);
        set_button_text(progress_button, "Back");
        visible(progress_spinner, 0);
    }
    memset(&request, 0, sizeof(request));
}

static void update_join_button(void) {
    if (password_acceptable(lv_textarea_get_text(password_field))) {
        lv_obj_remove_state(join_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(join_button, LV_STATE_DISABLED);
    }
}

static void ask_for_password(void) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    char title[96];
    snprintf(title, sizeof(title), "Join %s", chosen.ssid);
    lv_label_set_text(password_title, title);
    char caption[96];
    snprintf(caption, sizeof(caption), "A %s network. Type its password.", security_text(chosen.security));
    lv_label_set_text(password_caption, caption);
    lv_label_set_text(password_hint, "8 to 63 characters, or a 64-digit hexadecimal key.");
    lv_obj_set_style_text_color(password_hint, lvgl_theme_color(palette->text_dim), LV_PART_MAIN);
    lv_textarea_set_text(password_field, "");
    lv_obj_remove_state(show_password, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(password_field, 1);
    lv_obj_add_state(remember_network, LV_STATE_CHECKED);
    update_join_button();
    show(password_page);
    lv_group_focus_obj(password_field);
    report_password_page();
}

static void on_network_clicked(lv_event_t *event) {
    int index = (int)(long)lv_event_get_user_data(event);
    if (index < 0 || index >= network_count) {
        return;
    }
    chosen = networks[index];
    if (chosen.connected) {
        return;
    }
    if (!chosen.supported) {
        snprintf(notice, sizeof(notice), "%s cannot be joined: %s", chosen.ssid,
                 (chosen.security & WIRELESS_SECURITY_WPA3_SAE) ? "WPA3 is not supported yet."
                                                                : "its security is not supported yet.");
        notice_until_ms = sys_uptime_ms() + NOTICE_MS;
        refresh_status_label();
        return;
    }
    notice[0] = 0;
    if (is_secured(chosen.security) && !chosen.known) {
        ask_for_password();
    } else {
        join("");
    }
}

static void on_join(lv_event_t *event) {
    (void)event;
    const char *password = lv_textarea_get_text(password_field);
    if (!password_acceptable(password)) {
        const desktop_palette_t *palette = lvgl_theme_palette();
        lv_label_set_text(password_hint, error_text(WIRELESS_ERROR_BAD_PASSWORD_FORMAT));
        lv_obj_set_style_text_color(password_hint, lvgl_theme_color(palette->danger), LV_PART_MAIN);
        return;
    }
    join(password);
    lv_textarea_set_text(password_field, "");
}

static void on_password_changed(lv_event_t *event) {
    (void)event;
    update_join_button();
}

static void on_back(lv_event_t *event) {
    (void)event;
    lv_textarea_set_text(password_field, "");
    show(list_page);
}

static void on_progress_button(lv_event_t *event) {
    (void)event;
    if (joining) {
        sys_wireless(WIRELESS_OPERATION_DISCONNECT, 0, 0);
        joining = 0;
    }
    show(list_page);
    shown_generation = 0xFFFFFFFFu;
    refresh();
}

static void on_scan(lv_event_t *event) {
    (void)event;
    sys_wireless(WIRELESS_OPERATION_SCAN, 0, 0);
    refresh();
}

static void on_disconnect(lv_event_t *event) {
    (void)event;
    sys_wireless(WIRELESS_OPERATION_DISCONNECT, 0, 0);
    shown_generation = 0xFFFFFFFFu;
    refresh();
}

static void on_forget(lv_event_t *event) {
    (void)event;
    for (int i = 0; i < network_count; i++) {
        if (networks[i].connected) {
            sys_wireless(WIRELESS_OPERATION_FORGET, &networks[i], 0);
        }
    }
    shown_generation = 0xFFFFFFFFu;
    refresh();
}

static void on_show_password(lv_event_t *event) {
    (void)event;
    int checked = lv_obj_has_state(show_password, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(password_field, !checked);
}

static int on_key(lvgl_window_t *window, uint32_t ch, uint32_t mods) {
    (void)window;
    (void)mods;
    int on_password = !lv_obj_has_flag(password_page, LV_OBJ_FLAG_HIDDEN);
    if ((ch == '\n' || ch == '\r') && on_password) {
        on_join(0);
        return 1;
    }
    if (ch == 27 && on_password) {
        on_back(0);
        return 1;
    }
    return 0;
}

static void on_tick(lv_timer_t *timer) {
    (void)timer;
    refresh();
}

static lv_obj_t *page(lv_obj_t *root) {
    lv_obj_t *p = lv_obj_create(root);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(p, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *heading(lv_obj_t *parent, const char *text) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lvgl_theme_color(palette->text), LV_PART_MAIN);
    return label;
}

static lv_obj_t *spacer(lv_obj_t *parent) {
    lv_obj_t *gap = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(gap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(gap, 0, LV_PART_MAIN);
    lv_obj_set_width(gap, LV_PCT(100));
    lv_obj_set_flex_grow(gap, 1);
    lv_obj_remove_flag(gap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return gap;
}

static lv_obj_t *footer(lv_obj_t *parent) {
    lv_obj_t *row = lvgl_theme_row(parent, NULL);
    lv_obj_t *gap = lvgl_theme_caption(row, "");
    lv_obj_set_flex_grow(gap, 1);
    return row;
}

static void build_list_page(lv_obj_t *root) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    list_page = page(root);
    heading(list_page, "Choose a network");
    status_label = lvgl_theme_caption(list_page, "");
    lv_obj_set_width(status_label, LV_PCT(100));
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);

    list = lv_obj_create(list_page);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_color(list, lvgl_theme_color(palette->surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, lvgl_theme_color(palette->outline), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list, LVGL_THEME_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *row = footer(list_page);
    forget_button = lvgl_theme_button(row, "Forget", 0);
    lv_obj_add_event_cb(forget_button, on_forget, LV_EVENT_CLICKED, NULL);
    disconnect_button = lvgl_theme_button(row, "Disconnect", 0);
    lv_obj_add_event_cb(disconnect_button, on_disconnect, LV_EVENT_CLICKED, NULL);
    scan_button = lvgl_theme_button(row, "Scan Again", 1);
    lv_obj_add_event_cb(scan_button, on_scan, LV_EVENT_CLICKED, NULL);
    lvgl_window_report_geometry("network_list", list);
    lvgl_window_report_geometry("scan", scan_button);
}

static void build_password_page(lv_obj_t *root) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    password_page = page(root);
    password_title = heading(password_page, "");
    password_caption = lvgl_theme_caption(password_page, "");

    password_field = lv_textarea_create(password_page);
    lv_obj_set_width(password_field, LV_PCT(100));
    lv_textarea_set_one_line(password_field, 1);
    lv_textarea_set_password_mode(password_field, 1);
    lv_textarea_set_max_length(password_field, WIRELESS_PASSWORD_MAX);
    lv_textarea_set_placeholder_text(password_field, "Password");
    lv_obj_add_event_cb(password_field, on_password_changed, LV_EVENT_VALUE_CHANGED, NULL);

    password_hint = lvgl_theme_caption(password_page, "");
    lv_obj_set_width(password_hint, LV_PCT(100));
    lv_label_set_long_mode(password_hint, LV_LABEL_LONG_WRAP);

    show_password = lv_checkbox_create(password_page);
    lv_checkbox_set_text(show_password, "Show password");
    lv_obj_set_style_text_color(show_password, lvgl_theme_color(palette->text), LV_PART_MAIN);
    lv_obj_add_event_cb(show_password, on_show_password, LV_EVENT_VALUE_CHANGED, NULL);

    remember_network = lv_checkbox_create(password_page);
    lv_checkbox_set_text(remember_network, "Remember this network and join it automatically");
    lv_obj_set_style_text_color(remember_network, lvgl_theme_color(palette->text), LV_PART_MAIN);

    spacer(password_page);
    lv_obj_t *row = footer(password_page);
    lv_obj_t *back = lvgl_theme_button(row, "Back", 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    join_button = lvgl_theme_button(row, "Join", 1);
    lv_obj_add_event_cb(join_button, on_join, LV_EVENT_CLICKED, NULL);
}

/* Where things are is only known once their page is the one laid out - the
   pages share the window - so each page says so when it is shown. */
static void report_password_page(void) {
    lv_obj_update_layout(password_page);
    lvgl_window_report_geometry("password", password_field);
    lvgl_window_report_geometry("show_password", show_password);
    lvgl_window_report_geometry("remember", remember_network);
    lvgl_window_report_geometry("join", join_button);
}

static void build_progress_page(lv_obj_t *root) {
    progress_page = page(root);
    progress_title = heading(progress_page, "");
    for (int i = 0; i < STEP_COUNT; i++) {
        progress_steps[i] = lvgl_theme_caption(progress_page, "");
    }
    progress_spinner = lv_spinner_create(progress_page);
    lv_spinner_set_anim_params(progress_spinner, 1000, 200);
    lv_obj_set_size(progress_spinner, 36, 36);
    progress_detail = lvgl_theme_caption(progress_page, "");
    lv_obj_set_width(progress_detail, LV_PCT(100));
    lv_label_set_long_mode(progress_detail, LV_LABEL_LONG_WRAP);
    spacer(progress_page);
    lv_obj_t *row = footer(progress_page);
    progress_button = lvgl_theme_button(row, "Cancel", 1);
    lv_obj_add_event_cb(progress_button, on_progress_button, LV_EVENT_CLICKED, NULL);
}

static unsigned long count_drawn_pixels(void) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    const uint32_t *pixels = wireless_window.window.graphics.pixels;
    unsigned long total = (unsigned long)wireless_window.window.width * wireless_window.window.height;
    unsigned long drawn = 0;
    for (unsigned long i = 0; i < total; i++) {
        if ((pixels[i] & 0x00FFFFFFu) != palette->window) {
            drawn++;
        }
    }
    return drawn;
}

int desktop_application_wireless(int selftest) {
    if (lvgl_window_open(WIRELESS_WINDOW_WIDTH, WIRELESS_WINDOW_HEIGHT, "Wi-Fi", &wireless_window) != 0) {
        printf("[wifi] window open failed\n");
        return 1;
    }
    lvgl_theme_apply(&wireless_window);
    lvgl_window_set_key_handler(&wireless_window, on_key);

    lv_obj_t *root = lvgl_theme_page(lv_screen_active());
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    build_list_page(root);
    build_password_page(root);
    build_progress_page(root);
    show(list_page);
    refresh();
    if (status.state != WIRELESS_STATE_ABSENT && status.state != WIRELESS_STATE_STARTING &&
        status.state != WIRELESS_STATE_CONNECTED) {
        sys_wireless(WIRELESS_OPERATION_SCAN, 0, 0);
        refresh();
    }
    lv_timer_create(on_tick, WIRELESS_REFRESH_MS, NULL);

    for (int frame = 0; frame < 8; frame++) {
        lv_timer_handler();
        lvgl_window_pump(&wireless_window, 16);
    }
    unsigned long drawn = count_drawn_pixels();
    printf("[wifi] the Wi-Fi window rendered %lu pixels\n", drawn);
    if (drawn < WIRELESS_MINIMUM_DRAWN_PIXELS) {
        lvgl_window_close(&wireless_window);
        return 1;
    }
    if (selftest) {
        lvgl_window_close(&wireless_window);
        return 0;
    }
    lvgl_window_run(&wireless_window);
    lvgl_window_close(&wireless_window);
    return 0;
}
