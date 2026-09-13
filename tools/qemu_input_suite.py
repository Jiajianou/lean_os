#!/usr/bin/env python3

import os
import subprocess
import shutil
import signal
import sys
import threading
import time
import traceback
from concurrent.futures import ThreadPoolExecutor

import qemu_input
from qemu_input import BOOT_MARKER, Machine

ARTIFACT_DIR = os.environ.get("LEANOS_INPUT_ARTIFACTS", "/tmp/leanos-input-failures")

_current = threading.local()

def current_test():
    return getattr(_current, "name", "unknown")

ICON_X = 56
ICONS = [
    ("Terminal", "gui_terminal", 56, ICON_X),
    ("Editor", "text_editor", 146, ICON_X),
    ("Files", "file_manager", 236, ICON_X),
    ("Settings", "settings", 326, ICON_X),
    ("Clock", "gui_clock", 416, ICON_X),
    ("Paint", "gui_paint", 506, ICON_X),
    ("Tasks", "task_manager", 596, ICON_X),
    ("README", "text_editor", 56, ICON_X + 90),
    ("Browser", "netsurf", 146, ICON_X + 90),
]

ICON_BOX = 0x4C99E6
CTX_MENU_BG = 0x243040
EMPTY_DESKTOP = (500, 500)

SCREEN_H = 768
WALLPAPER_BASE = 0x1A1A2E
WALLPAPER_TOP_PCT = 155
WALLPAPER_BOTTOM_PCT = 60

def _scale(color, pct):
    out = 0
    for shift in (16, 8, 0):
        out |= min(255, ((color >> shift) & 0xFF) * pct // 100) << shift
    return out

def _trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q

def desktop_px(y, base=WALLPAPER_BASE):
    top = _scale(base, WALLPAPER_TOP_PCT)
    bottom = _scale(base, WALLPAPER_BOTTOM_PCT)
    out = 0
    for shift in (16, 8, 0):
        a = (top >> shift) & 0xFF
        b = (bottom >> shift) & 0xFF
        out |= (a + _trunc_div((b - a) * y, SCREEN_H - 1)) << shift
    return out

DESKTOP_BG = desktop_px(EMPTY_DESKTOP[1])

TRANSLUCENT_NUM, TRANSLUCENT_DEN = 3, 4
LAUNCHER_OPACITY_NUM, LAUNCHER_OPACITY_DEN = 4, 5

def blend(under, over, num, den):
    out = 0
    for shift in (16, 8, 0):
        u = (under >> shift) & 0xFF
        o = (over >> shift) & 0xFF
        out |= ((u * (den - num) + o * num) // den) << shift
    return out

def panel_px(raw, y):
    return blend(desktop_px(y), raw, TRANSLUCENT_NUM, TRANSLUCENT_DEN)

EDITOR_MENU_ROW_H = 16
EDITOR_MENU_BG = 0x242424
EDITOR_MENU_ITEM_W = 110
EDITOR_MENU_ITEM_H = 20
EDITOR_MENU_X = 4

FONT_H = 16
PANEL_TOP = SCREEN_H - 32
BTN_Y = 4
BTN_H = 24
SLOT_W = 96
SLOT_GAP = 4
START_X = 4
START_W = 72
SLOTS_X = START_X + START_W + 8
TRAY_W = 112

PANEL_PROBE_Y = PANEL_TOP + 2
BTN_PROBE_Y = PANEL_TOP + BTN_Y + 12

PANEL_BG = panel_px(0x181828, PANEL_PROBE_Y)
SLOT_BG = panel_px(0x263447, BTN_PROBE_Y)
SLOT_HOVER_BG = panel_px(0x365070, BTN_PROBE_Y)
SLOT_FOCUS_BG = panel_px(0x2E4A63, BTN_PROBE_Y)
SLOT_MIN_BG = panel_px(0x352A20, BTN_PROBE_Y)
SLOT_COLORS = (SLOT_BG, SLOT_HOVER_BG, SLOT_FOCUS_BG, SLOT_MIN_BG)

START_PROBE = (71, PANEL_TOP + 6)
START_CLICK = (40, PANEL_TOP + BTN_Y + BTN_H // 2)
START_BG = panel_px(0x243447, START_PROBE[1])
START_HOVER_BG = panel_px(0x365070, START_PROBE[1])
START_PRESS_BG = panel_px(0x4C99E6, START_PROBE[1])
START_COLORS = (START_BG, START_HOVER_BG, START_PRESS_BG)

TRAY_SEP_PROBE = (1024 - TRAY_W, PANEL_TOP + 14)
TRAY_SEP = panel_px(0x303C4E, TRAY_SEP_PROBE[1])

LAUNCHER_BG_RAW = 0x1C2233
LAUNCHER_SEL_BG = 0x335577
LAUNCHER_W, LAUNCHER_H = 480, 320
LAUNCHER_X = (1024 - LAUNCHER_W) // 2
LAUNCHER_Y = (768 - LAUNCHER_H) // 3
LAUNCHER_LIST_Y = 46
LAUNCHER_ROW_H = 20
LAUNCHER_PAD = 12

POWER_BTN_W, POWER_BTN_H, POWER_BTN_GAP = 96, 22, 8
POWER_BTN_Y = LAUNCHER_H - LAUNCHER_PAD - POWER_BTN_H
POWER_OFF_X = LAUNCHER_W - LAUNCHER_PAD - 2 * POWER_BTN_W - POWER_BTN_GAP
POWER_REBOOT_X = LAUNCHER_W - LAUNCHER_PAD - POWER_BTN_W
POWER_CONFIRM_W, POWER_CONFIRM_H = 300, 96
POWER_CONFIRM_BG = 0x202838

def power_button_center(which):
    bx = POWER_OFF_X if which == 0 else POWER_REBOOT_X
    return (LAUNCHER_X + bx + POWER_BTN_W // 2, LAUNCHER_Y + POWER_BTN_Y + POWER_BTN_H // 2)

LAUNCHER_LOWER_PROBE = (LAUNCHER_X + 20, LAUNCHER_Y + POWER_BTN_Y + POWER_BTN_H // 2)
LAUNCHER_LOWER_BG = blend(desktop_px(LAUNCHER_LOWER_PROBE[1]), LAUNCHER_BG_RAW,
                          LAUNCHER_OPACITY_NUM, LAUNCHER_OPACITY_DEN)

def power_confirm_probe():
    return (LAUNCHER_X + (LAUNCHER_W - POWER_CONFIRM_W) // 2 + POWER_CONFIRM_W - 12,
            LAUNCHER_Y + (LAUNCHER_H - POWER_CONFIRM_H) // 2 + POWER_CONFIRM_H - 8)
LAUNCHER_PROBE = (LAUNCHER_X + 428, LAUNCHER_Y + LAUNCHER_LIST_Y + 5 * LAUNCHER_ROW_H + 10)
LAUNCHER_BG = blend(desktop_px(LAUNCHER_PROBE[1]), LAUNCHER_BG_RAW,
                    LAUNCHER_OPACITY_NUM, LAUNCHER_OPACITY_DEN)

ACCENT = 0x4C99E6
SNAP_PREVIEW_NUM, SNAP_PREVIEW_DEN = 1, 4

def launcher_row_probe(i):
    return (LAUNCHER_X + 428, LAUNCHER_Y + LAUNCHER_LIST_Y + i * LAUNCHER_ROW_H + 10)

def slot_probe(i):
    return (SLOTS_X + i * (SLOT_W + SLOT_GAP) + SLOT_W - 8,
            PANEL_TOP + BTN_Y + 12)

def slot_click(i):
    return (SLOTS_X + i * (SLOT_W + SLOT_GAP) + SLOT_W - 24, PANEL_TOP + BTN_Y)

TITLEBAR_H = 20
BTN_SIZE = 14
BTN_GAP = 4
BTN_MARGIN = 4
BTN_MINIMIZE, BTN_MAXIMIZE, BTN_CLOSE = 0, 1, 2

BTN_CLOSE_COLOR = 0xFF5F57
BTN_MAXIMIZE_COLOR = 0xFEBC2E
BTN_MINIMIZE_COLOR = 0x8FA88F
BTN_HOVER_LIGHTEN = 2

def lighten(color, num, den):
    out = 0
    for shift in (16, 8, 0):
        c = (color >> shift) & 0xFF
        out |= (c + (255 - c) * num // den) << shift
    return out

def titlebar_button_disc(win_x, win_y, win_w, button):
    cx, cy = titlebar_button_center(win_x, win_y, win_w, button)
    return (cx - 4, cy)

def titlebar_button_center(win_x, win_y, win_w, button):
    x = win_x + win_w - BTN_MARGIN - BTN_SIZE - button * (BTN_SIZE + BTN_GAP)
    y = win_y - TITLEBAR_H + (TITLEBAR_H - BTN_SIZE) // 2
    return (x + BTN_SIZE // 2, y + BTN_SIZE // 2)

CLOCK_W = 200

BORDER = 2
BORDER_COLOR = 0x444466

SETTINGS_W, SETTINGS_H = 320, 680
WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H = 258, 68, 22

MOTION_BTN_Y = 288
VOL_Y, VOL_BTN_W, VOL_BTN_H, VOL_BTN_GAP = 288, 22, 20, 4
VOL_X0 = 12 + 58
MOTION_BTN_W, MOTION_BTN_H = 50, 20
MOTION_BTN_X = 320 - 12 - MOTION_BTN_W
SETTINGS_BTN_ON = 0x607088
SETTINGS_BTN_OFF = 0x445566

def volume_click(sx, sy, step):
    return (sx + VOL_X0 + step * (VOL_BTN_W + VOL_BTN_GAP) + VOL_BTN_W - 4,
            sy + VOL_Y + VOL_BTN_H - 4)

def volume_probe(sx, sy, step):
    return (sx + VOL_X0 + step * (VOL_BTN_W + VOL_BTN_GAP) + 3,
            sy + VOL_Y + 3)

def motion_click(sx, sy):
    return (sx + MOTION_BTN_X + MOTION_BTN_W - 6, sy + MOTION_BTN_Y + MOTION_BTN_H - 4)

def motion_probe(sx, sy):
    return (sx + MOTION_BTN_X + 6, sy + MOTION_BTN_Y + 4)

MODE_BTN_Y, MODE_BTN_W, MODE_BTN_H, MODE_BTN_GAP, MODE_COLS = 340, 92, 20, 6, 3
GFX_PAD = 12
CONFIRM_Y = MODE_BTN_Y + 3 * (MODE_BTN_H + 4) + 6
CONFIRM_H = 20
KEEP_BTN_W = 64
KEEP_BTN_X = SETTINGS_W - GFX_PAD - KEEP_BTN_W
SMALL_MODE = (800, 600)
SMALL_MODE_INDEX = 0
MODE_REVERT_S = 10 + 5

def mode_btn_center(sx, sy, i):
    return (sx + GFX_PAD + (i % MODE_COLS) * (MODE_BTN_W + MODE_BTN_GAP) + MODE_BTN_W // 2,
            sy + MODE_BTN_Y + (i // MODE_COLS) * (MODE_BTN_H + 4) + MODE_BTN_H // 2)

FM_W, FM_H = 340, 360
FM_HEADER_H = 24
FM_COLS_H = 16
FM_LIST_Y = FM_HEADER_H + FM_COLS_H
FM_ROW_H = 16
FM_LIST_W = FM_W - 8

FM_LABEL_COLOR = 0x90A0C0
FM_TEXT_COLOR = 0xD8D8D8
FM_STATUS_H = 20
FM_ROWS_VISIBLE = (FM_H - FM_LIST_Y - FM_STATUS_H) // FM_ROW_H

FM_UP_ROW = 0
FM_FIRST_FILE_ROW = 1
FM_ROOT_FIRST_ROW = 0

def fm_row_point(x, y, row):
    return (x + 60, y + FM_LIST_Y + row * FM_ROW_H + FM_ROW_H // 2)

def fm_rows_with_text(shot, x, y):
    n = 0
    for row in range(FM_ROWS_VISIBLE):
        top = y + FM_LIST_Y + row * FM_ROW_H
        if shot.count_color(FM_TEXT_COLOR, x + 6, top, FM_LIST_W - 12, FM_ROW_H) > 0:
            n += 1
    return n

DRAG_LABEL_BG = 0x335577

TOAST_W, TOAST_H, TOAST_GAP, TOAST_MARGIN = 300, 56, 8, 12
TOAST_STRIPE_W = 4
TOAST_BG = 0x222A38
TOAST_ERROR_C = 0xE05C55

def toast_rect(i):
    return (1024 - TOAST_W - TOAST_MARGIN, TOAST_MARGIN + i * (TOAST_H + TOAST_GAP))

def toast_stripe_probe(i):
    x, y = toast_rect(i)
    return (x + 1 + TOAST_STRIPE_W // 2, y + TOAST_H // 2)

def toast_click_point(i):
    x, y = toast_rect(i)
    return (x + TOAST_W - 20, y + TOAST_H // 2)
TASKS_W, TASKS_H = 420, 360
LIST_Y_IN_WIN = 40
LIST_H_IN_WIN = TASKS_H - LIST_Y_IN_WIN - 34
SCROLLBAR_THUMB = 0x506080
TM_ROW_H = 16
TM_SELECT = 0x4C6699
TM_BG = 0x1C1C24

def tm_row_states(shot, tx, ty):
    selected = -1
    last = -1
    rows = (LIST_H_IN_WIN) // TM_ROW_H
    for row in range(rows):
        y = ty + LIST_Y_IN_WIN + row * TM_ROW_H + TM_ROW_H // 2
        if shot.px(tx + 4, y) == TM_SELECT:
            selected = row
        if any(shot.px(x, y) != TM_BG for x in range(tx + 4, tx + TASKS_W - 12, 3)):
            last = row
    return selected, last

MENU_W = 124
MENU_ITEM_H = 22
MENU_MINIMIZE, MENU_CLOSE, MENU_FORCE_QUIT = 0, 1, 2
MENU_BG_RAW = 0x243040

def taskbar_menu_row_center(slot, row):
    x = SLOTS_X + slot * (SLOT_W + SLOT_GAP)
    top = PANEL_TOP - MENU_ITEM_H * 3
    return (x + MENU_W // 2, top + row * MENU_ITEM_H + MENU_ITEM_H // 2)

FIRST_APP_IDX = 2

CONTENT_TOP = 22
CONTENT_BOTTOM = SCREEN_H - 32

def app_origin(slot, height=None):
    x = 100 + slot * 40
    y = 100 + slot * 40
    if height is not None:
        y = max(min(y, CONTENT_BOTTOM - height), CONTENT_TOP)
    return (x, y)

class Failure(Exception):
    pass

ED_W, ED_H = 640, 384
ED_CONTENT_Y0 = 16
ED_TEXT_COLOR = 0xE0E0E0

TERM_W, TERM_H = 560, 336
TERM_TEXT_COLOR = 0xD0D0D0

def check(condition, message):
    if not condition:
        raise Failure(message)

def refusals(machine):
    return [line.split("refused: ", 1)[1]
            for line in machine.read_log().splitlines()
            if "[wm] window request refused: " in line]

def count_app_windows(shot):
    n = 0
    for i in range(10):
        x, y = slot_probe(i)
        if shot.px(x, y) not in SLOT_COLORS:
            break
        n += 1
    return n

def save_failure_shot(machine, name):
    src = os.path.join(machine._dir, "shot%03d.ppm" % machine._shot_seq)
    if not os.path.exists(src):
        return None
    os.makedirs(ARTIFACT_DIR, exist_ok=True)
    dst = os.path.join(ARTIFACT_DIR, "%s.ppm" % name)
    shutil.copyfile(src, dst)
    return dst

def desktop_is_painted(shot):
    return (shot.px(*EMPTY_DESKTOP) == DESKTOP_BG and
            shot.px(76, 76) == ICON_BOX and
            shot.px(512, PANEL_PROBE_Y) == PANEL_BG and
            shot.px(*TRAY_SEP_PROBE) == TRAY_SEP and
            shot.px(*START_PROBE) in START_COLORS)

def boot(machine, timeout=None):
    machine.boot_to_desktop(settle=0.0, timeout=timeout or machine.boot_timeout or 300)
    deadline = time.time() + 30
    shot = None
    while time.time() < deadline:
        shot = machine.screenshot()
        if desktop_is_painted(shot):
            return
        time.sleep(0.5)
    probes = (
        ("wallpaper", shot.px(*EMPTY_DESKTOP), desktop_px(EMPTY_DESKTOP[1])),
        ("first icon", shot.px(76, 76), ICON_BOX),
        ("taskbar", shot.px(512, PANEL_PROBE_Y), PANEL_BG),
        ("tray separator", shot.px(*TRAY_SEP_PROBE), TRAY_SEP),
        ("Start button", shot.px(*START_PROBE), START_COLORS[0]),
    )
    wrong = ", ".join("%s 0x%06X (wanted 0x%06X)" % p for p in probes if p[1] != p[2])
    raise Failure("the desktop never finished painting after boot - %s; "
                  "screendump %s, guest log %s"
                  % (wrong or "every probe matched on the last look",
                     save_failure_shot(machine, current_test()),
                     machine.save_log("desktop-never-painted")))

def wait_for(machine, predicate, what, timeout=12.0):
    deadline = time.time() + timeout
    shot = None
    while time.time() < deadline:
        shot = machine.screenshot()
        if predicate(shot):
            return shot
        time.sleep(0.4)
    raise Failure("%s (last screenshot showed %d app window(s), focus on slot "
                  "%d; screendump saved to %s)"
                  % (what, count_app_windows(shot), focused_slot(shot),
                     save_failure_shot(machine, current_test())))

def wait_for_windows(machine, n, timeout=12.0):
    return wait_for(machine, lambda s: count_app_windows(s) == n,
                    "expected %d app window(s)" % n, timeout)

def focused_slot(shot):
    for i in range(10):
        x, y = slot_probe(i)
        px = shot.px(x, y)
        if px not in SLOT_COLORS:
            break
        if px == SLOT_FOCUS_BG:
            return i
    return -1

TASKBAR_MAX_SLOTS = 0
while (SLOTS_X + (TASKBAR_MAX_SLOTS + 1) * (SLOT_W + SLOT_GAP) - SLOT_GAP
       <= 1024 - TRAY_W):
    TASKBAR_MAX_SLOTS += 1

def _screens_differ(a, b, at_least):
    n = 0
    for y in range(0, min(a.height, b.height), 4):
        for x in range(0, min(a.width, b.width), 4):
            if a.px(x, y) != b.px(x, y):
                n += 1
    return n * 16 >= at_least

def test_double_click_launches_every_icon(m):
    boot(m)
    for i, (name, _program, y, x) in enumerate(ICONS):
        m.double_click(x, y)
        timeout = 45.0 if name == "Browser" else 12.0
        want = min(i + 1, TASKBAR_MAX_SLOTS)
        beyond_taskbar = i >= TASKBAR_MAX_SLOTS
        before = m.screenshot() if beyond_taskbar else None
        wait_for_windows(m, want, timeout=timeout)
        if beyond_taskbar:
            after = wait_for(m, lambda s: _screens_differ(before, s, 20000),
                             "%s opened no window that changed the screen "
                             "(the taskbar is full at %d slots, so this is "
                             "what proves it launched)"
                             % (name, TASKBAR_MAX_SLOTS),
                             timeout=timeout)
            del after

    refused = refusals(m)
    check(not refused,
          "compositor refused %d window request(s): %s" % (len(refused), refused))

def test_single_click_does_not_launch(m):
    boot(m)
    check(count_app_windows(m.screenshot()) == 0, "the desktop did not start empty")
    m.click(ICON_X, ICONS[0][2])
    time.sleep(4.0)
    after = count_app_windows(m.screenshot())
    check(after == 0, "a single click opened %d window(s)" % after)

def test_titlebar_close_button(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    m.click(*titlebar_button_center(x, y, CLOCK_W, BTN_CLOSE))
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "clicking the titlebar close button did not close the window")

def test_titlebar_drag_moves_window(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    before = wait_for_windows(m, 1)
    x, y = app_origin(FIRST_APP_IDX)
    check(before.px(x + 4, y - 8) != desktop_px(y - 8),
          "Clock did not appear at the expected cascade position")

    m.drag(x + 100, y - 8, x + 300, y + 120)
    after = wait_for(m, lambda s: s.px(x + 4, y - 8) == desktop_px(y - 8),
                     "window still at its original position after a titlebar drag")
    check(after.px(x + 204, y + 120) != desktop_px(y + 120),
          "window did not appear at the drag destination")

def test_alt_tab_cycles_focus(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[5][2])
    before = wait_for_windows(m, 2)
    was = focused_slot(before)
    check(was >= 0, "no app is focused before Alt-Tab")

    m.sendkey("alt-tab")
    shot = wait_for(m, lambda s: focused_slot(s) not in (was,),
                    "Alt-Tab left focus on the same app (taskbar slot %d)" % was)
    check(focused_slot(shot) >= 0, "nothing is focused after Alt-Tab")

def test_desktop_context_menu(m):
    boot(m)
    m.right_click(*EMPTY_DESKTOP)
    wait_for(m,
             lambda s: s.count_color(CTX_MENU_BG, EMPTY_DESKTOP[0],
                                      EMPTY_DESKTOP[1], 120, 40) > 500,
             "right-clicking the desktop did not draw the context menu")

    m.click(EMPTY_DESKTOP[0] + 40, EMPTY_DESKTOP[1] + 8)
    wait_for_windows(m, 1)

def test_launch_close_stress(m):
    boot(m)
    rounds = 60
    for round_no in range(rounds):
        m.double_click(ICON_X, ICONS[4][2])
        wait_for(m, lambda s: count_app_windows(s) == 1,
                 "round %d: Clock did not open" % round_no)
        x, y = app_origin(FIRST_APP_IDX)
        m.click(*titlebar_button_center(x, y, CLOCK_W, BTN_CLOSE))
        wait_for(m, lambda s: count_app_windows(s) == 0,
                 "round %d: Clock did not close" % round_no)

    refused = refusals(m)
    check(not refused,
          "after %d launch/close cycles the compositor started refusing "
          "windows: %s" % (rounds, refused))

def test_start_button_opens_launcher(m):
    boot(m)
    check(m.screenshot().px(*START_PROBE) == START_BG,
          "the Start button is not drawn at rest")

    m.move_to(*START_CLICK)
    wait_for(m, lambda s: s.px(*START_PROBE) in (START_HOVER_BG, START_PRESS_BG),
             "hovering the Start button did not highlight it - a panel should "
             "receive WM_EVENT_MOUSE_MOVE even though it never holds focus")

    m.click()
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) == LAUNCHER_BG,
             "clicking Start did not open the launcher overlay")
    m.click()
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) == desktop_px(LAUNCHER_PROBE[1]),
             "clicking Start again did not close the launcher overlay")

def test_taskbar_click_keeps_app_focused(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)
    check(focused_slot(m.screenshot()) == 0, "the Clock did not take focus when it opened")

    m.click(*START_CLICK)
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) == LAUNCHER_BG,
             "clicking Start did not open the launcher overlay")
    check(focused_slot(m.screenshot()) == 0,
          "clicking the taskbar took focus away from the running app")
    m.click(*START_CLICK)

def test_taskbar_button_focus_and_minimize(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[5][2])
    wait_for_windows(m, 2)
    check(focused_slot(m.screenshot()) == 1, "Paint did not take focus when it opened")

    m.click(*slot_click(0))
    wait_for(m, lambda s: focused_slot(s) == 0,
             "clicking the Clock's taskbar button did not focus it")
    m.click(*slot_click(0))
    wait_for(m, lambda s: s.px(*slot_probe(0)) == SLOT_MIN_BG,
             "clicking the focused app's taskbar button did not minimize it")

def test_editor_in_window_file_menu(m):
    boot(m)
    m.double_click(ICON_X, ICONS[1][2])
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    m.click(x + EDITOR_MENU_X + 12, y + EDITOR_MENU_ROW_H // 2)
    wait_for(m,
             lambda s: s.count_color(EDITOR_MENU_BG, x + EDITOR_MENU_X,
                                      y + EDITOR_MENU_ROW_H + 2,
                                      EDITOR_MENU_ITEM_W, EDITOR_MENU_ITEM_H) > 200,
             "clicking File in the editor's own menu row did not open its dropdown")

    m.click(x + EDITOR_MENU_X + EDITOR_MENU_ITEM_W // 2,
            y + EDITOR_MENU_ROW_H + 3 * EDITOR_MENU_ITEM_H + EDITOR_MENU_ITEM_H // 2)
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "picking File > Quit did not close the Editor")

def test_launcher_keychord_types_and_launches(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) == LAUNCHER_BG,
             "Ctrl+Space did not open the launcher")
    check(m.screenshot().px(*launcher_row_probe(0)) == LAUNCHER_SEL_BG,
          "the launcher's first result is not drawn selected")

    m.type_text("gui_pai")
    m.sendkey("ret")
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) != LAUNCHER_BG,
             "Enter did not dismiss the launcher")
    wait_for_windows(m, 2)

def test_launcher_click_launches_a_result(m):
    boot(m)
    m.click(*START_CLICK)
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) == LAUNCHER_BG,
             "the Start button did not open the launcher")

    m.type_text("gui_clo")
    m.click(*launcher_row_probe(0))
    wait_for_windows(m, 1)

def test_launcher_escape_dismisses(m):
    boot(m)
    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) == LAUNCHER_BG,
             "Ctrl+Space did not open the launcher")
    m.sendkey("esc")
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) == desktop_px(LAUNCHER_PROBE[1]),
             "Escape did not dismiss the launcher")
    check(count_app_windows(m.screenshot()) == 0,
          "dismissing the launcher with Escape launched something")

def test_snap_drag_to_edge(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    m.press(x + 100, y - TITLEBAR_H // 2)
    m.move_held(1020, 400)

    shot = m.screenshot()
    expected = blend(desktop_px(60), ACCENT, SNAP_PREVIEW_NUM, SNAP_PREVIEW_DEN)
    check(shot.px(600, 60) == expected,
          "no snap preview while dragging into the right edge (got 0x%06X at (600,60), "
          "expected 0x%06X)" % (shot.px(600, 60), expected))

    m.release()
    shot = wait_for(m, lambda s: s.px(600, 12) == ACCENT,
                    "releasing at the right edge did not snap the window to the right half")
    check(shot.px(200, 12) == desktop_px(12),
          "the snapped window is not confined to the right half")

def test_taskbar_right_click_force_quit(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    m.right_click(*slot_click(0))
    probe = (SLOTS_X + MENU_W - 12, PANEL_TOP - MENU_ITEM_H * 3 + 6)
    expected = panel_px(MENU_BG_RAW, probe[1])
    wait_for(m, lambda s: s.px(*probe) == expected,
             "right-clicking a taskbar button did not raise its context menu "
             "(the panel overhang never came up)")

    m.click(*taskbar_menu_row_center(0, MENU_FORCE_QUIT))
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "Force Quit on the taskbar context menu did not remove the window")
    wait_for(m, lambda s: s.px(*probe) == desktop_px(probe[1]),
             "the panel overhang stayed raised after the menu closed")

def test_titlebar_right_click_force_quit(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    menu_x, menu_y = x + 60, y - TITLEBAR_H // 2
    m.right_click(menu_x, menu_y)
    probe = (menu_x + MENU_W - 12, menu_y + MENU_ITEM_H + MENU_ITEM_H // 2)
    wait_for(m, lambda s: s.px(*probe) == MENU_BG_RAW,
             "right-clicking a titlebar did not raise the window context menu")

    m.click(menu_x + MENU_W // 2, menu_y + MENU_FORCE_QUIT * MENU_ITEM_H + MENU_ITEM_H // 2)
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "Force Quit on the titlebar context menu did not remove the window")

def test_ctrl_shift_esc_opens_task_manager(m):
    boot(m)
    check(count_app_windows(m.screenshot()) == 0, "the desktop did not start empty")
    m.sendkey("ctrl-shift-esc")
    wait_for_windows(m, 1)

def test_task_manager_end_task(m):
    boot(m)
    m.double_click(ICON_X, ICONS[6][2])
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 2)

    tx, ty = app_origin(FIRST_APP_IDX)
    m.click(tx + TASKS_W - 40, ty + 8)

    for _ in range(90):
        m.sendkey("down")

    def selection_at_end(shot):
        selected, last = tm_row_states(shot, tx, ty)
        return selected >= 0 and selected == last

    wait_for(m, selection_at_end,
             "the selection never reached the last row of the task list", timeout=25.0)

    end_x = tx + TASKS_W - 96 - 8 - 96 - 8 + 48
    end_y = ty + TASKS_H - 22 - 6 + 11
    m.click(end_x, end_y)
    wait_for(m, lambda s: count_app_windows(s) == 1,
             "End Task in the task manager did not terminate the selected process")

def test_titlebar_button_hover_lights_and_still_closes(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    probe = titlebar_button_disc(x, y, CLOCK_W, BTN_MINIMIZE)
    m.move_to(x + 60, y - TITLEBAR_H // 2)
    shot = m.screenshot()
    check(shot.px(*probe) == BTN_MINIMIZE_COLOR,
          "the minimize button was already lit with the pointer elsewhere on the titlebar "
          "(got 0x%06X)" % shot.px(*probe))

    m.move_to(*titlebar_button_center(x, y, CLOCK_W, BTN_MINIMIZE))
    expected = lighten(BTN_MINIMIZE_COLOR, 1, BTN_HOVER_LIGHTEN)
    wait_for(m, lambda s: s.px(*probe) == expected,
             "hovering the minimize button did not light it")

    m.click(*titlebar_button_center(x, y, CLOCK_W, BTN_CLOSE))
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "the titlebar close button stopped closing once hover tracking was added")

def test_titlebar_double_click_maximizes(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    m.double_click(x + 100, y - TITLEBAR_H // 2)
    wait_for(m, lambda s: s.px(60, 12) == ACCENT,
             "double-clicking the titlebar did not maximize the window")

    m.double_click(2 + 100, 12)
    wait_for(m, lambda s: s.px(60, 12) == desktop_px(12),
             "double-clicking the titlebar again did not restore the window")

def test_resize_edge_changes_cursor(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)

    def shape_at(at):
        m.move_to(*at)
        found = m.find_cursor_shape(m.screenshot(), at)
        check(found is not None, "no cursor found at (%d, %d)" % at)
        return found[2]

    got = shape_at((x + 100, y + 45))
    check(got == "arrow", "expected the plain arrow over window content, got %r" % got)

    got = shape_at((x + CLOCK_W + 2, y + 45))
    check(got == "horizontal",
          "moving onto a resize edge did not change the drawn cursor (got %r)" % got)

    got = shape_at((x + CLOCK_W + 2, y + 90))
    check(got == "diag_nw_se",
          "the bottom-right corner did not get the diagonal resize cursor (got %r)" % got)

    got = shape_at((x + 100, y - TITLEBAR_H // 2))
    check(got == "move", "the titlebar did not get the move cursor (got %r)" % got)

def test_shutdown_powers_off_the_machine(m):
    boot(m)
    m.click(*START_CLICK)
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "the Start button did not open the launcher")

    m.click(*power_button_center(0))
    wait_for(m, lambda s: s.px(*power_confirm_probe()) == POWER_CONFIRM_BG,
             "clicking Shut Down did not raise a confirm box")

    m.sendkey("y")
    took = m.wait_for_exit(timeout=40.0)
    check(took is not None,
          "the guest never powered off - QEMU is still running 40s after confirming "
          "Shut Down (log tail: %r)" % m.read_log()[-400:])
    check(m.exit_status == 0,
          "the guest exited, but with status %d rather than 0" % m.exit_status)
    log = m.read_log()
    check("[power] orderly stop complete" in log,
          "the shutdown did not go through the orderly stop (log tail: %r)" % log[-400:])

def test_shutdown_confirm_can_be_cancelled(m):
    boot(m)
    m.click(*START_CLICK)
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "the Start button did not open the launcher")

    m.click(*power_button_center(0))
    wait_for(m, lambda s: s.px(*power_confirm_probe()) == POWER_CONFIRM_BG,
             "clicking Shut Down did not raise a confirm box")

    m.sendkey("n")
    wait_for(m, lambda s: s.px(*power_confirm_probe()) != POWER_CONFIRM_BG and
                          s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "cancelling the confirm box did not return to the launcher")
    time.sleep(4.0)
    check(m.exit_status is None,
          "cancelling the shutdown confirm powered the machine off anyway")

def test_settings_persist_across_a_reboot(m):
    boot(m)
    m.double_click(ICON_X, ICONS[3][2])
    wait_for_windows(m, 1)

    sx, sy = app_origin(FIRST_APP_IDX, SETTINGS_H)
    m.click(sx + 12 + WALL_BTN_W // 2, sy + WALL_BTN_Y + WALL_BTN_H // 2)
    shot = wait_for(m, lambda s: s.px(700, 200) == s.px(700, 600),
                    "picking the Flat wallpaper did not flatten the desktop gradient")
    flat = shot.px(700, 200)
    check(flat != desktop_px(200),
          "the 'flat' desktop is the same color the gradient already was at that row")

    boots_before = m.read_log().count(BOOT_MARKER)
    m.sendkey("ctrl-spc")
    m.type_text("reboot")
    m.sendkey("ret")

    deadline = time.time() + 300
    while time.time() < deadline:
        if m.read_log().count(BOOT_MARKER) > boots_before:
            break
        time.sleep(1.0)
    else:
        raise Failure("the machine never came back up after `reboot` (log: %s, tail: %r)"
                      % (m.save_log("reboot-never-finished"), m.read_log()[-400:]))
    check("[power] restarting." in m.read_log(),
          "the machine restarted without going through SYS_shutdown's own path")

    deadline = time.time() + 60
    while time.time() < deadline:
        shot = m.screenshot()
        if shot.px(700, 200) == flat and shot.px(700, 600) == flat:
            return
        time.sleep(1.0)
    raise Failure("after restarting, the desktop came back with the default gradient "
                  "rather than the saved Flat wallpaper")

def test_session_restores_windows_across_a_reboot(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)
    x, y = app_origin(FIRST_APP_IDX)

    dest_x, dest_y = x + 380, y + 260
    m.drag(x + 100, y - 8, dest_x + 100, dest_y - 8)
    wait_for(m, lambda s: s.px(dest_x + 4, dest_y - 8) != desktop_px(dest_y - 8),
             "the Clock did not move to where it was dragged")
    content = wait_for(m, lambda s: s.px(dest_x + 4, dest_y + 4) != desktop_px(dest_y + 4),
                       "the Clock's content is not where it was dragged to")
    content_px = content.px(dest_x + 4, dest_y + 4)

    m.double_click(ICON_X, ICONS[5][2])
    wait_for_windows(m, 2)

    time.sleep(3.0)

    boots_before = m.read_log().count(BOOT_MARKER)
    m.sendkey("ctrl-spc")
    m.type_text("reboot")
    m.sendkey("ret")

    deadline = time.time() + 300
    while time.time() < deadline:
        if m.read_log().count(BOOT_MARKER) > boots_before:
            break
        time.sleep(1.0)
    else:
        raise Failure("the machine never came back up after `reboot` (log: %s, tail: %r)"
                      % (m.save_log("reboot-never-finished"), m.read_log()[-400:]))

    wait_for(m, lambda s: count_app_windows(s) >= 2,
             "the session did not bring both windows back", timeout=120.0)
    wait_for(m, lambda s: s.px(dest_x + 4, dest_y + 4) == content_px,
             "a window came back, but not where it was left - the session restored "
             "the program and let the cascade place it", timeout=60.0)

def test_launcher_does_not_offer_data_files(m):
    boot(m)
    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "Ctrl+Space did not open the launcher")
    check(m.screenshot().px(*launcher_row_probe(0)) == LAUNCHER_SEL_BG,
          "the launcher's first result is not drawn selected before typing")

    m.type_text("readme")
    wait_for(m, lambda s: s.px(*launcher_row_probe(0)) != LAUNCHER_SEL_BG,
             "the launcher still offered a result for a file that is not a program")

    m.sendkey("ret")
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) != LAUNCHER_BG,
             "Enter did not dismiss the launcher")
    time.sleep(4.0)
    shot = m.screenshot()
    check(count_app_windows(shot) == 0,
          "Enter on an empty launcher opened %d window(s)" % count_app_windows(shot))
    check(shot.px(*toast_stripe_probe(0)) != TOAST_ERROR_C,
          "a toast was raised for a file the launcher should never have offered")

def test_clicking_a_toast_dismisses_it(m):
    boot(m)
    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "Ctrl+Space did not open the launcher")
    m.type_text("wm_faulter")
    m.sendkey("ret")

    probe = toast_stripe_probe(0)
    wait_for(m, lambda s: s.px(*probe) == TOAST_ERROR_C, "no toast was raised", timeout=15.0)

    m.click(*toast_click_point(0))
    shot = wait_for(m, lambda s: s.px(*probe) != TOAST_ERROR_C,
                    "clicking the toast did not dismiss it", timeout=2.5)
    check(shot.px(*probe) == desktop_px(probe[1]),
          "the toast went away but left something behind at 0x%06X" % shot.px(*probe))

def test_wheel_scrolls_the_file_list_one_row_per_detent(m):
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    list_x = x + 6
    row0_y = y + FM_LIST_Y

    m.double_click(*fm_row_point(x, y, FM_UP_ROW))
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) > 0, "the file manager showed nothing after going up")
    m.double_click(*fm_row_point(x, y, FM_ROOT_FIRST_ROW))
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) >= FM_ROWS_VISIBLE,
             "entering /bin did not fill the list - there is nothing here long enough to scroll")

    def row_pixels(shot, row):
        top = row0_y + row * FM_ROW_H
        return [shot.px(px, py)
                for py in range(top, top + FM_ROW_H)
                for px in range(list_x, x + FM_LIST_W - 4)]

    m.move_to(x + FM_W // 2, y + FM_H // 2)
    before = m.screenshot()
    want = row_pixels(before, 3)
    check(any(p != want[0] for p in want),
          "the row this test scrolls to is blank - the file list is too short to scroll")

    m.wheel(3)
    after = m.screenshot()
    got = row_pixels(after, 0)
    if got != want:
        m.wheel(-6)
        after = m.screenshot()
        got = row_pixels(after, 0)
    check(got == want,
          "three wheel detents did not move the file list by exactly three rows")

def test_alt_f4_closes_the_focused_window(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)
    m.sendkey("alt-f4")
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "Alt+F4 did not close the focused window")

def test_ctrl_alt_arrows_snap_and_maximize(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    m.sendkey("ctrl-alt-right")
    wait_for(m, lambda s: s.px(600, 12) == ACCENT,
             "Ctrl+Alt+Right did not snap the window to the right half")

    m.sendkey("ctrl-alt-left")
    wait_for(m, lambda s: s.px(60, 12) == ACCENT and s.px(600, 12) == desktop_px(12),
             "Ctrl+Alt+Left did not snap the window back to the left half")

    m.sendkey("ctrl-alt-down")
    wait_for(m, lambda s: count_app_windows(s) == 1 and focused_slot(s) == -1,
             "Ctrl+Alt+Down did not minimize the window (its taskbar button should "
             "still be there, just not focused)")

def test_drag_a_file_onto_the_desktop_opens_it(m):
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    m.press(*fm_row_point(x, y, FM_FIRST_FILE_ROW))
    m.move_held(700, 500)

    shot = m.screenshot()
    check(shot.count_color(DRAG_LABEL_BG, 700, 500, 200, 32) > 0,
          "no drag label followed the cursor - the drag never started")

    m.release()
    wait_for(m, lambda s: count_app_windows(s) == 2,
             "dropping a file on the desktop did not open it in the editor")

FILES_ORIGIN = app_origin(FIRST_APP_IDX)
TASKS_ORIGIN = app_origin(FIRST_APP_IDX + 1)
FILES_SLOT = 0
TASKS_SLOT = 1
FILES_IN_FRONT_PROBE = (FILES_ORIGIN[0] + FM_W, 400)
TASKS_IN_FRONT_PROBE = (TASKS_ORIGIN[0] - BORDER, 400)
FILES_TITLEBAR_CLICK = (FILES_ORIGIN[0] + 20, FILES_ORIGIN[1] - TITLEBAR_H // 2)
TASKS_W = 400
_TASKS_BTN_BAND = TASKS_ORIGIN[0] + TASKS_W - 54
_TASKS_FREE_LO = FILES_ORIGIN[0] + FM_W + 4
if _TASKS_FREE_LO >= _TASKS_BTN_BAND:
    raise SystemExit("qemu_input_suite: Files is now wide enough to cover every clickable "
                     "part of Tasks' titlebar - the overlap tests need a different pair")
TASKS_TITLEBAR_CLICK = ((_TASKS_FREE_LO + _TASKS_BTN_BAND) // 2, TASKS_ORIGIN[1] - TITLEBAR_H // 2)
OVERLAP_CLICK = (400, 480)

def open_overlapping_pair(m):
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[6][2])
    wait_for_windows(m, 2)
    wait_for(m, lambda s: s.px(*TASKS_IN_FRONT_PROBE) == BORDER_COLOR,
             "the second window launched did not start in front of the first")

def test_clicking_a_window_raises_it(m):
    open_overlapping_pair(m)

    m.click(*FILES_TITLEBAR_CLICK)
    shot = wait_for(m, lambda s: (s.px(*FILES_IN_FRONT_PROBE) == BORDER_COLOR and
                                  focused_slot(s) == FILES_SLOT),
                    "clicking the covered window's titlebar did not raise and focus it")
    check(shot.px(*TASKS_IN_FRONT_PROBE) != BORDER_COLOR,
          "the window that was raised did not cover the one that had been in front")

    m.click(*TASKS_TITLEBAR_CLICK)
    shot = wait_for(m, lambda s: (s.px(*TASKS_IN_FRONT_PROBE) == BORDER_COLOR and
                                  focused_slot(s) == TASKS_SLOT),
                    "clicking the other window's titlebar did not raise and focus it back")
    check(shot.px(*FILES_IN_FRONT_PROBE) != BORDER_COLOR,
          "both windows claim to be in front after the second raise")

def test_overlap_click_reaches_the_front_window(m):
    open_overlapping_pair(m)
    m.click(*FILES_TITLEBAR_CLICK)
    wait_for(m, lambda s: s.px(*FILES_IN_FRONT_PROBE) == BORDER_COLOR,
             "the covered window did not raise, so there is no occlusion to test")

    m.click(*OVERLAP_CLICK)
    time.sleep(2.0)
    shot = m.screenshot()
    check(focused_slot(shot) == FILES_SLOT,
          "a click in the overlap region focused slot %d - the window behind, "
          "which is not visible at that pixel" % focused_slot(shot))
    check(shot.px(*FILES_IN_FRONT_PROBE) == BORDER_COLOR,
          "the front window stopped being in front after being clicked")

def test_alt_tab_visits_windows_in_use_order(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 2)
    m.double_click(ICON_X, ICONS[6][2])
    shot = wait_for_windows(m, 3)
    check(focused_slot(shot) == 2,
          "the last window launched is not the focused one")

    m.sendkey("alt-tab")
    wait_for(m, lambda s: focused_slot(s) == 1,
             "Alt+Tab did not reach the window used before the focused one")

    m.sendkey("alt-tab")
    wait_for(m, lambda s: focused_slot(s) == 2,
             "a second Alt+Tab did not come back to the window it started on")

    m.sendkey("shift-alt-tab")
    wait_for(m, lambda s: focused_slot(s) == 0,
             "Shift+Alt+Tab did not wrap to the window used longest ago")

def test_a_crashing_program_only_takes_itself_down(m):
    boot(m)
    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "Ctrl+Space did not open the launcher")
    m.type_text("wm_faulter")
    m.sendkey("ret")

    wait_for_windows(m, 1)

    probe = toast_stripe_probe(0)
    wait_for(m, lambda s: s.px(*probe) == TOAST_ERROR_C,
             "a client that faulted raised no crash toast", timeout=15.0)
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "the faulting client's window was never reclaimed")

    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1, timeout=15.0)

def test_file_manager_navigates_directories(m):
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 1)
    x, y = app_origin(FIRST_APP_IDX)

    home_rows = fm_rows_with_text(m.screenshot(), x, y)
    check(0 < home_rows < FM_ROWS_VISIBLE,
          "/home did not open as a short list (%d rows)" % home_rows)

    m.double_click(*fm_row_point(x, y, FM_UP_ROW))
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) > 0,
             "double-clicking .. left the file manager showing nothing")
    m.double_click(*fm_row_point(x, y, FM_ROOT_FIRST_ROW))
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) >= FM_ROWS_VISIBLE,
             "double-clicking /bin did not enter a directory full of programs")

    m.double_click(*fm_row_point(x, y, FM_UP_ROW))
    wait_for(m, lambda s: 0 < fm_rows_with_text(s, x, y) < FM_ROWS_VISIBLE,
             "double-clicking .. did not leave /bin")

FM_CTX_ITEM_W = 124
FM_CTX_ITEM_H = 20
FM_CTX_COUNT = 6
FM_PROMPT_W, FM_PROMPT_H = 260, 72
FM_PROMPT_BG = 0x243040

def fm_prompt_is_open(shot, x, y):
    px = x + (FM_W - FM_PROMPT_W) // 2
    py = y + (FM_H - FM_PROMPT_H) // 2
    return shot.count_color(FM_PROMPT_BG, px, py, FM_PROMPT_W, FM_PROMPT_H) > 3000

def test_file_manager_creates_a_folder_and_deletes_it_full(m):
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 1)
    x, y = app_origin(FIRST_APP_IDX)

    before = fm_rows_with_text(m.screenshot(), x, y)
    check(before > 0, "the Files window listed nothing")

    press_x = x + 60
    press_y = y + FM_LIST_Y + (FM_ROWS_VISIBLE - 1) * FM_ROW_H
    mx = min(press_x, x + FM_LIST_W - FM_CTX_ITEM_W)
    my = min(press_y, y + (FM_H - FM_STATUS_H) - FM_CTX_ITEM_H * FM_CTX_COUNT)

    m.right_click(press_x, press_y)
    wait_for(m, lambda s: s.count_color(CTX_MENU_BG, mx, my, FM_CTX_ITEM_W,
                                        FM_CTX_ITEM_H * FM_CTX_COUNT) > 500,
             "right-clicking the file list did not open a context menu")

    m.click(mx + FM_CTX_ITEM_W // 2, my + FM_CTX_ITEM_H + FM_CTX_ITEM_H // 2)
    wait_for(m, lambda s: fm_prompt_is_open(s, x, y),
             "the New Folder menu item did not open a prompt")

    m.type_text("m112dir")
    m.sendkey("ret")
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) == before + 1,
             "the new folder did not appear in the list", timeout=15.0)

    m.double_click(*fm_row_point(x, y, FM_FIRST_FILE_ROW))
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) == 1,
             "row 1 was not the empty folder that was just created")

    m.sendkey("n")
    wait_for(m, lambda s: fm_prompt_is_open(s, x, y),
             "N did not open the New File prompt")
    m.type_text("inside")
    m.sendkey("ret")
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) == 2,
             "the new file did not appear inside the new folder", timeout=15.0)

    m.sendkey("left")
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) == before + 1,
             "the Left arrow did not leave the folder")

    m.click(*fm_row_point(x, y, FM_FIRST_FILE_ROW))
    time.sleep(0.4)
    m.sendkey("backspace")
    wait_for(m, lambda s: fm_prompt_is_open(s, x, y),
             "Backspace did not open a delete confirm")
    m.sendkey("y")
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) == before,
             "the folder and the file inside it were not deleted", timeout=15.0)

def test_file_manager_refuses_a_new_name_that_escapes_the_folder(m):
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 1)
    x, y = app_origin(FIRST_APP_IDX)

    before = fm_rows_with_text(m.screenshot(), x, y)
    check(before > 0, "the Files window listed nothing")

    m.sendkey("f")
    wait_for(m, lambda s: fm_prompt_is_open(s, x, y),
             "F did not open the New Folder prompt")
    m.type_text("m112esc")
    m.sendkey("ret")
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) == before + 1,
             "the folder to escape into was not created", timeout=15.0)

    m.sendkey("n")
    wait_for(m, lambda s: fm_prompt_is_open(s, x, y),
             "N did not open the New File prompt")
    m.type_text("m112esc/inside")
    m.sendkey("ret")

    time.sleep(3.0)
    check(fm_rows_with_text(m.screenshot(), x, y) == before + 1,
          "a name with a path separator in it created something here")

    m.double_click(*fm_row_point(x, y, FM_FIRST_FILE_ROW))
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) >= 1,
             "double-clicking the new folder did not enter it")
    time.sleep(1.5)
    rows = fm_rows_with_text(m.screenshot(), x, y)
    check(rows == 1,
          "a name with a path separator in it escaped into the subfolder "
          "(%d rows inside it, expected just \"..\")" % rows)

    m.sendkey("left")
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) == before + 1,
             "the Left arrow did not leave the folder")
    m.click(*fm_row_point(x, y, FM_FIRST_FILE_ROW))
    time.sleep(0.4)
    m.sendkey("backspace")
    wait_for(m, lambda s: fm_prompt_is_open(s, x, y),
             "Backspace did not open a delete confirm")
    m.sendkey("y")
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) == before,
             "the folder this test made was not cleaned up", timeout=15.0)

def test_desktop_survives_losing_the_compositor(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "Ctrl+Space did not open the launcher")
    m.type_text("wm_crash")
    m.sendkey("ret")

    wait_for(m, lambda s: desktop_is_painted(s),
             "the desktop never came back after the compositor was killed",
             timeout=40.0)

    wait_for(m, lambda s: count_app_windows(s) == 1,
             "the app that was open before the crash did not come back",
             timeout=30.0)

    m.double_click(ICON_X, ICONS[5][2])
    wait_for_windows(m, 2, timeout=20.0)

def _settled_row(machine, sample, tries=8):
    last = sample(machine.screenshot())
    for _ in range(tries):
        time.sleep(0.4)
        now = sample(machine.screenshot())
        if now == last:
            return now
        last = now
    return last

def test_editor_undo_restores_the_buffer(m):
    boot(m)
    m.double_click(ICON_X, ICONS[0][2])
    wait_for_windows(m, 1)
    m.type_text("HELLO")

    time.sleep(0.5)
    m.sendkey("ctrl-c")
    time.sleep(0.5)

    m.double_click(ICON_X, ICONS[1][2])
    wait_for_windows(m, 2)
    ex, ey = app_origin(FIRST_APP_IDX + 1)

    def first_row(shot):
        return [shot.px(px, py)
                for py in range(ey + ED_CONTENT_Y0, ey + ED_CONTENT_Y0 + 16)
                for px in range(ex, ex + 200)]

    m.type_text("AB")
    wait_for(m, lambda s: any(p == ED_TEXT_COLOR for p in first_row(s)),
             "nothing was typed into the editor")
    before = _settled_row(m, first_row)

    m.sendkey("ctrl-v")
    wait_for(m, lambda s: first_row(s) != before,
             "Ctrl+V pasted nothing - the editor had no paste at all before M56")

    m.sendkey("ctrl-z")
    wait_for(m, lambda s: first_row(s) == before,
             "one undo did not put the buffer back exactly as it was before the paste")

def test_terminal_scrollback_scrolls_with_the_wheel(m):
    boot(m)
    m.double_click(ICON_X, ICONS[0][2])
    wait_for_windows(m, 1)
    tx, ty = app_origin(FIRST_APP_IDX)

    def top_row(shot):
        return [shot.px(px, py)
                for py in range(ty, ty + 16)
                for px in range(tx, tx + TERM_W - 8)]

    m.type_text("ls /bin")
    m.sendkey("ret")
    wait_for(m, lambda s: any(p == TERM_TEXT_COLOR for p in top_row(s)),
             "the terminal produced no output to scroll through", timeout=20.0)
    before = _settled_row(m, top_row)

    m.move_to(tx + TERM_W // 2, ty + TERM_H // 2)

    back = 5
    m.wheel(back)
    if _settled_row(m, top_row) == before:
        back = -5
        m.wheel(back)
    check(_settled_row(m, top_row) != before,
          "scrolling the wheel revealed nothing - the terminal is still discarding its top line")

    m.wheel(-back)
    wait_for(m, lambda s: top_row(s) == before,
             "scrolling forward again did not return to the live view")
    m.wheel(-back)
    wait_for(m, lambda s: top_row(s) == before,
             "scrolling forward past the live view moved it")

def test_copying_a_file_shows_up_in_another_window(m):
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 2)

    ax, ay = app_origin(FIRST_APP_IDX)
    bx, by = app_origin(FIRST_APP_IDX + 1)

    before = fm_rows_with_text(m.screenshot(), bx, by)
    check(before > 0, "the second Files window listed nothing")

    m.click(*fm_row_point(bx, by, FM_FIRST_FILE_ROW))
    time.sleep(0.4)
    m.sendkey("c")
    time.sleep(0.6)
    m.type_text("m56copy")
    m.sendkey("ret")

    wait_for(m, lambda s: fm_rows_with_text(s, bx, by) == before + 1,
             "the copy did not appear in the window that made it", timeout=15.0)
    m.click(ax + 20, ay - TITLEBAR_H // 2)
    wait_for(m, lambda s: fm_rows_with_text(s, ax, ay) == before + 1,
             "the other window's listing never caught up with the new file", timeout=15.0)

def test_soak_desktop_stays_usable(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 2)

    soak_seconds = float(os.environ.get("LEANOS_SOAK_SECONDS", "180"))
    deadline = time.time() + soak_seconds
    while time.time() < deadline:
        time.sleep(5.0)
        m.move_to(*slot_click(0))

    check(count_app_windows(m.screenshot()) == 2,
          "a window disappeared during the soak")

    m.double_click(ICON_X, ICONS[0][2])
    wait_for_windows(m, 3, timeout=20.0)

    refused = refusals(m)
    check(not refused,
          "the compositor refused %d window request(s) during the soak: %s"
          % (len(refused), refused))

def _bar_spans(shot):
    y = shot.height - 30
    left = shot.px(2, y)
    right = shot.px(shot.width - 3, y)
    above = shot.px(shot.width // 2, shot.height // 2)
    return left == right and left != above

def test_display_resolution_changes_and_persists(m):
    boot(m)
    m.double_click(ICON_X, ICONS[3][2])
    wait_for_windows(m, 1)

    sx, sy = app_origin(FIRST_APP_IDX, SETTINGS_H)
    m.click(*mode_btn_center(sx, sy, SMALL_MODE_INDEX))
    wait_for(m, lambda s: (s.width, s.height) == SMALL_MODE,
             "clicking a resolution did not change the display size")
    wait_for(m, _bar_spans,
             "the taskbar does not span the new %dx%d display - its buffer was not reallocated"
             % SMALL_MODE, timeout=20.0)

    kx = sx + KEEP_BTN_X + KEEP_BTN_W // 2
    ky = CONTENT_TOP + CONFIRM_Y + CONFIRM_H // 2
    m.click(kx, ky)

    time.sleep(MODE_REVERT_S)
    shot = m.screenshot()
    check((shot.width, shot.height) == SMALL_MODE,
          "the confirmed resolution did not stick - the display is %dx%d again"
          % (shot.width, shot.height))
    check(_bar_spans(shot), "the taskbar stopped spanning the display after the mode was kept")

def test_display_resolution_reverts_when_not_confirmed(m):
    boot(m)
    before = m.screenshot()
    original = (before.width, before.height)
    check(original != SMALL_MODE, "the desktop already boots at the mode this test switches to")

    m.double_click(ICON_X, ICONS[3][2])
    wait_for_windows(m, 1)
    sx, sy = app_origin(FIRST_APP_IDX, SETTINGS_H)
    m.click(*mode_btn_center(sx, sy, SMALL_MODE_INDEX))
    wait_for(m, lambda s: (s.width, s.height) == SMALL_MODE,
             "clicking a resolution did not change the display size")

    deadline = time.time() + MODE_REVERT_S + 10
    shot = None
    while time.time() < deadline:
        shot = m.screenshot()
        if (shot.width, shot.height) == original:
            break
        time.sleep(0.5)
    check((shot.width, shot.height) == original,
          "an unconfirmed resolution was never reverted - still %dx%d" % (shot.width, shot.height))
    check(_bar_spans(shot), "after the revert the taskbar does not span the restored display")

def test_behaviour_settings_persist(m):
    boot(m)
    m.double_click(ICON_X, ICONS[3][2])
    wait_for_windows(m, 1)

    sx, sy = app_origin(FIRST_APP_IDX, SETTINGS_H)
    probe = motion_probe(sx, sy)
    shot = wait_for(m, lambda s: s.px(*probe) == SETTINGS_BTN_ON,
                    "Settings did not open with animations on, which is the default")

    m.click(*motion_click(sx, sy))
    wait_for(m, lambda s: s.px(*probe) == SETTINGS_BTN_OFF,
             "clicking the Motion switch did not turn animations off")

    mute = volume_probe(sx, sy, 0)
    check(m.screenshot().px(*mute) == SETTINGS_BTN_OFF,
          "the volume did not start un-muted, which is the default")
    m.click(*volume_click(sx, sy, 0))
    wait_for(m, lambda s: s.px(*mute) == SETTINGS_BTN_ON,
             "clicking mute did not take")

    boots_before = m.read_log().count(BOOT_MARKER)
    m.sendkey("ctrl-spc")
    m.type_text("reboot")
    m.sendkey("ret")
    deadline = time.time() + 300
    while time.time() < deadline and m.read_log().count(BOOT_MARKER) <= boots_before:
        time.sleep(1.0)
    check(m.read_log().count(BOOT_MARKER) > boots_before, "the machine never restarted")

    shot = wait_for(m, lambda s: count_app_windows(s) >= 1 and
                                 s.px(*probe) in (SETTINGS_BTN_ON, SETTINGS_BTN_OFF),
                    "Settings did not come back with a readable Motion switch",
                    timeout=120.0)
    check(shot.px(*probe) == SETTINGS_BTN_OFF,
          "the Motion switch forgot it had been turned off across a restart")
    check(shot.px(*volume_probe(sx, sy, 0)) == SETTINGS_BTN_ON,
          "the volume forgot it had been muted across a restart")

class Region:

    def __init__(self, x, y, w, h, why):
        self.x, self.y, self.w, self.h, self.why = x, y, w, h, why

    def contains(self, px, py):
        return self.x <= px < self.x + self.w and self.y <= py < self.y + self.h

CLOCK_REGION = Region(1024 - 80, PANEL_TOP, 80, 32, "the taskbar clock ticks")

def burst(machine, n=25):
    return [machine.screenshot() for _ in range(n)]

def assert_stable_outside(machine, base, shots, allowed, what, step=3,
                          tolerance=0):
    W, H = base.width, base.height
    worst = None
    for i, s in enumerate(shots):
        diffs = []
        for y in range(0, H, step):
            for x in range(0, W, step):
                if any(r.contains(x, y) for r in allowed):
                    continue
                if s.px(x, y) != base.px(x, y):
                    diffs.append((x, y))
                    if len(diffs) > 4000:
                        break
            if len(diffs) > 4000:
                break
        if len(diffs) > tolerance and (worst is None or len(diffs) > worst[1]):
            worst = (i, len(diffs), diffs)

    if worst is None:
        return

    i, n, diffs = worst
    xs = [d[0] for d in diffs]
    ys = [d[1] for d in diffs]
    path = save_failure_shot(machine, current_test())
    raise Failure(
        "%s: frame %d of %d changed %d sampled pixel(s) outside the regions "
        "this action may touch (first at (%d,%d)), spanning x=%d..%d "
        "y=%d..%d. Allowed: %s. A transient that puts everything back is "
        "exactly what this check exists for - look at the frame rather than "
        "re-running. Screendump saved to %s"
        % (what, i, len(shots), n, diffs[0][0], diffs[0][1],
           min(xs), max(xs), min(ys), max(ys),
           "; ".join("%s at (%d,%d,%d,%d)" % (r.why, r.x, r.y, r.w, r.h)
                     for r in allowed),
           path))

def test_launching_an_app_does_not_disturb_the_rest_of_the_screen(m):
    boot(m)
    m.move_to(ICON_X, ICONS[4][2])
    time.sleep(1.0)
    base = m.screenshot()

    m.double_click()
    shots = burst(m, 25)
    wait_for_windows(m, 1)

    allowed = [
        Region(140, 100, 340, 240, "the window that opened, its chrome, its "
                                   "shadow and its open animation"),
        Region(0, PANEL_TOP, 400, 32, "the taskbar gaining a button"),
        CLOCK_REGION,
        Region(ICON_X - 48, ICONS[4][2] - 48, 96, 96,
               "the icon that was double-clicked, and the cursor on it"),
    ]
    assert_stable_outside(m, base, shots, allowed,
                          "launching an app disturbed the rest of the desktop")

def test_closing_an_app_does_not_disturb_the_rest_of_the_screen(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    m.move_to(*titlebar_button_center(x, y, CLOCK_W, BTN_CLOSE))
    time.sleep(1.5)

    base = m.screenshot()
    m.click()
    shots = burst(m, 25)
    wait_for_windows(m, 0)

    allowed = [
        Region(140, 100, 340, 240, "the window that closed, its shadow and "
                                   "its close animation"),
        Region(0, PANEL_TOP, 400, 32, "the taskbar losing a button"),
        CLOCK_REGION,
        Region(300, 100, 120, 60, "the cursor, parked on the close button"),
        Region(ICON_X - 48, ICONS[4][2] - 48, 96, 96,
               "the launching icon's selection highlight"),
    ]
    assert_stable_outside(m, base, shots, allowed,
                          "closing an app disturbed the rest of the desktop")

def test_moving_the_cursor_changes_only_the_cursor(m):
    boot(m)
    m.move_to(300, 300)
    time.sleep(1.0)
    base = m.screenshot()

    path = [(340, 300), (380, 320), (420, 300), (460, 280), (500, 300),
            (460, 280), (420, 300), (380, 320), (340, 300), (300, 300)]
    for x, y in path:
        m.move_to(x, y)
    time.sleep(0.5)
    shots = [m.screenshot() for _ in range(3)]

    allowed = [
        Region(292, 292, 24, 24, "the cursor, back where it started"),
        CLOCK_REGION,
    ]
    assert_stable_outside(m, base, shots, allowed,
                          "moving the cursor and returning left the screen changed - "
                          "a trail, or a repaint that did not restore what it covered")

def test_typing_into_a_window_changes_only_that_window(m):
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 1)
    x, y = app_origin(FIRST_APP_IDX)
    m.move_to(x + 100, y + 100)
    time.sleep(1.5)
    base = m.screenshot()

    shots = []
    for ch in "the quick brown fox":
        m.type_text(ch)
        shots.append(m.screenshot())

    allowed = [
        Region(x - 40, y - 60, 700, 520, "the editor window, its chrome and its shadow"),
        Region(0, PANEL_TOP, 400, 32, "the taskbar"),
        CLOCK_REGION,
    ]
    assert_stable_outside(m, base, shots, allowed,
                          "typing into a window changed pixels outside it")

def test_a_window_redrawing_itself_leaves_its_neighbours_alone(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[6][2])
    wait_for_windows(m, 2)

    m.move_to(700, 600)
    time.sleep(2.0)
    base = m.screenshot()

    shots = []
    for _ in range(12):
        shots.append(m.screenshot())
        time.sleep(0.3)

    allowed = [
        Region(100, 60, 640, 520, "the two windows, their chrome and shadows"),
        Region(0, PANEL_TOP, 500, 32, "the taskbar"),
        CLOCK_REGION,
        Region(660, 560, 80, 80, "the cursor, parked"),
    ]
    assert_stable_outside(m, base, shots, allowed,
                          "a window repainting itself disturbed the desktop around it")

def _page_columns(shot):
    cols = [x for x in range(0, shot.width)
            if sum(1 for y in range(0, shot.height, 2)
                   if shot.px(x, y) == 0xFFFFFF) > 25]
    return (min(cols), max(cols)) if cols else None

def _blue_in_page(shot):
    region = _page_columns(shot)
    if region is None:
        return 0
    x0, x1 = region
    n = 0
    for y in range(0, shot.height, 4):
        for x in range(x0, x1 + 1, 4):
            c = shot.px(x, y)
            if (c & 0xFF) > 150 and (c & 0xFF) - ((c >> 16) & 0xFF) > 50:
                n += 1
    return n * 4

def test_browser_renders_a_page(m):
    boot(m)
    check(count_app_windows(m.screenshot()) == 0, "the desktop did not start empty")

    browser = ICONS[-1]
    check(browser[0] == "Browser", "the last icon is %s, not the browser" % browser[0])
    m.double_click(browser[3], browser[2])

    wait_for_windows(m, 1, timeout=45.0)

    shot = wait_for(m,
                    lambda s: s.count_color(0xFFFFFF, 0, 0, s.width, s.height) > 40000,
                    "the browser window never filled with a rendered page - it "
                    "started and painted no white at all, which is what a "
                    "browser drawing into a buffer nothing displays looks like",
                    timeout=45.0)

    shot = wait_for(m, lambda s: _blue_in_page(s) > 5000,
                    "the page painted its background but NetSurf's own banner "
                    "image never appeared on it - libpng did not decode it, or "
                    "the plotters did not draw it",
                    timeout=45.0)

    region = _page_columns(shot)
    check(region is not None,
          "no column of this screen has a dense run of white in it, so there "
          "is no page on it - even though something painted white somewhere")
    x0, x1 = region
    check(x1 - x0 > 400,
          "the page is only %d columns wide; a browser window here is ~780"
          % (x1 - x0))

    blue = red = text = 0
    for y in range(0, shot.height, 2):
        for x in range(x0, x1 + 1, 2):
            c = shot.px(x, y)
            r, g, b = (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF
            if b > 150 and b - r > 50:
                blue += 1
            if r > 150 and r - b > 50:
                red += 1
            if (r < 96 and g < 96 and b < 96 and x0 + 3 < x < x1 - 3 and
                    shot.px(x - 3, y) == 0xFFFFFF and
                    shot.px(x + 3, y) == 0xFFFFFF):
                text += 1

    check(blue > 5000,
          "only %d blue pixels on the page - NetSurf's own banner image did "
          "not decode, or did not draw" % blue)
    check(blue > 20 * (red + 1),
          "%d blue against %d red on a page whose banner is blue. If those "
          "are the wrong way round, this surface has red and blue swapped - "
          "see user_space/bin/nsfb_leanos.c, where the pixel format is "
          "claimed to match the compositor's exactly" % (blue, red))
    check(text > 50,
          "only %d dark pixels between white ones - freetype rasterised no "
          "text onto the page" % text)

def test_browser_loads_a_page_from_another_machine(m):
    boot(m)
    browser = ICONS[-1]
    m.double_click(browser[3], browser[2])
    wait_for_windows(m, 1, timeout=45.0)
    wait_for(m, lambda s: _blue_in_page(s) > 5000,
             "the browser never finished showing its own start page",
             timeout=45.0)

    m.click(620, 151)
    time.sleep(0.3)
    for _ in range(60):
        m.sendkey("backspace")
    m.type_text(qemu_input.NET_PAGE_URL)
    started = time.time()
    m.sendkey("ret")

    def block_on_screen(s):
        return s.count_color(qemu_input.NET_PAGE_BLOCK, 180, 160, 820, 560) > 20000

    BUDGET_S = 10.0
    wait_for(m, block_on_screen,
             "the page from 10.0.2.100 was not fully on screen within %.0f s of "
             "pressing Enter - the network, TCP or the browser's fetch loop "
             "is losing time (see M116)" % BUDGET_S,
             timeout=BUDGET_S)
    print("    page from another machine on screen %.1f s after Enter"
          % (time.time() - started))

def test_browser_survives_an_empty_flex_container(m):
    boot(m)
    browser = ICONS[-1]
    m.double_click(browser[3], browser[2])
    wait_for_windows(m, 1, timeout=45.0)
    wait_for(m, lambda s: _blue_in_page(s) > 5000,
             "the browser never finished showing its own start page",
             timeout=45.0)
    m.click(620, 151)
    time.sleep(0.3)
    for _ in range(60):
        m.sendkey("backspace")
    m.type_text("file:///usr/share/netsurf/flex.html")
    m.sendkey("ret")
    wait_for(m, lambda s: s.count_color(0xE02020, 180, 160, 820, 560) > 20000,
             "the block after an empty flex container never appeared - the "
             "layout failed (M117: malloc(0) returning NULL was how) or the "
             "browser died on its assertion",
             timeout=20.0)
    check(count_app_windows(m.screenshot()) == 1,
          "the browser window is gone - it died on the page")

def test_window_animations_stay_smooth(m):
    boot(m)
    before = m.read_log().count("anim_frame_gap_ms")
    n = 0
    for name, _, y, x in ICONS[:4]:
        m.double_click(x, y)
        n += 1
        wait_for_windows(m, n, timeout=30.0)
        time.sleep(0.6)
    for _ in range(4):
        m.sendkey("alt-f4")
        time.sleep(1.0)
    time.sleep(2.0)
    gaps = [int(l.split()[2]) for l in m.read_log().splitlines()
            if l.startswith("[perf] anim_frame_gap_ms")][before:]
    check(len(gaps) >= 4, "expected at least four animations reported, saw %d" % len(gaps))
    gaps.sort()
    print("    %d animations, gaps %s ms" % (len(gaps), gaps))
    check(gaps[1] <= 50, "the two best animations had gaps of %d and %d ms between frames - HEAD's best "
                          "two were 60 and 60, M117's 40 and 40; a desktop that cannot draw one animation "
                          "at that pace has its compositor not waking for its frames or its clients "
                          "spinning again (see M117)" % (gaps[0], gaps[1]))
    check(gaps[-1] <= 250, "an animation had a %d ms gap between two frames - that is a stall, not motion" % gaps[-1])

TESTS = [
    ("double_click_launches_every_icon", test_double_click_launches_every_icon),
    ("single_click_does_not_launch", test_single_click_does_not_launch),
    ("titlebar_close_button", test_titlebar_close_button),
    ("titlebar_drag_moves_window", test_titlebar_drag_moves_window),
    ("alt_tab_cycles_focus", test_alt_tab_cycles_focus),
    ("desktop_context_menu", test_desktop_context_menu),
    ("start_button_opens_launcher", test_start_button_opens_launcher),
    ("taskbar_click_keeps_app_focused", test_taskbar_click_keeps_app_focused),
    ("taskbar_button_focus_and_minimize", test_taskbar_button_focus_and_minimize),
    ("editor_in_window_file_menu", test_editor_in_window_file_menu),
    ("launcher_keychord_types_and_launches", test_launcher_keychord_types_and_launches),
    ("launcher_click_launches_a_result", test_launcher_click_launches_a_result),
    ("launcher_escape_dismisses", test_launcher_escape_dismisses),
    ("snap_drag_to_edge", test_snap_drag_to_edge),
    ("taskbar_right_click_force_quit", test_taskbar_right_click_force_quit),
    ("titlebar_right_click_force_quit", test_titlebar_right_click_force_quit),
    ("ctrl_shift_esc_opens_task_manager", test_ctrl_shift_esc_opens_task_manager),
    ("task_manager_end_task", test_task_manager_end_task),
    ("titlebar_button_hover_lights_and_still_closes", test_titlebar_button_hover_lights_and_still_closes),
    ("titlebar_double_click_maximizes", test_titlebar_double_click_maximizes),
    ("resize_edge_changes_cursor", test_resize_edge_changes_cursor),
    ("shutdown_confirm_can_be_cancelled", test_shutdown_confirm_can_be_cancelled),
    ("shutdown_powers_off_the_machine", test_shutdown_powers_off_the_machine),
    ("settings_persist_across_a_reboot", test_settings_persist_across_a_reboot),
    ("display_resolution_changes_and_persists", test_display_resolution_changes_and_persists),
    ("display_resolution_reverts_when_not_confirmed", test_display_resolution_reverts_when_not_confirmed),
    ("behaviour_settings_persist", test_behaviour_settings_persist),
    ("session_restores_windows_across_a_reboot", test_session_restores_windows_across_a_reboot),
    ("launcher_does_not_offer_data_files", test_launcher_does_not_offer_data_files),
    ("clicking_a_toast_dismisses_it", test_clicking_a_toast_dismisses_it),
    ("wheel_scrolls_the_file_list_one_row_per_detent", test_wheel_scrolls_the_file_list_one_row_per_detent),
    ("alt_f4_closes_the_focused_window", test_alt_f4_closes_the_focused_window),
    ("ctrl_alt_arrows_snap_and_maximize", test_ctrl_alt_arrows_snap_and_maximize),
    ("drag_a_file_onto_the_desktop_opens_it", test_drag_a_file_onto_the_desktop_opens_it),
    ("clicking_a_window_raises_it", test_clicking_a_window_raises_it),
    ("overlap_click_reaches_the_front_window", test_overlap_click_reaches_the_front_window),
    ("alt_tab_visits_windows_in_use_order", test_alt_tab_visits_windows_in_use_order),
    ("a_crashing_program_only_takes_itself_down", test_a_crashing_program_only_takes_itself_down),
    ("file_manager_navigates_directories", test_file_manager_navigates_directories),
    ("file_manager_creates_a_folder_and_deletes_it_full",
     test_file_manager_creates_a_folder_and_deletes_it_full),
    ("file_manager_refuses_a_new_name_that_escapes_the_folder",
     test_file_manager_refuses_a_new_name_that_escapes_the_folder),
    ("desktop_survives_losing_the_compositor", test_desktop_survives_losing_the_compositor),
    ("editor_undo_restores_the_buffer", test_editor_undo_restores_the_buffer),
    ("terminal_scrollback_scrolls_with_the_wheel", test_terminal_scrollback_scrolls_with_the_wheel),
    ("copying_a_file_shows_up_in_another_window", test_copying_a_file_shows_up_in_another_window),
    ("soak_desktop_stays_usable", test_soak_desktop_stays_usable),
    ("launch_close_stress", test_launch_close_stress),
    ("launching_an_app_does_not_disturb_the_rest_of_the_screen",
     test_launching_an_app_does_not_disturb_the_rest_of_the_screen),
    ("closing_an_app_does_not_disturb_the_rest_of_the_screen",
     test_closing_an_app_does_not_disturb_the_rest_of_the_screen),
    ("moving_the_cursor_changes_only_the_cursor",
     test_moving_the_cursor_changes_only_the_cursor),
    ("typing_into_a_window_changes_only_that_window",
     test_typing_into_a_window_changes_only_that_window),
    ("a_window_redrawing_itself_leaves_its_neighbours_alone",
     test_a_window_redrawing_itself_leaves_its_neighbours_alone),
    ("browser_renders_a_page", test_browser_renders_a_page),
    ("browser_loads_a_page_from_another_machine",
     test_browser_loads_a_page_from_another_machine),
    ("browser_survives_an_empty_flex_container",
     test_browser_survives_an_empty_flex_container),
    ("window_animations_stay_smooth", test_window_animations_stay_smooth),
]

def _die_on_signal(signum, _frame):
    raise KeyboardInterrupt("received signal %d" % signum)

QUICK_TESTS = [
    "double_click_launches_every_icon",
    "titlebar_close_button",
    "clicking_a_window_raises_it",
    "editor_undo_restores_the_buffer",
    "terminal_scrollback_scrolls_with_the_wheel",
    "file_manager_navigates_directories",
    "file_manager_creates_a_folder_and_deletes_it_full",
    "settings_persist_across_a_reboot",
    "a_crashing_program_only_takes_itself_down",
    "launching_an_app_does_not_disturb_the_rest_of_the_screen",
    "browser_loads_a_page_from_another_machine",
    "browser_survives_an_empty_flex_container",
    "window_animations_stay_smooth",
]

def default_jobs():
    try:
        n = os.cpu_count() or 2
    except Exception:
        n = 2
    return max(1, min(4, n // 3))

FLAKE_LOG = "build/flakes.tsv"

def _commit():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return "unknown"

def record_run(results, jobs, elapsed):
    try:
        os.makedirs("build", exist_ok=True)
        new = not os.path.exists(FLAKE_LOG)
        with open(FLAKE_LOG, "a") as f:
            if new:
                f.write("when\tcommit\ttest\tverdict\tjobs\trun_s\n")
            stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            commit = _commit()
            for name, detail, _msg in results:
                f.write("%s\t%s\t%s\t%s\t%d\t%.0f\n"
                        % (stamp, commit, name,
                           "fail" if detail is not None else "pass",
                           jobs, elapsed))
    except Exception:
        pass

def record_wall_clock(count, jobs, elapsed, snapshot, snap_secs):
    try:
        os.makedirs("build", exist_ok=True)
        path = os.path.join("build", "test-history.tsv")
        new = not os.path.exists(path)
        with open(path, "a") as f:
            if new:
                f.write("when\tcommit\tharness\tverdict\twall_s\tboot_s\n")
            f.write("%s\t%s\t%s\t%s\t%.0f\t%.0f\n"
                    % (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                       _commit(),
                       "input-suite-%s-%dtests-%djobs"
                       % ("snapshot" if snapshot else "cold", count, jobs),
                       "pass", elapsed, snap_secs))
    except Exception:
        pass

def known_flaky(names):
    try:
        seen = {}
        with open(FLAKE_LOG) as f:
            next(f, None)
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 4:
                    continue
                _when, commit, test, verdict = parts[:4]
                seen.setdefault((commit, test), set()).add(verdict)
        wanted = set(names)
        return sorted({test for (_c, test), v in seen.items()
                       if test in wanted and len(v) > 1})
    except Exception:
        return []

COLD_BOOT_TESTS = {
    "shutdown_powers_off_the_machine",
    "settings_persist_across_a_reboot",
    "session_restores_windows_across_a_reboot",
    "behaviour_settings_persist",
}

def run_one(name, fn, boot_timeout, snapshot=None):
    _current.name = name
    started = time.time()
    try:
        use = None if name in COLD_BOOT_TESTS else snapshot
        with Machine(boot_timeout=boot_timeout, snapshot=use) as m:
            fn(m)
    except Failure as exc:
        return (name, str(exc), "   FAIL (%.0fs): %s" % (time.time() - started, exc))
    except KeyboardInterrupt:
        raise
    except Exception:
        detail = "harness error:\n" + traceback.format_exc()
        return (name, detail, "   ERROR (%.0fs):\n%s" % (time.time() - started,
                                                          traceback.format_exc()))
    return (name, None, "   pass (%.0fs)" % (time.time() - started))

def check_stale_snapshot():
    key = qemu_input.snapshot_key()
    qcow, meta = qemu_input.snapshot_paths(key)
    os.makedirs(qemu_input.SNAPSHOT_DIR, exist_ok=True)
    failures = []

    other = qemu_input.snapshot_paths("0" * 64)[0]
    if other == qcow:
        failures.append("two different images produced the same snapshot name")

    saved = None
    if os.path.exists(qcow):
        saved = qcow + ".held"
        os.replace(qcow, saved)
    saved_meta = None
    if os.path.exists(meta):
        saved_meta = meta + ".held"
        os.replace(meta, saved_meta)
    try:
        with open(qcow, "w") as f:
            f.write("not a snapshot")
        with open(meta, "w") as f:
            f.write("0" * 64 + "\n")
        if qemu_input.snapshot_is_valid(key):
            failures.append("a snapshot whose recorded key does not match the "
                            "image was accepted as valid")
        qemu_input.discard_snapshot(key)
        if os.path.exists(qcow) or os.path.exists(meta):
            failures.append("discard_snapshot left the stale files behind")
    finally:
        for tmp in (qcow, meta):
            if os.path.exists(tmp):
                os.unlink(tmp)
        if saved:
            os.replace(saved, qcow)
        if saved_meta:
            os.replace(saved_meta, meta)

    if os.path.exists(qcow) and os.path.exists(meta):
        if not qemu_input.snapshot_is_valid(key):
            failures.append("a current snapshot was refused")
    else:
        print("  (no current snapshot on disk to check the accepting half "
              "against - run the suite once first)")

    if failures:
        print("FAIL: the stale-snapshot check did not hold:")
        for f in failures:
            print("  - %s" % f)
        return 1
    print("PASS: a stale snapshot is refused and a current one is accepted.")
    return 0

def usage():
    print("usage: qemu_input_suite.py [--jobs N] [--quick] [--no-snapshot]\n                            [--check-stale] [test ...]")
    print()
    print("  --jobs N   run N guests at once (default: %d here)" % default_jobs())
    print("  --quick    the pre-commit subset (%d tests)" % len(QUICK_TESTS))
    print("  --no-snapshot  boot every guest cold (Q19's baseline)")
    print("  --check-stale  prove a stale snapshot is refused, not used")
    print()
    print("known tests:")
    for n, _ in TESTS:
        print("  %s" % n)

def main(argv):
    signal.signal(signal.SIGTERM, _die_on_signal)
    signal.signal(signal.SIGINT, _die_on_signal)

    jobs = int(os.environ.get("LEANOS_INPUT_JOBS", "0")) or default_jobs()
    use_snapshot = os.environ.get("LEANOS_INPUT_SNAPSHOT", "1") != "0"
    wanted = []
    args = argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--jobs" and i + 1 < len(args):
            jobs = max(1, int(args[i + 1]))
            i += 2
        elif a.startswith("--jobs="):
            jobs = max(1, int(a.split("=", 1)[1]))
            i += 1
        elif a == "--quick":
            wanted.extend(QUICK_TESTS)
            i += 1
        elif a == "--check-stale":
            return check_stale_snapshot()
        elif a == "--no-snapshot":
            use_snapshot = False
            i += 1
        elif a in ("-h", "--help"):
            usage()
            return 0
        else:
            wanted.append(a)
            i += 1

    selected = [(n, f) for n, f in TESTS if not wanted or n in wanted]
    if not selected:
        print("no test matched %r" % (wanted,))
        usage()
        return 2

    jobs = min(jobs, len(selected))
    boot_timeout = 420 + 90 * (jobs - 1)

    snapshot = None
    snap_secs = 0.0
    if use_snapshot and any(n not in COLD_BOOT_TESTS for n, _ in selected):
        if qemu_input.snapshot_is_valid():
            snapshot = qemu_input.snapshot_paths()[0]
            print("snapshot: reusing %s" % os.path.basename(snapshot), flush=True)
        else:
            qemu_input.discard_snapshot()
            print("snapshot: building one (the image changed, or there was none)",
                  flush=True)
            t0 = time.time()
            snapshot = qemu_input.build_snapshot(desktop_is_painted,
                                                 boot_timeout=boot_timeout)
            snap_secs = time.time() - t0
            print("snapshot: built in %.0fs - %s"
                  % (snap_secs, os.path.basename(snapshot)), flush=True)

    print("running %d test(s), %d at a time%s"
          % (len(selected), jobs,
             "" if snapshot else " (cold boot for every one)"), flush=True)
    started_all = time.time()
    results = []

    if jobs == 1:
        for name, fn in selected:
            print("== %s" % name, flush=True)
            r = run_one(name, fn, boot_timeout, snapshot)
            print(r[2], flush=True)
            results.append(r)
    else:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = [(n, pool.submit(run_one, n, f, boot_timeout, snapshot))
                       for n, f in selected]
            for name, fut in futures:
                r = fut.result()
                print("== %s" % name, flush=True)
                print(r[2], flush=True)
                results.append(r)

    failures = [(n, d) for n, d, _ in results if d is not None]
    elapsed = time.time() - started_all
    record_run(results, jobs, elapsed)
    record_wall_clock(len(selected), jobs, elapsed, snapshot, snap_secs)
    print()
    print("%d test(s) in %.0fs" % (len(selected), elapsed))
    flaky = known_flaky([n for n, _ in selected])
    if flaky:
        print("(%d test(s) with a history of changing verdict without a code "
              "change: %s - see build/flakes.tsv)" % (len(flaky), ", ".join(flaky)))
    if failures:
        print("FAIL: %d/%d interactive test(s) failed:" % (len(failures), len(selected)))
        for name, detail in failures:
            suffix = "  [KNOWN FLAKY]" if name in flaky else ""
            print("  - %s: %s%s" % (name, detail, suffix))
        return 1
    print("PASS: %d/%d interactive tests passed." % (len(selected), len(selected)))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
