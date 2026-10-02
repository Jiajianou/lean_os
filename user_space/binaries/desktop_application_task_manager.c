#include <stdio.h>
#include <string.h>

#include "desktop_applications.h"
#include "input.h"
#include "lvgl_theme.h"
#include "signal.h"
#include "syscall_wrappers.h"

#define TASK_MANAGER_WINDOW_WIDTH  520
#define TASK_MANAGER_WINDOW_HEIGHT 420

#define TASK_MANAGER_ROW_HEIGHT 26
#define TASK_MANAGER_REFRESH_MS 1000
#define TASK_MANAGER_MINIMUM_DRAWN_PIXELS 20000

#define COLUMN_PID_WIDTH   52
#define COLUMN_STATE_WIDTH 76
#define COLUMN_PARENT_WIDTH 56
#define COLUMN_RESOURCE_WIDTH 64

static const char *const PROTECTED_NAMES[] = {"init", "kernel", "cpu-idle"};
#define PROTECTED_NAME_COUNT ((int)(sizeof(PROTECTED_NAMES) / sizeof(PROTECTED_NAMES[0])))

static lvgl_window_t task_manager_window;

static task_info_t tasks[TASK_INFO_MAX];
static task_info_t every_task[TASK_INFO_MAX];
static int task_count;
static int show_system_tasks;
static int selected = -1;
static long selected_pid = -1;

#define ROW_CELL_COUNT 5

static lv_obj_t *list;
static lv_obj_t *rows[TASK_INFO_MAX];
static lv_obj_t *row_cells[TASK_INFO_MAX][ROW_CELL_COUNT];
static lv_obj_t *status_label;

static int is_protected(const char *name) {
    for (int i = 0; i < PROTECTED_NAME_COUNT; i++) {
        if (strcmp(name, PROTECTED_NAMES[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *state_name(int32_t state) {
    if (state == TASK_INFO_TERMINATED) {
        return "exited";
    }
    return state == TASK_INFO_RUNNING ? "running" : "ready";
}

static void set_status(const char *text, uint32_t color) {
    lv_label_set_text(status_label, text);
    lv_obj_set_style_text_color(status_label, lvgl_theme_color(color), LV_PART_MAIN);
}

static void on_row_clicked(lv_event_t *event);

static lv_obj_t *row_cell(lv_obj_t *row, const char *text, int32_t width, uint32_t color) {
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    if (width > 0) {
        lv_obj_set_width(label, width);
    } else {
        lv_obj_set_flex_grow(label, 1);
    }
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lvgl_theme_color(color), LV_PART_MAIN);
    return label;
}

static lv_obj_t *make_row(lv_obj_t *parent, int index) {
    static const int32_t CELL_WIDTHS[ROW_CELL_COUNT] = {
        COLUMN_PID_WIDTH, 0, COLUMN_STATE_WIDTH, COLUMN_PARENT_WIDTH, COLUMN_RESOURCE_WIDTH,
    };
    const desktop_palette_t *palette = lvgl_theme_palette();
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, TASK_MANAGER_ROW_HEIGHT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lvgl_theme_color(palette->accent), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, (void *)(long)index);
    for (int i = 0; i < ROW_CELL_COUNT; i++) {
        row_cells[index][i] = row_cell(row, "", CELL_WIDTHS[i], lvgl_theme_palette()->text);
    }
    return row;
}

static void fill_row(int index, const task_info_t *task, int is_selected) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    lv_obj_t *row = rows[index];
    int dead = (task->state == TASK_INFO_TERMINATED);
    uint32_t color = is_selected ? palette->accent_text : (dead ? palette->text_dim : palette->text);
    char text[32];

    snprintf(text, sizeof(text), "%d", (int)task->pid);
    lv_label_set_text(row_cells[index][0], text);
    lv_label_set_text(row_cells[index][1], task->name[0] ? task->name : "?");
    lv_label_set_text(row_cells[index][2], state_name(task->state));
    snprintf(text, sizeof(text), "%d", (int)task->parent_pid);
    lv_label_set_text(row_cells[index][3], text);
    snprintf(text, sizeof(text), "%d/%d", (int)task->open_file_descriptors,
             (int)task->shared_memory_segments);
    lv_label_set_text(row_cells[index][4], text);
    for (int i = 0; i < ROW_CELL_COUNT; i++) {
        lv_obj_set_style_text_color(row_cells[index][i], lvgl_theme_color(color), LV_PART_MAIN);
    }

    if (is_selected) {
        lv_obj_add_state(row, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(row, LV_STATE_CHECKED);
    }
}

static void refresh_rows(void) {
    for (int i = 0; i < task_count; i++) {
        if (!rows[i]) {
            rows[i] = make_row(list, i);
        }
        fill_row(i, &tasks[i], i == selected);
        lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = task_count; i < TASK_INFO_MAX; i++) {
        if (rows[i]) {
            lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void select_row(int index) {
    selected = index;
    selected_pid = (index >= 0 && index < task_count) ? (long)tasks[index].pid : -1;
}

/* M220. The table used to open on sixteen rows of "idle" - the kernel's own
   tasks, one per processor, which nobody can end - with the programs a
   person opened this window to find below the fold. Three groups now: what
   a person started, newest at the top; the desktop's own processes; and,
   only when asked for, the kernel's tasks, which have no parent process. */
static const char *const DESKTOP_PARTS[] = {"init", "compositor", "desktop_shell", "desktop_icons"};

static int task_group(const task_info_t *task) {
    if (task->parent_pid < 0) {
        return 2;
    }
    for (int i = 0; i < (int)(sizeof(DESKTOP_PARTS) / sizeof(DESKTOP_PARTS[0])); i++) {
        if (strcmp(task->name, DESKTOP_PARTS[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void order_tasks(int total) {
    task_count = 0;
    for (int group = 0; group < 3; group++) {
        if (group == 2 && !show_system_tasks) {
            break;
        }
        int start = task_count;
        for (int i = 0; i < total; i++) {
            if (task_group(&every_task[i]) != group) {
                continue;
            }
            int at = task_count++;
            while (at > start && (group == 0 ? tasks[at - 1].pid < every_task[i].pid
                                             : tasks[at - 1].pid > every_task[i].pid)) {
                tasks[at] = tasks[at - 1];
                at--;
            }
            tasks[at] = every_task[i];
        }
    }
}

static void refresh_tasks(void) {
    long n = sys_taskinfo(every_task, TASK_INFO_MAX);
    order_tasks(n > 0 ? (int)n : 0);
    /* The selection is a process, not a row. The table is read again every
       tick, a process that exits above the selection moves everything below
       it up one, and End Task used to go to whatever had moved into the row
       (M209). A selected process that is gone leaves nothing selected rather
       than its neighbour. */
    selected = -1;
    for (int i = 0; i < task_count && selected_pid >= 0; i++) {
        if ((long)tasks[i].pid == selected_pid) {
            selected = i;
        }
    }
    if (selected < 0) {
        selected_pid = -1;
    }
    refresh_rows();
}

static void scroll_to_selected(void) {
    if (selected < 0 || selected >= task_count || !rows[selected]) {
        return;
    }
    lv_obj_scroll_to_view(rows[selected], LV_ANIM_OFF);
}

static void signal_selected(int number) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    if (selected < 0 || selected >= task_count) {
        set_status("Nothing selected.", palette->danger);
        return;
    }
    const task_info_t *task = &tasks[selected];
    if (task->state == TASK_INFO_TERMINATED) {
        set_status("That process has already exited.", palette->danger);
        return;
    }
    if (is_protected(task->name)) {
        set_status("That process is part of the desktop.", palette->danger);
        return;
    }
    if (sys_kill(task->pid, number) != 0) {
        set_status("The kernel refused that signal.", palette->danger);
        return;
    }
    set_status(number == SIGKILL ? "Force Quit sent." : "End Task sent.", palette->positive);
    refresh_tasks();
}

static void on_row_clicked(lv_event_t *event) {
    int index = (int)(long)lv_event_get_user_data(event);
    if (index >= task_count) {
        return;
    }
    select_row(index);
    refresh_rows();
}

static void on_end_task(lv_event_t *event) {
    (void)event;
    signal_selected(SIGTERM);
}

static void on_force_quit(lv_event_t *event) {
    (void)event;
    signal_selected(SIGKILL);
}

static void on_show_system(lv_event_t *event) {
    show_system_tasks = lv_obj_has_state((lv_obj_t *)lv_event_get_target(event), LV_STATE_CHECKED) ? 1 : 0;
    refresh_tasks();
    printf("[tasks] system tasks %s, %d listed\n", show_system_tasks ? "shown" : "hidden", task_count);
}

static void on_tick(lv_timer_t *timer) {
    (void)timer;
    refresh_tasks();
}

static int on_key(lvgl_window_t *window, uint32_t ch, uint32_t mods) {
    (void)window;
    (void)mods;
    if (ch == KEYBOARD_KEY_UP) {
        if (selected > 0) {
            select_row(selected - 1);
            refresh_rows();
            scroll_to_selected();
        }
        return 1;
    }
    if (ch == KEYBOARD_KEY_DOWN) {
        if (selected + 1 < task_count) {
            select_row(selected + 1);
            refresh_rows();
            scroll_to_selected();
        }
        return 1;
    }
    if (ch == '\n' || ch == '\r') {
        signal_selected(SIGTERM);
        return 1;
    }
    return 0;
}

static void build_user_interface(void) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    lv_obj_t *root = lvgl_theme_page(lv_screen_active());
    lv_obj_set_style_pad_row(root, 8, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(root);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(header, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    row_cell(header, "PID", COLUMN_PID_WIDTH, palette->text_dim);
    row_cell(header, "NAME", 0, palette->text_dim);
    row_cell(header, "STATE", COLUMN_STATE_WIDTH, palette->text_dim);
    row_cell(header, "PPID", COLUMN_PARENT_WIDTH, palette->text_dim);
    row_cell(header, "FD/SHM", COLUMN_RESOURCE_WIDTH, palette->text_dim);

    list = lv_obj_create(root);
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

    lv_obj_t *footer = lvgl_theme_row(root, NULL);
    lv_obj_t *system_switch = lv_switch_create(footer);
    lv_obj_set_size(system_switch, 40, 22);
    lv_obj_set_style_bg_color(system_switch, lvgl_theme_color(palette->outline), LV_PART_MAIN);
    lv_obj_set_style_bg_color(system_switch, lvgl_theme_color(palette->accent), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(system_switch, lvgl_theme_color(palette->text), LV_PART_KNOB);
    lv_obj_add_event_cb(system_switch, on_show_system, LV_EVENT_VALUE_CHANGED, NULL);
    lvgl_theme_caption(footer, "System tasks");
    status_label = lvgl_theme_caption(footer, "");
    lv_obj_set_flex_grow(status_label, 1);
    lv_obj_t *end_task = lvgl_theme_button(footer, "End Task", 0);
    lv_obj_add_event_cb(end_task, on_end_task, LV_EVENT_CLICKED, NULL);
    lv_obj_t *force_quit = lvgl_theme_button(footer, "Force Quit", 1);
    lv_obj_set_style_bg_color(force_quit, lvgl_theme_color(palette->danger), LV_PART_MAIN);
    lv_obj_add_event_cb(force_quit, on_force_quit, LV_EVENT_CLICKED, NULL);

    lvgl_window_report_geometry("list", list);
    lvgl_window_report_geometry("end_task", end_task);
    lvgl_window_report_geometry("force_quit", force_quit);
    lvgl_window_report_geometry("show_system", system_switch);
}

static unsigned long count_drawn_pixels(void) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    const uint32_t *pixels = task_manager_window.window.graphics.pixels;
    unsigned long total =
        (unsigned long)task_manager_window.window.width * task_manager_window.window.height;
    unsigned long drawn = 0;
    for (unsigned long i = 0; i < total; i++) {
        if ((pixels[i] & 0x00FFFFFFu) != palette->window) {
            drawn++;
        }
    }
    return drawn;
}

int desktop_application_task_manager(int selftest) {
    if (lvgl_window_open(TASK_MANAGER_WINDOW_WIDTH, TASK_MANAGER_WINDOW_HEIGHT, "Tasks",
                         &task_manager_window) != 0) {
        printf("[m127] task_manager: window open failed\n");
        return 1;
    }
    lvgl_theme_apply(&task_manager_window);
    lvgl_window_set_key_handler(&task_manager_window, on_key);

    build_user_interface();
    refresh_tasks();
    printf("[tasks] %d programs listed, newest first\n", task_count);
    lv_timer_create(on_tick, TASK_MANAGER_REFRESH_MS, NULL);

    for (int frame = 0; frame < 8; frame++) {
        lv_timer_handler();
        lvgl_window_pump(&task_manager_window, 16);
    }

    unsigned long drawn = count_drawn_pixels();
    printf("[m127] task_manager rendered %lu pixels\n", drawn);
    if (drawn < TASK_MANAGER_MINIMUM_DRAWN_PIXELS) {
        printf("[m127] task_manager render FAILED\n");
        lvgl_window_close(&task_manager_window);
        return 1;
    }
    if (selftest) {
        lvgl_window_close(&task_manager_window);
        return 0;
    }

    lvgl_window_run(&task_manager_window);
    lvgl_window_close(&task_manager_window);
    return 0;
}
