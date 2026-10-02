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
    ("Browser", "browser", 146, ICON_X + 90),
]

CTX_MENU_BG = 0x222A36
CTX_MENU_PAD = 6
CTX_MENU_ITEM_H = 28
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

def over_255(v):
    return (v + 128 + ((v + 128) >> 8)) >> 8

def blend_alpha(under, over, alpha):
    out = 0
    for shift in (16, 8, 0):
        u = (under >> shift) & 0xFF
        o = (over >> shift) & 0xFF
        out |= over_255(o * alpha + u * (255 - alpha)) << shift
    return out

LAUNCHER_ALPHA = LAUNCHER_OPACITY_NUM * 255 // LAUNCHER_OPACITY_DEN

def panel_px(raw, y):
    return blend(desktop_px(y), raw, TRANSLUCENT_NUM, TRANSLUCENT_DEN)

EDITOR_MENU_ROW_H = 16
EDITOR_MENU_BG = 0x242424
EDITOR_MENU_ITEM_W = 110
EDITOR_MENU_ITEM_H = 20
EDITOR_MENU_X = 4

FONT_H = 16
PANEL_H = 44
PANEL_TOP = SCREEN_H - PANEL_H
BTN_H = 34
BTN_Y = (PANEL_H - BTN_H) // 2
SLOT_W = 130
SLOT_GAP = 6
SLOT_H = BTN_H
START_X = 8
START_W = 46
SLOTS_X = START_X + START_W + 10
CLOCK_W = 35
TRAY_PAD = 12
WS_DOT_W, WS_DOT_GAP = 14, 5
TRAY_W = TRAY_PAD + 4 * WS_DOT_W + 3 * WS_DOT_GAP + TRAY_PAD + CLOCK_W + TRAY_PAD

PANEL_TOP_RAW = 0x222A38
PANEL_BOTTOM_RAW = 0x141821
ACCENT = 0x4C99E6

def panel_row_raw(row):
    out = 0
    for shift in (16, 8, 0):
        top = (PANEL_TOP_RAW >> shift) & 0xFF
        bottom = (PANEL_BOTTOM_RAW >> shift) & 0xFF
        out |= (top + _trunc_div((bottom - top) * row, PANEL_H - 1)) << shift
    return out

PANEL_PROBE_ROW = 20
PANEL_PROBE_Y = PANEL_TOP + PANEL_PROBE_ROW
PANEL_BG = panel_px(panel_row_raw(PANEL_PROBE_ROW), PANEL_PROBE_Y)

# The Start glyph is a plain donut: a ring of the accent colour with the
# panel showing through its middle. One probe on the ring, one in the hole.
START_RING_OUTER, START_RING_INNER = 8, 4
START_CENTRE = (START_X + START_W // 2, PANEL_TOP + BTN_Y + BTN_H // 2)
START_PROBE = (START_CENTRE[0] - (START_RING_OUTER + START_RING_INNER) // 2 - 1,
               START_CENTRE[1])
START_HOLE = START_CENTRE
START_GLYPH = panel_px(ACCENT, START_PROBE[1])
START_CLICK = (START_X + START_W // 2, PANEL_TOP + BTN_Y + BTN_H // 2)
START_BG_PROBE = (START_X + 6, PANEL_TOP + BTN_Y + BTN_H // 2)
START_HIGHLIGHT_MARGIN = 20

INDICATOR_Y = PANEL_TOP + BTN_Y + SLOT_H - 2
PANEL_GROUND_X = 3

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
LAUNCHER_LOWER_BG = blend_alpha(desktop_px(LAUNCHER_LOWER_PROBE[1]), LAUNCHER_BG_RAW,
                                LAUNCHER_ALPHA)

def power_confirm_probe():
    return (LAUNCHER_X + (LAUNCHER_W - POWER_CONFIRM_W) // 2 + POWER_CONFIRM_W - 12,
            LAUNCHER_Y + (LAUNCHER_H - POWER_CONFIRM_H) // 2 + POWER_CONFIRM_H - 8)
LAUNCHER_PROBE = (LAUNCHER_X + 428, LAUNCHER_Y + LAUNCHER_LIST_Y + 5 * LAUNCHER_ROW_H + 10)
LAUNCHER_BG = blend_alpha(desktop_px(LAUNCHER_PROBE[1]), LAUNCHER_BG_RAW, LAUNCHER_ALPHA)

ACCENT = 0x4C99E6
SNAP_PREVIEW_NUM, SNAP_PREVIEW_DEN = 1, 4

def launcher_row_probe(i):
    return (LAUNCHER_X + 428, LAUNCHER_Y + LAUNCHER_LIST_Y + i * LAUNCHER_ROW_H + 10)

def slot_center_x(i):
    return SLOTS_X + i * (SLOT_W + SLOT_GAP) + SLOT_W // 2

def slot_click(i):
    return (slot_center_x(i), PANEL_TOP + BTN_Y + SLOT_H // 2)

def channel_sum(color):
    return ((color >> 16) & 0xFF) + ((color >> 8) & 0xFF) + (color & 0xFF)

def panel_ground(shot, y):
    return shot.px(PANEL_GROUND_X, y)

SLOT_OCCUPIED_MARGIN = 60
SLOT_MINIMIZED_CEILING = 165
SLOT_ACCENT_MARGIN = 40

def slot_indicator(shot, i):
    return shot.px(slot_center_x(i), INDICATOR_Y)

def slot_occupied(shot, i):
    ground = channel_sum(panel_ground(shot, INDICATOR_Y))
    return channel_sum(slot_indicator(shot, i)) > ground + SLOT_OCCUPIED_MARGIN

def slot_is_focused(shot, i):
    px = slot_indicator(shot, i)
    return (slot_occupied(shot, i) and
            (px & 0xFF) - ((px >> 16) & 0xFF) > SLOT_ACCENT_MARGIN)

def start_highlighted(shot):
    ground = channel_sum(panel_ground(shot, START_BG_PROBE[1]))
    return channel_sum(shot.px(*START_BG_PROBE)) > ground + START_HIGHLIGHT_MARGIN

def slot_is_minimized(shot, i):
    if not slot_occupied(shot, i) or slot_is_focused(shot, i):
        return False
    ground = channel_sum(panel_ground(shot, INDICATOR_Y))
    return channel_sum(slot_indicator(shot, i)) < ground + SLOT_MINIMIZED_CEILING

TITLEBAR_H = 28
BTN_SIZE = 12
BTN_GAP = 8
BTN_MARGIN = 10
BTN_MINIMIZE, BTN_MAXIMIZE, BTN_CLOSE = 0, 1, 2
BTN_SLOT_FROM_RIGHT = {BTN_MINIMIZE: 1, BTN_MAXIMIZE: 0, BTN_CLOSE: 2}

BTN_CLOSE_COLOR = 0xFF5F57
BTN_MAXIMIZE_COLOR = 0x28C840
BTN_MINIMIZE_COLOR = 0xFEBC2E
BTN_IDLE_COLOR = 0x5A6270
BTN_HOVER_LIGHTEN = 5

def lighten(color, num, den):
    out = 0
    for shift in (16, 8, 0):
        c = (color >> shift) & 0xFF
        out |= (c + (255 - c) * num // den) << shift
    return out

def titlebar_button_disc(win_x, win_y, win_w, button):
    cx, cy = titlebar_button_center(win_x, win_y, win_w, button)
    return (cx - 4, cy - 3)

def titlebar_button_center(win_x, win_y, win_w, button):
    x = win_x + win_w - BTN_MARGIN - BTN_SIZE - BTN_SLOT_FROM_RIGHT[button] * (BTN_SIZE + BTN_GAP)
    y = win_y - TITLEBAR_H + (TITLEBAR_H - BTN_SIZE) // 2
    return (x + BTN_SIZE // 2, y + BTN_SIZE // 2)

CLOCK_W = 200

BORDER = 1
HAIRLINE_Y = BORDER
WINDOW_EDGE_MARGIN = 20

def window_edge_at(shot, x, y):
    here = channel_sum(shot.px(x, y))
    inside = channel_sum(shot.px(x + 3, y))
    return here + WINDOW_EDGE_MARGIN < inside

SETTINGS_W, SETTINGS_H = 720, 540

def settings_pane(m, origin, pane):
    mark = len(m.read_log())
    m.click(*widget_center(m, origin, "pane_" + pane))
    wait_for_log_after(m, "[settings] showing ", mark, "Settings did not switch pane")

def wait_for_log_after(m, needle, mark, what, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in m.read_log()[mark:]:
            return
        time.sleep(0.3)
    raise Failure("%s: %s (log: %s)" % (current_test(), what, m.save_log("settings")))

SMALL_MODE = (800, 600)
SMALL_MODE_INDEX = 0
MODE_REVERT_S = 10 + 5

DESKTOP_ACCENT = 0x4C99E6

def geometry(m, name, timeout=25.0):
    needle = "[geometry] %s " % name
    deadline = time.time() + timeout
    while time.time() < deadline:
        log = m.read_log()
        at = log.rfind(needle)
        if at >= 0:
            fields = log[at + len(needle):].split("\n", 1)[0].split()
            if len(fields) >= 4:
                return tuple(int(v) for v in fields[:4])
        time.sleep(0.3)
    raise Failure("%s never reported where it put %r (log: %s)"
                  % (current_test(), name, m.save_log("no-geometry")))

def widget_center(m, origin, name):
    x, y, w, h = geometry(m, name)
    return (origin[0] + x + w // 2, origin[1] + y + h // 2)

def widget_point(m, origin, name, fx, fy):
    x, y, w, h = geometry(m, name)
    return (origin[0] + x + int(w * fx), origin[1] + y + int(h * fy))

def dominant_color(shot, x, y, w, h):
    counts = {}
    for py in range(y, y + h):
        for px in range(x, x + w):
            c = shot.px(px, py)
            counts[c] = counts.get(c, 0) + 1
    return max(counts.items(), key=lambda kv: kv[1])[0]

def motion_is_on(m, shot, origin):
    x, y, w, h = geometry(m, "motion")
    left = origin[0] + x + 4
    top = origin[1] + y + h // 2 - 3
    return dominant_color(shot, left, top, max(4, w // 3), 6) == DESKTOP_ACCENT

def volume_is_audible(m, shot, origin):
    x, y, w, h = geometry(m, "volume")
    left = origin[0] + x + 4
    top = origin[1] + y + h // 2 - 2
    return dominant_color(shot, left, top, max(4, w // 6), 4) == DESKTOP_ACCENT

def mode_btn_center(m, origin, i):
    return widget_center(m, origin, "mode%d" % i)

FM_W, FM_H = 760, 480
FM_ROW_H = 26
FM_CONTENT_PAD = 6
FM_INK_SUM = 600

def fm_origin():
    return app_origin(FIRST_APP_IDX, FM_H)

def fm_mark(m):
    return len(m.read_log())

def fm_wait_log(m, needle, mark, what, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in m.read_log()[mark:]:
            return
        time.sleep(0.3)
    raise Failure("%s: %s (log: %s)" % (current_test(), what, m.save_log("files")))

def fm_showing(m, path, mark, what, timeout=20.0):
    fm_wait_log(m, "[files] showing %s " % path, mark, what, timeout)

def fm_click(m, origin, name, double=False, mark=None):
    if mark is None:
        x, y, w, h = geometry(m, name)
    else:
        x, y, w, h = geometry_after(m, name, mark)
    point = (origin[0] + x + w // 2, origin[1] + y + h // 2)
    if double:
        m.double_click(*point)
    else:
        m.click(*point)

def fm_rows_with_text(m, shot, origin):
    x, y, w, h = geometry(m, "content")
    top = origin[1] + y + FM_CONTENT_PAD
    n = 0
    for row in range((h - FM_CONTENT_PAD) // FM_ROW_H):
        band = top + row * FM_ROW_H
        if shot.count_brighter(FM_INK_SUM, origin[0] + x + 40, band + 4, w - 60, FM_ROW_H - 8,
                               ignore=qemu_input.CURSOR_COLOR) > 0:
            n += 1
        elif shot.count_color(DESKTOP_ACCENT, origin[0] + x + 40, band + 4, w - 60, FM_ROW_H - 8) > (w - 60) * 4:
            n += 1
    return n

def fm_rows_visible(m):
    x, y, w, h = geometry(m, "content")
    return (h - FM_CONTENT_PAD) // FM_ROW_H

def fm_open(m):
    mark = fm_mark(m)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 1)
    fm_showing(m, "/home", mark, "Files never said it was showing /home")
    origin = fm_origin()
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) > 0, "the Files window listed nothing")
    return origin

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
TASKS_W, TASKS_H = 520, 420
TM_ROW_H = 26
TM_ROW_GAP = 2
TM_LIST_PAD = 6

def tm_selected_row(m, shot, origin):
    x, y, w, h = geometry(m, "list")
    left = origin[0] + x + TM_LIST_PAD
    top = origin[1] + y + TM_LIST_PAD
    rows = (h - 2 * TM_LIST_PAD + TM_ROW_GAP) // (TM_ROW_H + TM_ROW_GAP)
    for row in range(rows):
        band_y = top + row * (TM_ROW_H + TM_ROW_GAP) + TM_ROW_H // 2 - 2
        if dominant_color(shot, left + 6, band_y, w - 2 * TM_LIST_PAD - 12, 4) == DESKTOP_ACCENT:
            return row, rows
    return -1, rows

MENU_W = 124
MENU_ITEM_H = 22
MENU_MINIMIZE, MENU_CLOSE, MENU_FORCE_QUIT = 0, 1, 2
MENU_BG_RAW = 0x1E2430

TASKBAR_MENU_W = 132
TASKBAR_MENU_ITEM_H = 26
TASKBAR_MENU_BG_RAW = 0x1E2430

def taskbar_menu_row_center(slot, row):
    x = SLOTS_X + slot * (SLOT_W + SLOT_GAP)
    top = PANEL_TOP - TASKBAR_MENU_ITEM_H * 3
    return (x + TASKBAR_MENU_W // 2,
            top + row * TASKBAR_MENU_ITEM_H + TASKBAR_MENU_ITEM_H // 2)

FIRST_APP_IDX = 2

CONTENT_TOP = TITLEBAR_H + 2
CONTENT_BOTTOM = SCREEN_H - PANEL_H

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
        if not slot_occupied(shot, i):
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
    # And what the machine said, beside what it showed: a program that dies
    # under a test prints why on the serial line, and the temporary directory
    # that log lives in is gone by the time anybody reads the verdict.
    try:
        shutil.copyfile(machine.log_path, os.path.join(ARTIFACT_DIR, "%s.log" % name))
    except OSError:
        pass
    return dst

ICON_TILE_BOX = (122, 32, 48, 48)
ICON_TILE_SUM = 600
ICON_TILE_PIXELS = 900

def icon_tile_painted(shot):
    return shot.count_brighter(ICON_TILE_SUM, *ICON_TILE_BOX) > ICON_TILE_PIXELS

def desktop_is_painted(shot):
    return (shot.px(*EMPTY_DESKTOP) == DESKTOP_BG and
            icon_tile_painted(shot) and
            shot.px(512, PANEL_PROBE_Y) == PANEL_BG and
            shot.px(*START_PROBE) == START_GLYPH)

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
        ("README icon tile pixels (wanted more than)",
         shot.count_brighter(ICON_TILE_SUM, *ICON_TILE_BOX)
         if shot.count_brighter(ICON_TILE_SUM, *ICON_TILE_BOX) <= ICON_TILE_PIXELS
         else ICON_TILE_PIXELS, ICON_TILE_PIXELS),
        ("taskbar", shot.px(512, PANEL_PROBE_Y), PANEL_BG),
        ("Start glyph", shot.px(*START_PROBE), START_GLYPH),
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
        if not slot_occupied(shot, i):
            break
        if slot_is_focused(shot, i):
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
        timeout = 45.0 if name == "Browser" else 12.0
        want = min(i + 1, TASKBAR_MAX_SLOTS)
        beyond_taskbar = i >= TASKBAR_MAX_SLOTS
        # Before the click, not after it: a window that opens inside the
        # time a screendump takes is already in an "after the click" shot,
        # and the change it is supposed to prove is then compared away.
        before = m.screenshot() if beyond_taskbar else None
        m.double_click(x, y)
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

def desktop_menu_item(i, at=EMPTY_DESKTOP):
    return (at[0] + 40, at[1] + CTX_MENU_PAD + i * CTX_MENU_ITEM_H + CTX_MENU_ITEM_H // 2)

def test_desktop_context_menu(m):
    boot(m)
    m.right_click(*EMPTY_DESKTOP)
    wait_for(m,
             lambda s: s.count_color(CTX_MENU_BG, EMPTY_DESKTOP[0],
                                      EMPTY_DESKTOP[1], 120, 40) > 500,
             "right-clicking the desktop did not draw the context menu")

    mark = len(m.read_log())
    m.click(*desktop_menu_item(0))
    wait_for_windows(m, 1)
    wait_for_log_after(m, "[settings] showing Wallpaper", mark,
                       "Change Wallpaper... did not open Settings on its Wallpaper pane")

    origin = app_origin(FIRST_APP_IDX, SETTINGS_H)
    mark = len(m.read_log())
    m.click(*widget_center(m, origin, "wallpaper%d" % WALLPAPER_AURORA))
    wait_for_log_after(m, "[desktop] wallpaper Aurora", mark,
                       "picking Aurora did not repaint the desktop with it")
    shot = m.screenshot()
    check(shot.px(WALLPAPER_PROBE_X, 200) != desktop_px(200),
          "the desktop still shows the gradient at (%d, 200) after picking Aurora" % WALLPAPER_PROBE_X)

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
    shot = m.screenshot()
    check(shot.px(*START_PROBE) == START_GLYPH,
          "the Start button's glyph is not drawn at rest")
    check(shot.px(*START_HOLE) == shot.px(START_BG_PROBE[0], START_HOLE[1]),
          "the Start glyph has no hole in its middle - it is meant to be a "
          "donut, with the panel showing through (0x%06X, panel 0x%06X)"
          % (shot.px(*START_HOLE), shot.px(START_BG_PROBE[0], START_HOLE[1])))
    ring = sum(1 for dx in range(-START_RING_OUTER, START_RING_OUTER)
               if shot.px(START_CENTRE[0] + dx, START_HOLE[1]) == START_GLYPH)
    check(ring >= 2 * (START_RING_OUTER - START_RING_INNER) - 2,
          "the Start ring is %d solid accent pixels across its middle row; "
          "a donut of this size has two solid runs of about %d each"
          % (ring, START_RING_OUTER - START_RING_INNER))
    check(not start_highlighted(shot),
          "the Start button is highlighted before the pointer has reached it")

    m.move_to(*START_CLICK)
    wait_for(m, start_highlighted,
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
    wait_for(m, lambda s: slot_is_minimized(s, 0),
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

def no_toast_after_a_close(m, before, what):
    probe = toast_stripe_probe(0)
    time.sleep(1.5)
    shot = m.screenshot()
    check(shot.px(*probe) == before,
          "%s raised a toast (0x%06X where the desktop was 0x%06X) - a program that "
          "quit when it was asked to is not one that stopped unexpectedly"
          % (what, shot.px(*probe), before))

# M209: the README editor reported "stopped unexpectedly" every time it was
# closed, because it quit with status 1 and the compositor called every
# non-zero exit a crash. Both ways out are graded, and the toast's absence is
# the point - the window going away was already true.
def test_closing_the_readme_raises_no_toast(m):
    boot(m)
    before = m.screenshot().px(*toast_stripe_probe(0))
    m.double_click(ICONS[7][3], ICONS[7][2])
    wait_for_windows(m, 1)
    x, y = app_origin(FIRST_APP_IDX)
    m.click(*titlebar_button_center(x, y, 640, BTN_CLOSE))
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "the README editor's titlebar close did not close it")
    no_toast_after_a_close(m, before, "closing the README from its titlebar")

    m.double_click(ICONS[7][3], ICONS[7][2])
    wait_for_windows(m, 1)
    x, y = app_origin(FIRST_APP_IDX)
    m.click(x + EDITOR_MENU_X + 12, y + EDITOR_MENU_ROW_H // 2)
    wait_for(m,
             lambda s: s.count_color(EDITOR_MENU_BG, x + EDITOR_MENU_X,
                                      y + EDITOR_MENU_ROW_H + 2,
                                      EDITOR_MENU_ITEM_W, EDITOR_MENU_ITEM_H) > 200,
             "clicking File in the README editor did not open its dropdown")
    m.click(x + EDITOR_MENU_X + EDITOR_MENU_ITEM_W // 2,
            y + EDITOR_MENU_ROW_H + 3 * EDITOR_MENU_ITEM_H + EDITOR_MENU_ITEM_H // 2)
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "File > Quit did not close the README editor")
    no_toast_after_a_close(m, before, "File > Quit in the README editor")

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
    shot = wait_for(m, lambda s: s.px(600, HAIRLINE_Y) == ACCENT,
                    "releasing at the right edge did not snap the window to the right half")
    check(shot.px(200, 12) == desktop_px(12),
          "the snapped window is not confined to the right half")

def test_taskbar_right_click_force_quit(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)

    m.right_click(*slot_click(0))
    probe = (SLOTS_X + TASKBAR_MENU_W - 14, PANEL_TOP - TASKBAR_MENU_ITEM_H * 3 + 8)
    expected = panel_px(TASKBAR_MENU_BG_RAW, probe[1])
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

    origin = app_origin(FIRST_APP_IDX)
    m.click(origin[0] + TASKS_W - 40, origin[1] + 8)

    for _ in range(90):
        m.sendkey("down")

    def selection_at_end(shot):
        selected, rows = tm_selected_row(m, shot, origin)
        return selected >= 0 and selected >= rows - 2

    wait_for(m, selection_at_end,
             "the selection never reached the last row of the task list", timeout=25.0)

    m.click(*widget_center(m, origin, "end_task"))
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
    wait_for(m, lambda s: s.px(60, HAIRLINE_Y) == ACCENT,
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

WALLPAPER_PROBE_X = 960
WALLPAPER_AURORA = 4

def test_settings_persist_across_a_reboot(m):
    boot(m)
    m.double_click(ICON_X, ICONS[3][2])
    wait_for_windows(m, 1)

    origin = app_origin(FIRST_APP_IDX, SETTINGS_H)
    settings_pane(m, origin, "wallpaper")
    m.click(*widget_center(m, origin, "wallpaper0"))
    shot = wait_for(m, lambda s: s.px(WALLPAPER_PROBE_X, 200) == s.px(WALLPAPER_PROBE_X, 600),
                    "picking the Flat wallpaper did not flatten the desktop gradient")
    flat = shot.px(WALLPAPER_PROBE_X, 200)
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
        if shot.px(WALLPAPER_PROBE_X, 200) == flat and shot.px(WALLPAPER_PROBE_X, 600) == flat:
            return
        time.sleep(1.0)
    raise Failure("after restarting, the desktop came back with the default gradient "
                  "rather than the saved Flat wallpaper")

def test_session_restores_windows_across_a_reboot(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)
    x, y = app_origin(FIRST_APP_IDX)

    # M211: clear of where Paint, the second window, opens - it is 520x380
    # now, and at the old spot it covered the Clock this test is looking for.
    dest_x, dest_y = x + 600, y + 300
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

def test_wheel_scrolls_the_file_list(m):
    boot(m)
    origin = fm_open(m)
    mark = fm_mark(m)
    fm_click(m, origin, "place_applications")
    fm_showing(m, "/bin", mark, "Applications did not show /bin")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) >= fm_rows_visible(m) - 1,
             "/bin did not fill the list - there is nothing here long enough to scroll")

    x, y, w, h = geometry(m, "content")
    left = origin[0] + x + 40
    right = origin[0] + x + w - 20
    top = origin[1] + y + FM_CONTENT_PAD
    step = 48

    def band(shot, at):
        return [shot.px(px, py) for py in range(at, at + 20) for px in range(left, right, 2)]

    m.move_to(origin[0] + x + w // 2, origin[1] + y + h // 2)
    before = m.screenshot()
    want = band(before, top + 3 * step)
    check(any(p != want[0] for p in want),
          "the band this test scrolls to is blank - the file list is too short to scroll")

    m.wheel(3)
    time.sleep(1.0)
    got = band(m.screenshot(), top)
    if got != want:
        m.wheel(-6)
        time.sleep(1.0)
        got = band(m.screenshot(), top)
    check(got == want,
          "three wheel detents did not move the file list by exactly three steps of %d pixels" % step)

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
    wait_for(m, lambda s: s.px(600, HAIRLINE_Y) == ACCENT,
             "Ctrl+Alt+Right did not snap the window to the right half")

    m.sendkey("ctrl-alt-left")
    wait_for(m, lambda s: s.px(60, HAIRLINE_Y) == ACCENT and s.px(600, HAIRLINE_Y) == desktop_px(HAIRLINE_Y),
             "Ctrl+Alt+Left did not snap the window back to the left half")

    m.sendkey("ctrl-alt-down")
    wait_for(m, lambda s: count_app_windows(s) == 1 and focused_slot(s) == -1,
             "Ctrl+Alt+Down did not minimize the window (its taskbar button should "
             "still be there, just not focused)")

def test_drag_a_file_onto_the_desktop_opens_it(m):
    boot(m)
    origin = fm_open(m)
    x, y, w, h = geometry(m, "item0")
    m.press(origin[0] + x + 60, origin[1] + y + h // 2)
    m.move_held(600, 690)

    shot = m.screenshot()
    check(shot.count_color(DRAG_LABEL_BG, 600, 690, 200, 32) > 0,
          "no drag label followed the cursor - the drag never started")

    m.release()
    wait_for(m, lambda s: count_app_windows(s) == 2,
             "dropping a file on the desktop did not open it in the editor")

CLOCK_W, CLOCK_H = 200, 90
FILES_ORIGIN = app_origin(FIRST_APP_IDX)
TASKS_ORIGIN = app_origin(FIRST_APP_IDX + 1)
FILES_SLOT = 0
TASKS_SLOT = 1
_PAIR_PROBE_Y = TASKS_ORIGIN[1] + (FILES_ORIGIN[1] + CLOCK_H - TASKS_ORIGIN[1]) // 2
FILES_IN_FRONT_PROBE = (FILES_ORIGIN[0] + CLOCK_W, _PAIR_PROBE_Y)
TASKS_IN_FRONT_PROBE = (TASKS_ORIGIN[0] - BORDER, _PAIR_PROBE_Y)
FILES_TITLEBAR_CLICK = (FILES_ORIGIN[0] + 20, FILES_ORIGIN[1] - TITLEBAR_H // 2)
_TASKS_BTN_BAND = TASKS_ORIGIN[0] + TASKS_W - 74
_TASKS_FREE_LO = FILES_ORIGIN[0] + CLOCK_W + 4
if _TASKS_FREE_LO >= _TASKS_BTN_BAND:
    raise SystemExit("qemu_input_suite: the Clock is now wide enough to cover every clickable "
                     "part of Tasks' titlebar - the overlap tests need a different pair")
TASKS_TITLEBAR_CLICK = ((_TASKS_FREE_LO + _TASKS_BTN_BAND) // 2, TASKS_ORIGIN[1] - TITLEBAR_H // 2)
OVERLAP_CLICK = ((TASKS_ORIGIN[0] + FILES_ORIGIN[0] + CLOCK_W) // 2, _PAIR_PROBE_Y)

def open_overlapping_pair(m):
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[6][2])
    wait_for_windows(m, 2)
    wait_for(m, lambda s: window_edge_at(s, *TASKS_IN_FRONT_PROBE),
             "the second window launched did not start in front of the first")

def test_clicking_a_window_raises_it(m):
    open_overlapping_pair(m)

    m.click(*FILES_TITLEBAR_CLICK)
    shot = wait_for(m, lambda s: (window_edge_at(s, *FILES_IN_FRONT_PROBE) and
                                  focused_slot(s) == FILES_SLOT),
                    "clicking the covered window's titlebar did not raise and focus it")
    check(not window_edge_at(shot, *TASKS_IN_FRONT_PROBE),
          "the window that was raised did not cover the one that had been in front")

    m.click(*TASKS_TITLEBAR_CLICK)
    shot = wait_for(m, lambda s: (window_edge_at(s, *TASKS_IN_FRONT_PROBE) and
                                  focused_slot(s) == TASKS_SLOT),
                    "clicking the other window's titlebar did not raise and focus it back")
    check(not window_edge_at(shot, *FILES_IN_FRONT_PROBE),
          "both windows claim to be in front after the second raise")

def test_overlap_click_reaches_the_front_window(m):
    open_overlapping_pair(m)
    m.click(*FILES_TITLEBAR_CLICK)
    wait_for(m, lambda s: window_edge_at(s, *FILES_IN_FRONT_PROBE),
             "the covered window did not raise, so there is no occlusion to test")

    m.click(*OVERLAP_CLICK)
    time.sleep(2.0)
    shot = m.screenshot()
    check(focused_slot(shot) == FILES_SLOT,
          "a click in the overlap region focused slot %d - the window behind, "
          "which is not visible at that pixel" % focused_slot(shot))
    check(window_edge_at(shot, *FILES_IN_FRONT_PROBE),
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
    origin = fm_open(m)
    home_rows = fm_rows_with_text(m, m.screenshot(), origin)
    check(0 < home_rows < fm_rows_visible(m),
          "/home did not open as a short list (%d rows)" % home_rows)

    mark = fm_mark(m)
    fm_click(m, origin, "place_computer")
    fm_showing(m, "/", mark, "the Computer place did not show /")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) > 0, "/ was listed as nothing")

    mark = fm_mark(m)
    fm_click(m, origin, "item0", double=True)
    fm_showing(m, "/bin", mark, "double-clicking the first folder of / did not enter /bin")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) >= fm_rows_visible(m) - 1,
             "/bin did not fill the list with programs")

    mark = fm_mark(m)
    fm_click(m, origin, "back")
    fm_showing(m, "/", mark, "the Back button did not return to /")

    mark = fm_mark(m)
    fm_click(m, origin, "forward")
    fm_showing(m, "/bin", mark, "the Forward button did not go to /bin again")

    mark = fm_mark(m)
    m.sendkey("left")
    fm_showing(m, "/", mark, "the Left arrow did not go to the enclosing folder")

    mark = fm_mark(m)
    fm_click(m, origin, "place_home")
    fm_showing(m, "/home", mark, "the Home place did not show /home")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == home_rows,
             "/home came back with a different number of rows")

def fm_name_dialog(m, keys, typed, mark_before):
    m.sendkey(keys)
    geometry_after(m, "dialog_field", mark_before)
    m.type_text(typed)
    m.sendkey("ret")

def test_file_manager_creates_a_folder_and_deletes_it_full(m):
    boot(m)
    origin = fm_open(m)
    before = fm_rows_with_text(m, m.screenshot(), origin)

    x, y, w, h = geometry(m, "content")
    mark = fm_mark(m)
    m.right_click(origin[0] + x + w // 2, origin[1] + y + h - 20)
    fm_click(m, origin, "menu_new_folder", mark=mark)
    geometry_after(m, "dialog_field", mark)
    m.type_text("m112dir")
    m.sendkey("ret")
    fm_wait_log(m, "[files] created /home/m112dir", mark, "New Folder did not make /home/m112dir")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == before + 1,
             "the new folder did not appear in the list", timeout=15.0)

    mark = fm_mark(m)
    fm_click(m, origin, "item0", double=True)
    fm_showing(m, "/home/m112dir", mark, "item 0 was not the folder that was just made")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == 0, "the new folder was not empty")

    mark = fm_mark(m)
    fm_name_dialog(m, "ctrl-n", "inside", mark)
    fm_wait_log(m, "[files] created /home/m112dir/inside", mark, "Ctrl+N did not make a file")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == 1,
             "the new file did not appear inside the new folder", timeout=15.0)

    mark = fm_mark(m)
    m.sendkey("left")
    fm_showing(m, "/home", mark, "the Left arrow did not leave the folder")

    fm_click(m, origin, "item0")
    time.sleep(0.4)
    mark = fm_mark(m)
    m.sendkey("backspace")
    fm_wait_log(m, "[files] trashed /home/m112dir", mark, "Backspace did not move the folder to the Trash")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == before,
             "the folder was still listed after it went to the Trash", timeout=15.0)

    mark = fm_mark(m)
    fm_click(m, origin, "place_trash")
    fm_showing(m, FILES_TRASH, mark, "the Trash place did not show the Trash")
    fm_click(m, origin, "item0")
    time.sleep(0.4)
    mark = fm_mark(m)
    m.sendkey("backspace")
    geometry_after(m, "dialog_ok", mark)
    m.sendkey("ret")
    fm_wait_log(m, "[files] deleted %s/m112dir" % FILES_TRASH, mark,
                "deleting from the Trash did not remove the folder and the file inside it")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == 0, "the Trash is not empty afterwards")

FILES_TRASH = "/home/.Trash"

def test_file_manager_refuses_a_new_name_that_escapes_the_folder(m):
    boot(m)
    origin = fm_open(m)
    before = fm_rows_with_text(m, m.screenshot(), origin)

    mark = fm_mark(m)
    fm_name_dialog(m, "ctrl-shift-n", "m112esc", mark)
    fm_wait_log(m, "[files] created /home/m112esc", mark, "the folder to escape into was not created")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == before + 1,
             "the folder to escape into was not listed", timeout=15.0)

    mark = fm_mark(m)
    fm_name_dialog(m, "ctrl-n", "m112esc/inside", mark)
    time.sleep(3.0)
    check("[files] created" not in m.read_log()[mark:],
          "a name with a path separator in it created something")
    check(fm_rows_with_text(m, m.screenshot(), origin) == before + 1,
          "a name with a path separator in it changed this folder")

    mark = fm_mark(m)
    fm_click(m, origin, "item0", double=True)
    fm_wait_log(m, "[files] showing /home/m112esc 0 items\n", mark,
                "a name with a path separator in it escaped into the subfolder")

    mark = fm_mark(m)
    m.sendkey("left")
    fm_showing(m, "/home", mark, "the Left arrow did not leave the folder")
    fm_click(m, origin, "item0")
    time.sleep(0.4)
    mark = fm_mark(m)
    m.sendkey("backspace")
    fm_wait_log(m, "[files] trashed /home/m112esc", mark, "the folder this test made was not cleaned up")

def test_file_manager_puts_back_and_undoes(m):
    boot(m)
    origin = fm_open(m)
    before = fm_rows_with_text(m, m.screenshot(), origin)

    fm_click(m, origin, "item0")
    time.sleep(0.4)
    mark = fm_mark(m)
    m.sendkey("ctrl-d")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == before + 1,
             "Ctrl+D did not duplicate the selected file", timeout=15.0)

    mark = fm_mark(m)
    m.sendkey("backspace")
    fm_wait_log(m, "[files] trashed /home/", mark, "Backspace did not trash the duplicate")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == before,
             "the duplicate was still listed after it went to the Trash", timeout=15.0)

    mark = fm_mark(m)
    m.sendkey("ctrl-z")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == before + 1,
             "Ctrl+Z did not bring the trashed file back", timeout=15.0)

    fm_click(m, origin, "item0")
    time.sleep(0.4)
    m.sendkey("ctrl-a")
    time.sleep(0.4)
    mark = fm_mark(m)
    fm_click(m, origin, "place_trash")
    fm_showing(m, FILES_TRASH, mark, "the Trash did not open")
    check(fm_rows_with_text(m, m.screenshot(), origin) == 0,
          "undo left something behind in the Trash")

    mark = fm_mark(m)
    fm_click(m, origin, "back")
    fm_showing(m, "/home", mark, "Back did not return home")
    m.sendkey("ctrl-a")
    time.sleep(0.4)
    mark = fm_mark(m)
    m.sendkey("backspace")
    fm_wait_log(m, "[files] trashed /home/", mark, "Backspace did not trash everything selected")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == 0,
             "trashing everything left rows behind", timeout=15.0)

    mark = fm_mark(m)
    fm_click(m, origin, "place_trash")
    fm_showing(m, FILES_TRASH, mark, "the Trash did not open")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == before + 1,
             "the Trash does not hold what was put in it")
    m.sendkey("ctrl-a")
    time.sleep(0.4)
    x, y, w, h = geometry(m, "item0")
    mark = fm_mark(m)
    m.right_click(origin[0] + x + 60, origin[1] + y + h // 2)
    fm_click(m, origin, "menu_put_back", mark=mark)
    fm_wait_log(m, "[files] put back /home/", mark, "Put Back did not restore anything")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == 0, "Put Back left things in the Trash")

    mark = fm_mark(m)
    fm_click(m, origin, "place_home")
    fm_wait_log(m, "[files] showing /home %d items\n" % (before + 1), mark,
                "Put Back did not return every file to /home")

def test_file_manager_searches_and_quick_looks(m):
    boot(m)
    origin = fm_open(m)
    mark = fm_mark(m)
    fm_click(m, origin, "search")
    time.sleep(0.4)
    m.type_text("notes")
    fm_wait_log(m, '[files] search "notes" in /home: 1 results', mark,
                "searching home for notes did not find the one file")
    wait_for(m, lambda s: fm_rows_with_text(m, s, origin) == 1, "the search result was not listed")

    fm_click(m, origin, "item0")
    time.sleep(0.4)
    mark = fm_mark(m)
    m.sendkey("spc")
    fm_wait_log(m, "[files] quick look notes", mark, "Space did not open Quick Look")
    px, py, pw, ph = geometry_after(m, "preview", mark)
    shot = m.screenshot()
    check(shot.count_brighter(FM_INK_SUM, origin[0] + px + 20, origin[1] + py + 60, pw - 40, ph - 80,
                              ignore=qemu_input.CURSOR_COLOR) > 200,
          "Quick Look opened but shows no text from the file")

    m.sendkey("esc")
    time.sleep(0.6)
    mark = fm_mark(m)
    m.sendkey("ctrl-i")
    geometry_after(m, "info", mark)

    m.sendkey("esc")
    time.sleep(0.4)
    mark = fm_mark(m)
    fm_click(m, origin, "search")
    m.sendkey("esc")
    fm_showing(m, "/home", mark, "Escape in the search field did not go back to the folder")

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
    first = fm_open(m)
    mark = fm_mark(m)
    m.double_click(ICON_X, ICONS[2][2])
    wait_for_windows(m, 2)
    fm_showing(m, "/home", mark, "the second Files window never listed /home")
    second = app_origin(FIRST_APP_IDX + 1, FM_H)

    before = fm_rows_with_text(m, m.screenshot(), second)
    check(before > 0, "the second Files window listed nothing")

    fm_click(m, second, "item0")
    time.sleep(0.4)
    m.sendkey("ctrl-d")
    wait_for(m, lambda s: fm_rows_with_text(m, s, second) == before + 1,
             "the copy did not appear in the window that made it", timeout=15.0)
    m.click(first[0] + 20, first[1] - TITLEBAR_H // 2)
    wait_for(m, lambda s: fm_rows_with_text(m, s, first) == before + 1,
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

    origin = app_origin(FIRST_APP_IDX, SETTINGS_H)
    settings_pane(m, origin, "display")
    m.click(*mode_btn_center(m, origin, SMALL_MODE_INDEX))
    wait_for(m, lambda s: (s.width, s.height) == SMALL_MODE,
             "clicking a resolution did not change the display size")
    wait_for(m, _bar_spans,
             "the taskbar does not span the new %dx%d display - its buffer was not reallocated"
             % SMALL_MODE, timeout=20.0)

    moved = (min(origin[0], SMALL_MODE[0] - SETTINGS_W),
             max(min(origin[1], SMALL_MODE[1] - PANEL_H - SETTINGS_H), CONTENT_TOP))
    keep_x, keep_y, keep_w, keep_h = geometry(m, "keep")
    check(moved[0] + keep_x + keep_w < SMALL_MODE[0] and moved[1] + keep_y + keep_h < SMALL_MODE[1],
          "the Keep button is off the edge of the smaller screen it is asking about")
    m.click(moved[0] + keep_x + keep_w // 2, moved[1] + keep_y + keep_h // 2)

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
    origin = app_origin(FIRST_APP_IDX, SETTINGS_H)
    settings_pane(m, origin, "display")
    m.click(*mode_btn_center(m, origin, SMALL_MODE_INDEX))
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

    origin = app_origin(FIRST_APP_IDX, SETTINGS_H)
    settings_pane(m, origin, "appearance")
    wait_for(m, lambda s: motion_is_on(m, s, origin),
             "Settings did not open with animations on, which is the default")
    m.click(*widget_center(m, origin, "motion"))
    wait_for(m, lambda s: not motion_is_on(m, s, origin),
             "clicking the Motion switch did not turn animations off")

    settings_pane(m, origin, "sound")
    check(volume_is_audible(m, m.screenshot(), origin),
          "the volume did not start un-muted, which is the default")
    m.click(*widget_point(m, origin, "volume", 0.0, 0.5))
    wait_for(m, lambda s: not volume_is_audible(m, s, origin),
             "dragging the volume slider to its left end did not mute it")

    boots_before = m.read_log().count(BOOT_MARKER)
    m.sendkey("ctrl-spc")
    m.type_text("reboot")
    m.sendkey("ret")
    deadline = time.time() + 300
    while time.time() < deadline and m.read_log().count(BOOT_MARKER) <= boots_before:
        time.sleep(1.0)
    check(m.read_log().count(BOOT_MARKER) > boots_before, "the machine never restarted")

    wait_for(m, lambda s: count_app_windows(s) >= 1,
             "Settings did not come back after the restart", timeout=120.0)
    geometry_after(m, "pane_appearance", m.read_log().rfind(BOOT_MARKER))
    settings_pane(m, origin, "appearance")
    check(not motion_is_on(m, m.screenshot(), origin),
          "the Motion switch forgot it had been turned off across a restart")
    settings_pane(m, origin, "sound")
    check(not volume_is_audible(m, m.screenshot(), origin),
          "the volume forgot it had been muted across a restart")

def clock_ink_span(shot):
    y0 = PANEL_TOP + PANEL_H // 2 - 7
    columns = []
    for x in range(shot.width - 1, shot.width - 160, -1):
        bright = any(channel_sum(shot.px(x, y)) > 0x200 for y in range(y0, y0 + 14))
        columns.append(bright)
    right = None
    left = None
    gap = 0
    for i, bright in enumerate(columns):
        if bright:
            if right is None:
                right = i
            left = i
            gap = 0
        elif right is not None:
            # A word space after a narrow "1" is nine columns of nothing, so
            # "7:41 PM" read as "PM" alone at 8. The workspace dots to the
            # left are never this bright, so the clock is still all it finds.
            gap += 1
            if gap > 12:
                break
    if right is None:
        return 0
    return left - right + 1

def test_twelve_hour_clock_reaches_the_taskbar(m):
    boot(m)
    m.double_click(ICON_X, ICONS[3][2])
    wait_for_windows(m, 1)
    origin = app_origin(FIRST_APP_IDX, SETTINGS_H)
    settings_pane(m, origin, "date_time")

    before = clock_ink_span(m.screenshot())
    check(before > 20, "the taskbar clock could not be found (%d columns)" % before)

    m.click(*widget_center(m, origin, "clock24"))
    wait_for(m, lambda s: clock_ink_span(s) > before + 12,
             "turning the 24-hour clock off did not widen the taskbar clock to say AM or PM",
             timeout=15.0)

    m.click(*widget_center(m, origin, "clock24"))
    wait_for(m, lambda s: abs(clock_ink_span(s) - before) <= 3,
             "turning the 24-hour clock back on did not put the taskbar clock back",
             timeout=15.0)

class Region:

    def __init__(self, x, y, w, h, why):
        self.x, self.y, self.w, self.h, self.why = x, y, w, h, why

    def contains(self, px, py):
        return self.x <= px < self.x + self.w and self.y <= py < self.y + self.h

CLOCK_REGION = Region(1024 - 80, PANEL_TOP, 80, PANEL_H, "the taskbar clock ticks")

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
        Region(0, PANEL_TOP, 400, PANEL_H, "the taskbar gaining a button"),
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
        Region(0, PANEL_TOP, 400, PANEL_H, "the taskbar losing a button"),
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
        Region(x - 40, y - 60, FM_W + 80, FM_H + 100, "the Files window, its chrome and its shadow"),
        Region(0, PANEL_TOP, 400, PANEL_H, "the taskbar"),
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
        Region(0, PANEL_TOP, 500, PANEL_H, "the taskbar"),
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
                    "the page painted its background but the home page's blue "
                    "banner never appeared on it - the stylesheet was not "
                    "applied, or the frame never reached the window",
                    timeout=45.0)

    # Chromium's own process model, or it is not Chromium: the launcher's
    # exec line is followed by the children it started - a renderer, the
    # network and storage services - each an exec of /proc/self/exe the
    # kernel logs as a load. M173 made this true on the desktop; before it
    # the same test passed with --single-process.
    log = m.read_log()
    after = log[log.index("browser: exec"):] if "browser: exec" in log else ""
    children = after.count("[elf] loaded") - 1
    check(children >= 3,
          "the browser started %d child process(es); Chromium here is a "
          "renderer, a network service and a storage service at the least "
          "(M173)" % children)
    check("Received signal" not in after and "Tracing not initialized" not in after,
          "a browser child process died - see the guest log")

    region = _page_columns(shot)
    check(region is not None,
          "no column of this screen has a dense run of white in it, so there "
          "is no page on it - even though something painted white somewhere")
    x0, x1 = region
    check(x1 - x0 > 400,
          "the page is only %d columns wide; a browser window here is ~780"
          % (x1 - x0))

    def _page_pixels(s):
        blue = red = text = 0
        for y in range(0, s.height, 2):
            for x in range(x0, x1 + 1, 2):
                c = s.px(x, y)
                r, g, b = (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF
                if b > 150 and b - r > 50:
                    blue += 1
                if r > 150 and r - b > 50:
                    red += 1
                if (r < 96 and g < 96 and b < 96 and x0 + 3 < x < x1 - 3 and
                        s.px(x - 3, y) == 0xFFFFFF and
                        s.px(x + 3, y) == 0xFFFFFF):
                    text += 1
        return blue, red, text

    # The banner is a stylesheet colour and is on screen the moment the
    # renderer's first frame is; the glyphs come a moment later, from the
    # font service in the browser process over mojo (M173). A shot taken at
    # the first sight of blue can be honest and textless.
    shot = wait_for(m, lambda s: _page_pixels(s)[2] > 100,
                    "the page painted its banner but no text followed it - "
                    "the renderer got no glyphs from the browser's font service",
                    timeout=30.0)
    blue, red, text = _page_pixels(shot)

    check(blue > 5000,
          "only %d blue pixels on the page - the home page's banner did "
          "not decode, or did not draw" % blue)
    check(blue > 20 * (red + 1),
          "%d blue against %d red on a page whose banner is blue. If those "
          "are the wrong way round, this surface has red and blue swapped - "
          "see user_space/binaries/nsfb_leanos.c, where the pixel format is "
          "claimed to match the compositor's exactly" % (blue, red))
    check(text > 50,
          "only %d dark pixels between white ones - freetype rasterised no "
          "text onto the page" % text)

# The Browser is //chrome since M187, and Ctrl+L is how a person reaches its
# omnibox: it focuses the field and selects what is in it, so what is typed
# next replaces the address. No coordinate in it, which is the point - the
# tab strip and the toolbar put the omnibox somewhere content_shell's
# address bar was not. It takes the omnibox a moment under TCG to take
# focus: with 0.4 s after Ctrl+L the first key went to the page, and
# "ttp://10.0.2.100:7778/" was searched for on Google.
#
# Not while a page is still loading, though: the tab's throbber and the stop
# button are up until the load finishes, and a finishing load puts its own
# address back in an omnibox nobody has committed an edit to - so an address
# typed into the home page while it was still loading was lost, and the test
# timed out looking at the home page. The tab strip and the reload button
# hold still once nothing is loading; the omnibox itself is left out, because
# a focused one has a caret that blinks.
def _toolbar_pixels(s):
    return (tuple(s.px(x, y) for y in range(88, 126, 3)
                  for x in range(180, s.width, 3)) +
            tuple(s.px(x, y) for y in range(130, 166, 3)
                  for x in range(180, 296, 3)))

def _focus_url_bar(m):
    deadline = time.time() + 45.0
    last = None
    still = 0
    while time.time() < deadline and still < 3:
        now = _toolbar_pixels(m.screenshot())
        still = still + 1 if now == last else 0
        last = now
        time.sleep(0.5)
    m.sendkey("ctrl-l")
    time.sleep(2.0)

# The omnibox reruns autocomplete on every key, and under TCG that takes
# longer than the 30 ms type_text leaves between keys: typed at that rate,
# "file:///usr/share/browser/flex.html" arrived as "l/". A quarter of a
# second a key is what a person gives it.
def _type_address(m, text):
    for ch in text:
        m.sendkey(qemu_input._qemu_keyname(ch))
        time.sleep(0.25)
    time.sleep(1.0)

def test_browser_loads_a_page_from_another_machine(m):
    boot(m)
    browser = ICONS[-1]
    m.double_click(browser[3], browser[2])
    wait_for_windows(m, 1, timeout=45.0)
    wait_for(m, lambda s: _blue_in_page(s) > 5000,
             "the browser never finished showing its own start page",
             timeout=45.0)

    _focus_url_bar(m)
    _type_address(m, qemu_input.NET_PAGE_URL)
    started = time.time()
    m.sendkey("ret")

    def block_on_screen(s):
        return s.count_color(qemu_input.NET_PAGE_BLOCK, 180, 160, 820, 560) > 20000

    # Ten seconds from M116 to M186. //chrome keeps Chromium's site isolation,
    # so leaving the file:// home page for 10.0.2.100 starts a renderer for the
    # new site whatever --renderer-process-limit says - and starting one here
    # is exec'ing a 306 MB program, copied page by page: about three of these
    # seconds (M187, measured alone at 10.5, 11.2 and 11.4 s). The condition
    # for ten again is an exec that shares a program's text rather than
    # copying it.
    BUDGET_S = 15.0
    wait_for(m, block_on_screen,
             "the page from 10.0.2.100 was not fully on screen within %.0f s of "
             "pressing Enter - the network, TCP or the browser's fetch loop "
             "is losing time (see M116)" % BUDGET_S,
             timeout=BUDGET_S)
    print("    page from another machine on screen %.1f s after Enter"
          % (time.time() - started))

def test_popup_window_opens_captures_and_dismisses(m):
    boot(m)
    # The launcher starts /bin/popuptest: a blue main window and a magenta
    # popup the compositor places above it (M174).
    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "Ctrl+Space did not open the launcher")
    m.type_text("popuptest")
    m.sendkey("ret")

    POPUP = 0xE020E0
    MAIN = 0x203A8C
    wait_for(m, lambda s: s.count_color(MAIN, 0, 0, s.width, s.height) > 40000,
             "popuptest's main window never appeared", timeout=20.0)
    shot = wait_for(m, lambda s: s.count_color(POPUP, 0, 0, s.width, s.height) > 8000,
                    "the popup the main window asked for was never placed - the "
                    "compositor drew nothing where a client-positioned window "
                    "belongs (M174)", timeout=15.0)
    print("    popup placed: %d magenta pixels over the window"
          % shot.count_color(POPUP, 0, 0, shot.width, shot.height))

    # A click far from the popup. The popup holds the mouse (capture), so the
    # compositor delivers this click to it with out-of-bounds coordinates,
    # which is how a menu learns it was dismissed. The popup goes; the main
    # window stays.
    m.click(820, 560)
    wait_for(m, lambda s: s.count_color(POPUP, 0, 0, s.width, s.height) < 1000,
             "the popup did not close when a captured click landed outside it "
             "(M174: the mouse grab did not route the outside click)",
             timeout=15.0)
    check(m.screenshot().count_color(MAIN, 0, 0, m.screenshot().width, m.screenshot().height) > 40000,
          "the main window vanished with the popup - a popup's dismissal took "
          "its parent with it")

def test_browser_survives_an_empty_flex_container(m):
    boot(m)
    browser = ICONS[-1]
    m.double_click(browser[3], browser[2])
    wait_for_windows(m, 1, timeout=45.0)
    wait_for(m, lambda s: _blue_in_page(s) > 5000,
             "the browser never finished showing its own start page",
             timeout=45.0)
    _focus_url_bar(m)
    _type_address(m, "file:///usr/share/browser/flex.html")
    m.sendkey("ret")
    wait_for(m, lambda s: s.count_color(0xE02020, 180, 160, 820, 560) > 20000,
             "the block after an empty flex container never appeared - the "
             "layout failed (M117: malloc(0) returning NULL was how) or the "
             "browser died on its assertion",
             timeout=20.0)
    check(count_app_windows(m.screenshot()) == 1,
          "the browser window is gone - it died on the page")

def _close_button_on_screen(shot):
    hits = [(x, y) for y in range(0, min(shot.height, PANEL_TOP), 2)
            for x in range(0, shot.width, 2) if shot.px(x, y) == BTN_CLOSE_COLOR]
    if not hits:
        return None
    xs = sorted(h[0] for h in hits)
    ys = sorted(h[1] for h in hits)
    return xs[len(xs) // 2], ys[len(ys) // 2]

def test_browser_closes_from_its_titlebar_button(m):
    # M199: the close button asks with SIGTERM and Chromium answers the way it
    # should - it writes its profile and exits 0. The compositor took "exited
    # 0" for "still running", and the dead browser's window stayed on the
    # screen until its process slot happened to be recycled. On the laptop that
    # was a browser that froze when closed. The button also has to be ON the
    # screen: a 900-pixel window cascaded to x=180 put it past the edge of
    # this 1024-pixel display.
    boot(m)
    browser = ICONS[-1]
    m.double_click(browser[3], browser[2])
    wait_for_windows(m, 1, timeout=45.0)
    shot = wait_for(m, lambda s: _blue_in_page(s) > 5000,
                    "the browser never finished showing its own start page",
                    timeout=45.0)
    button = _close_button_on_screen(shot)
    check(button is not None and button[0] < shot.width - 4,
          "the browser's close button is not on the screen (found at %r) - "
          "its window was placed past the right edge" % (button,))
    m.click(*button)
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "the browser's window stayed after its close button was clicked",
             timeout=30.0)
    m.double_click(ICON_X, ICONS[4][2])
    wait_for_windows(m, 1, timeout=20.0)

def _new_tab_button(shot, page_left):
    # The tab strip is the light band between the titlebar and the toolbar,
    # and the + is the last dark mark on it: the tab list's own glyphs, each
    # tab's title and close box are all to its left.
    marks = sorted({x for y in range(88, 122) for x in range(page_left + 1, shot.width)
                    if sum(((shot.px(x, y) >> s) & 0xFF) for s in (16, 8, 0)) < 250})
    if not marks:
        return None
    last = [marks[-1]]
    for x in reversed(marks[:-1]):
        if last[-1] - x > 3:
            break
        last.append(x)
    return ((min(last) + max(last)) // 2, 104)

def test_browser_new_tab_button_opens_a_tab(m):
    # M202: every views::Button that acts on release - New Tab, back, reload,
    # a tab's close box - ignored every click, because this ozone platform
    # left the released button out of a release's flags and Chromium only
    # fires a button whose flags name a button it answers to. Ctrl+T worked
    # and so did anything that acts on press, which is how it survived from
    # M171 to the laptop.
    boot(m)
    browser = ICONS[-1]
    m.double_click(browser[3], browser[2])
    wait_for_windows(m, 1, timeout=45.0)
    shot = wait_for(m, lambda s: _blue_in_page(s) > 5000,
                    "the browser never finished showing its own start page",
                    timeout=45.0)
    page = _page_columns(shot)
    check(page is not None, "could not find the browser's page on the screen")
    plus = _new_tab_button(shot, page[0])
    check(plus is not None, "could not find the New Tab button on the tab strip")
    m.click(*plus)

    # A second tab pushes the + a tab's width to the right; nothing else does.
    def moved(s):
        now = _new_tab_button(s, page[0])
        return now is not None and now[0] > plus[0] + 100
    wait_for(m, moved,
             "clicking the New Tab button at %r opened no tab - the click "
             "reached the button (it highlights under the cursor) and the "
             "button never fired" % (plus,),
             timeout=45.0)

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

# M207: the Wi-Fi wizard, on a machine with no wired card and the simulated
# radio fw_cfg switches on - five access points that hold their own
# passphrases and run the authenticator's half of WPA2. The tray grows by the
# radio's four bars, and only on a machine that has one.
WIFI_MACHINE = dict(extra_args=["-fw_cfg", "name=opt/leanos/wireless,string=simulated"],
                    wired_network=False)
WIFI_TRAY_W = TRAY_W + 18 + TRAY_PAD
WIFI_BARS = (1024 - WIFI_TRAY_W + TRAY_PAD, PANEL_TOP + PANEL_H // 2 - 7, 18, 14)
WIFI_W, WIFI_H = 460, 500
WIFI_PASSWORD = "lean os wireless"

def is_red(c):
    return ((c >> 16) & 255) > 170 and ((c >> 8) & 255) < 120 and (c & 255) < 120

def is_green(c):
    return ((c >> 8) & 255) > 150 and ((c >> 16) & 255) < 140

def is_blue(c):
    return (c & 255) > 180 and ((c >> 16) & 255) < 120

def count_where(shot, test, x, y, w, h):
    n = 0
    for py in range(y, y + h):
        for px in range(x, x + w):
            if test(shot.px(px, py)):
                n += 1
    return n

# Lit bars are the clock's ink, which reads 0xA9AEB9 through the panel - a
# channel sum of 528; unlit ones are a faint white over the panel, about 250.
def lit_wifi_bars(shot):
    x, y, w, h = WIFI_BARS
    return shot.count_brighter(450, x, y, w, h)

def geometry_after(m, name, mark, timeout=25.0):
    needle = "[geometry] %s " % name
    deadline = time.time() + timeout
    while time.time() < deadline:
        log = m.read_log()
        at = log.rfind(needle)
        if at >= mark:
            fields = log[at + len(needle):].split("\n", 1)[0].split()
            if len(fields) >= 4:
                return tuple(int(v) for v in fields[:4])
        time.sleep(0.3)
    raise Failure("%s: the wizard never reported where it put %r after that step (log: %s)"
                  % (current_test(), name, m.save_log("no-geometry")))

def wait_for_log(m, text, what, timeout=40.0, count=1):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if m.read_log().count(text) >= count:
            return
        time.sleep(0.3)
    raise Failure("%s (log: %s)" % (what, m.save_log("wifi")))

def open_wifi(m):
    shot = m.screenshot()
    x, y, w, h = WIFI_BARS
    check(shot.count_brighter(150, x, y, w, h) > 0,
          "the taskbar shows no radio, on a machine that has one")
    m.click(x + w // 2, y + h // 2)
    wait_for_windows(m, 1)
    return app_origin(FIRST_APP_IDX, WIFI_H)

def choose(m, origin, row):
    x, y, w, h = geometry(m, "network%d" % row)
    mark = len(m.read_log())
    m.click(origin[0] + x + 60, origin[1] + y + h // 2)
    return mark

def join_workshop(m, origin):
    mark = choose(m, origin, 0)
    geometry_after(m, "password", mark)
    m.type_text(WIFI_PASSWORD)
    m.sendkey("ret")
    wait_for_log(m, "[wifi] connected to Workshop", "the right password did not join Workshop")

def test_wifi_wizard_joins_after_a_wrong_password(m):
    boot(m)
    check(lit_wifi_bars(m.screenshot()) == 0, "the radio's bars are lit before anything was joined")
    origin = open_wifi(m)
    x, y, w, h = geometry(m, "network0")
    wait_for(m, lambda s: s.count_brighter(450, origin[0] + x + 40, origin[1] + y + 4, 120, h - 8) > 40,
             "the strongest network's name was never drawn in the list")

    mark = choose(m, origin, 0)
    px, py, pw, ph = geometry_after(m, "password", mark)
    m.type_text("not the password")
    m.sendkey("ret")
    wait_for_log(m, "[wifi] not joined: the handshake failed - the password is wrong",
                 "a wrong password was not named as one")
    check("[net] IP now runs over wlan0" not in m.read_log(),
          "IP was attached to a network whose handshake failed")
    hint = (origin[0] + px, origin[1] + py + ph + 4, pw, 24)
    wait_for(m, lambda s: count_where(s, is_red, *hint) > 30,
             "the password page did not come back saying the password was wrong")

    m.type_text(WIFI_PASSWORD)
    m.sendkey("ret")
    wait_for_log(m, "[wifi] connected to Workshop", "the right password did not join Workshop")
    check("[net] 192.168.77.23/24 via 192.168.77.1" in m.read_log(),
          "the network's DHCP server was never heard - no address came over the radio")
    steps = (origin[0] + 14, origin[1] + 40, 200, 80)
    wait_for(m, lambda s: count_where(s, is_green, *steps) > 60,
             "the progress page never showed its steps done")

    bx, by, bw, bh = geometry(m, "progress_button")
    m.click(origin[0] + bx + bw // 2, origin[1] + by + bh // 2)
    x, y, w, h = geometry(m, "network0")
    wait_for(m, lambda s: is_blue(s.px(origin[0] + x + w - 6, origin[1] + y + h // 2)),
             "the list does not show the joined network as connected")
    wait_for(m, lambda s: lit_wifi_bars(s) > 20, "the taskbar's radio bars did not light once joined")

def test_wifi_remembers_a_network_across_a_reboot(m):
    boot(m)
    origin = open_wifi(m)
    join_workshop(m, origin)
    boots_before = m.read_log().count(BOOT_MARKER)
    m.sendkey("ctrl-spc")
    m.type_text("reboot")
    m.sendkey("ret")
    wait_for_log(m, BOOT_MARKER, "the machine never came back up after `reboot`", timeout=300.0,
                 count=boots_before + 1)
    wait_for_log(m, "[wifi] rejoining a remembered network",
                 "after a reboot the machine did not go back to the network it remembered", timeout=90.0)
    wait_for_log(m, "[wifi] connected to Workshop", "the remembered network was not joined again",
                 timeout=60.0, count=2)
    wait_for(m, lambda s: lit_wifi_bars(s) > 20, "the radio's bars stayed dark after rejoining", timeout=30.0)

def test_wifi_open_network_needs_no_password_and_wpa3_is_refused(m):
    boot(m)
    origin = open_wifi(m)
    geometry(m, "network2")
    mark = choose(m, origin, 2)
    time.sleep(2.0)
    check(m.read_log().rfind("[geometry] password ") < mark,
          "choosing a WPA3-only network asked for a password this machine could not use")
    check("[wifi] joining Laboratory" not in m.read_log(),
          "the kernel was asked to join a network whose security it cannot speak")
    wait_for(m, lambda s: count_where(s, is_red, origin[0] + 14, origin[1] + 40, 420, 20) > 30,
             "choosing an unsupported network did not say why nothing happened")
    mark = choose(m, origin, 1)
    wait_for_log(m, "[wifi] connected to Corner Cafe", "the open network was not joined")
    check(m.read_log().rfind("[geometry] password ") < mark,
          "an open network asked for a password")


PAINT_W, PAINT_H = 520, 380
PAINT_TOOLBAR_H = 40
PAINT_RED = 0xE74C3C

def is_whiteish(c):
    return channel_sum(c) >= 3 * 235

def test_a_painting_becomes_the_desktop_picture(m):
    boot(m)
    mark = len(m.read_log())
    m.double_click(ICON_X, ICONS[5][2])
    wait_for_windows(m, 1)
    paint = app_origin(FIRST_APP_IDX, PAINT_H)
    time.sleep(0.5)
    m.click(paint[0] + 10 + 26 + 10, paint[1] + 20)
    y = paint[1] + PAINT_TOOLBAR_H + 170
    m.drag(paint[0] + 40, y, paint[0] + 480, y, steps=12)
    m.click(paint[0] + PAINT_W - 10 - 29, paint[1] + 20)
    wait_for_log_after(m, "[paint] saved /home/Pictures/Painting 1.bmp", mark,
                       "Paint's Save button did not save the painting into Pictures")
    m.sendkey("alt-f4")
    wait_for(m, lambda s: count_app_windows(s) == 0, "Paint did not close")

    mark = len(m.read_log())
    m.right_click(*EMPTY_DESKTOP)
    time.sleep(0.4)
    m.click(*desktop_menu_item(0))
    wait_for_log_after(m, "[settings] showing Wallpaper", mark,
                       "the desktop's Change Wallpaper... did not open Settings on Wallpaper")
    settings = app_origin(FIRST_APP_IDX, SETTINGS_H)
    mark = len(m.read_log())
    m.click(*widget_center(m, settings, "choose_picture"))
    fm_showing(m, "/home/Pictures", mark, "Choose a Picture... did not open Files on Pictures")

    files = app_origin(FIRST_APP_IDX + 1, FM_H)
    x, y, w, h = geometry_after(m, "item0", mark)
    mark = len(m.read_log())
    m.right_click(files[0] + x + 60, files[1] + y + h // 2)
    fm_click(m, files, "menu_set_desktop_picture", mark=mark)
    fm_wait_log(m, "[files] desktop picture /home/Pictures/Painting 1.bmp", mark,
                "Set as Desktop Picture did not take the painting")
    fm_wait_log(m, "[desktop] wallpaper picture Painting 1.bmp", mark,
                "the desktop did not repaint with the painting")

    m.sendkey("alt-f4")
    time.sleep(0.6)
    m.sendkey("alt-f4")
    wait_for(m, lambda s: count_app_windows(s) == 0, "Files and Settings did not close")
    shot = m.screenshot()
    check(is_whiteish(shot.px(*EMPTY_DESKTOP)),
          "the desktop at %r is 0x%06X, not the painting's white paper"
          % (EMPTY_DESKTOP, shot.px(*EMPTY_DESKTOP)))
    red = shot.count_color(PAINT_RED, 140, 360, 880, 50)
    check(red > 5000, "the painting's red stroke is %d pixels on the desktop, not a band across it" % red)

# M211: QEMU has no I2C touchpad, so this machine labels its PS/2 pointer a
# trackpad from outside the image, and the trackpad's own settings - not the
# mouse's - are what the pointer then answers to.
TRACKPAD_MACHINE = dict(extra_args=["-fw_cfg", "name=opt/leanos/pointer,string=trackpad"])
TRACKPAD_SLOW_STEP = 16

def cursor_after(m, steps, near):
    m.home()
    for dx, dy in steps:
        m.monitor("mouse_move %d %d" % (dx, dy), settle=0.03)
    time.sleep(0.3)
    m.cursor = None
    return m.find_cursor(m.screenshot(), near, radius=200)

def test_trackpad_settings_move_the_pointer(m):
    m.step_limit = TRACKPAD_SLOW_STEP
    boot(m)
    check("[mouse] opt/leanos/pointer=trackpad" in m.read_log(),
          "the harness switch did not reach the PS/2 driver")

    slow = [(0, 16)] * 15
    fast = cursor_after(m, slow + [(100, 0)], (250, 240))
    check(fast is not None and fast[0] > 140,
          "one fast step of 100 counts put the cursor at %r - the trackpad's default acceleration did "
          "not take it further than 100 pixels" % (fast,))

    m.double_click(ICON_X, ICONS[3][2])
    wait_for_windows(m, 1)
    origin = app_origin(FIRST_APP_IDX, SETTINGS_H)
    settings_pane(m, origin, "trackpad")
    mark = len(m.read_log())
    m.click(*widget_point(m, origin, "trackpad_acceleration", 0.0, 0.5))
    wait_for_log_after(m, "[settings] trackpad acceleration 0", mark,
                       "the Acceleration slider's left end did not turn acceleration off")
    exact = cursor_after(m, slow + [(100, 0)], (100, 240))
    check(exact == (100, 240),
          "with acceleration off, 100 counts and 240 moved the cursor to %r" % (exact,))

    m.cursor = None
    mark = len(m.read_log())
    m.click(*widget_point(m, origin, "trackpad_speed", 1.0, 0.5))
    wait_for_log_after(m, "[settings] trackpad speed 300", mark,
                       "the Tracking speed slider's right end did not set 300%")
    tripled = cursor_after(m, [(0, 16)] * 5 + [(10, 0)] * 5, (150, 240))
    check(tripled == (150, 240),
          "at 300%%, 50 counts across and 80 down moved the cursor to %r, not (150, 240)" % (tripled,))

def test_a_tap_reaches_every_toolkit_control(m):
    # M212: a tap's press and release arrive together, and a toolkit that
    # samples the pointer once a frame saw neither. A pane row and a switch
    # are different LVGL widgets, so both are tapped.
    boot(m)
    m.double_click(ICON_X, ICONS[3][2])
    wait_for_windows(m, 1)
    origin = app_origin(FIRST_APP_IDX, SETTINGS_H)
    geometry(m, "pane_trackpad")
    for pane, name in (("trackpad", "Trackpad"), ("display", "Display"), ("date_time", "Date & Time")):
        mark = len(m.read_log())
        m.tap(*widget_center(m, origin, "pane_" + pane))
        wait_for_log_after(m, "[settings] showing %s" % name, mark,
                           "a tap on the %s pane's row did not open it" % name)

    settings_pane(m, origin, "trackpad")
    mark = len(m.read_log())
    m.tap(*widget_center(m, origin, "trackpad_natural"))
    wait_for_log_after(m, "[settings] trackpad natural scrolling 0", mark,
                       "a tap on the Natural scrolling switch did not turn it off")
    mark = len(m.read_log())
    m.tap(*widget_center(m, origin, "trackpad_natural"))
    wait_for_log_after(m, "[settings] trackpad natural scrolling 1", mark,
                       "a second tap on the switch did not turn it back on")

GOP_IMAGE = os.path.join(qemu_input.REPO_ROOT, "build", "os-image-gop.bin")
GOP_PANEL = (2560, 1600)

def build_gop_image():
    # M213: the laptop's case - a mode the boot loader chose from the
    # firmware's list, nothing to change it with afterwards, and a
    # lean_os.cfg that names its own sectors - made from the image under test.
    base = qemu_input.IMAGE
    if os.path.exists(GOP_IMAGE) and os.path.getmtime(GOP_IMAGE) >= os.path.getmtime(base):
        return
    subprocess.check_call(["tools/make-hardware-image.sh", "--no-wireless-firmware", "--log-mib", "0",
                           "--video", "%dx%d" % GOP_PANEL, "--output", GOP_IMAGE],
                          cwd=qemu_input.REPO_ROOT, stdout=subprocess.DEVNULL)

GOP_MACHINE = dict(extra_args=["-fw_cfg", "name=opt/leanos/display,string=firmware"], image=GOP_IMAGE)

def desktop_line(width, height, percent, panel=GOP_PANEL):
    return "[compositor] the desktop is %dx%d on a %dx%d panel, at %d%%" % (width, height, panel[0], panel[1],
                                                                            percent)

def gop_click(m, x, y):
    # A cursor drawn at twice its size is not the shape the harness looks
    # for, so the pointer is placed by counting rather than by looking.
    m.home()
    m._step_by(x, y)
    time.sleep(0.2)
    m.cursor = (x, y)
    m.click()

def gop_widget(m, origin, name, mark):
    deadline = time.time() + 25
    needle = "[geometry] %s " % name
    while time.time() < deadline:
        log = m.read_log()[mark:]
        at = log.rfind(needle)
        if at >= 0:
            x, y, w, h = (int(v) for v in log[at + len(needle):].split("\n", 1)[0].split()[:4])
            return (origin[0] + x + w // 2, origin[1] + y + h // 2)
        time.sleep(0.3)
    raise Failure("Settings never reported where it put %r (log: %s)" % (name, m.save_log("no-geometry")))

def start_glyph_is_at(shot, x, y):
    r, g, b = (shot.px(x, y) >> 16) & 0xFF, (shot.px(x, y) >> 8) & 0xFF, shot.px(x, y) & 0xFF
    return b > r + 60 and b > 0x80

def test_display_scale_and_startup_mode_on_a_fixed_mode_screen(m):
    m.wait_for_marker(desktop_line(1280, 800, 200), timeout=300)
    time.sleep(4)
    # The Start button's ring, in desktop pixels: on a doubled desktop it is
    # drawn twice the size at twice the distance from the corner.
    glyph = (START_PROBE[0], 800 - (SCREEN_H - START_PROBE[1]))
    wait_for(m, lambda s: start_glyph_is_at(s, 2 * glyph[0] + 1, 2 * glyph[1] + 1),
             "the taskbar's Start button is not where a 200% desktop puts it")

    mark = len(m.read_log())
    m.sendkey("ctrl-spc")
    time.sleep(0.5)
    m.type_text("settings")
    m.sendkey("ret")
    origin = app_origin(FIRST_APP_IDX)
    gop_click(m, *gop_widget(m, origin, "pane_display", mark))
    wait_for_log_after(m, "[settings] showing Display", mark, "Settings did not open its Display pane")
    shown = mark

    mark = len(m.read_log())
    gop_click(m, *gop_widget(m, origin, "scale125", shown))
    wait_for_log_after(m, desktop_line(2048, 1280, 125), mark, "choosing 125% did not rescale the desktop")
    wait_for_log_after(m, desktop_line(1280, 800, 200), mark, "a scale nobody kept did not go back by itself",
                       timeout=30.0)

    mark = len(m.read_log())
    gop_click(m, *gop_widget(m, origin, "scale100", shown))
    wait_for_log_after(m, desktop_line(2560, 1600, 100), mark, "choosing 100% did not rescale the desktop")
    wait_for(m, lambda s: start_glyph_is_at(s, glyph[0], 1600 - (800 - glyph[1])),
             "at 100% the taskbar was not laid out again along the bottom of the full panel")
    keep = gop_widget(m, origin, "keep", mark)
    gop_click(m, *keep)
    wait_for_log_after(m, "[settings] kept scale 100", mark, "Keep did not keep the scale")
    time.sleep(12)
    check(desktop_line(1280, 800, 200) not in m.read_log()[mark:],
          "a scale that was kept went back anyway")

    mark = len(m.read_log())
    gop_click(m, *gop_widget(m, origin, "startup1920x1200", shown))
    wait_for_log_after(m, "[display] the next boot starts in video=1920x1200", mark,
                       "choosing 1920x1200 did not rewrite lean_os.cfg")
    gop_click(m, *gop_widget(m, origin, "restart", mark))
    time.sleep(1.0)
    boots = m.read_log().count(BOOT_MARKER)
    m.sendkey("y")
    deadline = time.time() + 300
    while time.time() < deadline and m.read_log().count(BOOT_MARKER) <= boots:
        time.sleep(1.0)
    check(m.read_log().count(BOOT_MARKER) > boots, "Restart Now and a yes did not restart the machine")
    after = m.read_log().rfind(BOOT_MARKER)
    wait_for_log_after(m, "[inventory] screen: 1920x1200", m.read_log().rfind("[fwcfg]"),
                       "the boot after the restart did not start in the mode Settings chose")
    wait_for_log_after(m, desktop_line(1920, 1200, 100, panel=(1920, 1200)), after,
                       "the kept scale did not survive the restart", timeout=120.0)
    shot = m.screenshot()
    check((shot.width, shot.height) == (1920, 1200),
          "the screen after the restart is %dx%d, not 1920x1200" % (shot.width, shot.height))

TESTS = [
    ("a_tap_reaches_every_toolkit_control", test_a_tap_reaches_every_toolkit_control),
    ("display_scale_and_startup_mode_on_a_fixed_mode_screen",
     test_display_scale_and_startup_mode_on_a_fixed_mode_screen),
    ("double_click_launches_every_icon", test_double_click_launches_every_icon),
    ("single_click_does_not_launch", test_single_click_does_not_launch),
    ("titlebar_close_button", test_titlebar_close_button),
    ("titlebar_drag_moves_window", test_titlebar_drag_moves_window),
    ("alt_tab_cycles_focus", test_alt_tab_cycles_focus),
    ("desktop_context_menu", test_desktop_context_menu),
    ("a_painting_becomes_the_desktop_picture", test_a_painting_becomes_the_desktop_picture),
    ("trackpad_settings_move_the_pointer", test_trackpad_settings_move_the_pointer),
    ("start_button_opens_launcher", test_start_button_opens_launcher),
    ("taskbar_click_keeps_app_focused", test_taskbar_click_keeps_app_focused),
    ("taskbar_button_focus_and_minimize", test_taskbar_button_focus_and_minimize),
    ("editor_in_window_file_menu", test_editor_in_window_file_menu),
    ("closing_the_readme_raises_no_toast", test_closing_the_readme_raises_no_toast),
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
    ("wheel_scrolls_the_file_list", test_wheel_scrolls_the_file_list),
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
    ("file_manager_puts_back_and_undoes", test_file_manager_puts_back_and_undoes),
    ("file_manager_searches_and_quick_looks", test_file_manager_searches_and_quick_looks),
    ("twelve_hour_clock_reaches_the_taskbar", test_twelve_hour_clock_reaches_the_taskbar),
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
    ("browser_closes_from_its_titlebar_button",
     test_browser_closes_from_its_titlebar_button),
    ("browser_new_tab_button_opens_a_tab",
     test_browser_new_tab_button_opens_a_tab),
    ("popup_window_opens_captures_and_dismisses",
     test_popup_window_opens_captures_and_dismisses),
    ("window_animations_stay_smooth", test_window_animations_stay_smooth),
    ("wifi_wizard_joins_after_a_wrong_password", test_wifi_wizard_joins_after_a_wrong_password),
    ("wifi_remembers_a_network_across_a_reboot", test_wifi_remembers_a_network_across_a_reboot),
    ("wifi_open_network_needs_no_password_and_wpa3_is_refused",
     test_wifi_open_network_needs_no_password_and_wpa3_is_refused),
]

# Tests that need a machine other than the suite's usual one - so a cold
# boot, since the snapshot was taken of the usual one.
MACHINE_OPTIONS = {
    "display_scale_and_startup_mode_on_a_fixed_mode_screen": GOP_MACHINE,
    "trackpad_settings_move_the_pointer": TRACKPAD_MACHINE,
    "wifi_wizard_joins_after_a_wrong_password": WIFI_MACHINE,
    "wifi_remembers_a_network_across_a_reboot": WIFI_MACHINE,
    "wifi_open_network_needs_no_password_and_wpa3_is_refused": WIFI_MACHINE,
}

def _die_on_signal(signum, _frame):
    raise KeyboardInterrupt("received signal %d" % signum)

QUICK_TESTS = [
    "a_tap_reaches_every_toolkit_control",
    "desktop_context_menu",
    "a_painting_becomes_the_desktop_picture",
    "double_click_launches_every_icon",
    "titlebar_close_button",
    "closing_the_readme_raises_no_toast",
    "clicking_a_window_raises_it",
    "editor_undo_restores_the_buffer",
    "terminal_scrollback_scrolls_with_the_wheel",
    "file_manager_navigates_directories",
    "file_manager_creates_a_folder_and_deletes_it_full",
    "file_manager_puts_back_and_undoes",
    "settings_persist_across_a_reboot",
    "a_crashing_program_only_takes_itself_down",
    "launching_an_app_does_not_disturb_the_rest_of_the_screen",
    "browser_loads_a_page_from_another_machine",
    "browser_survives_an_empty_flex_container",
    "browser_closes_from_its_titlebar_button",
    "browser_new_tab_button_opens_a_tab",
    "popup_window_opens_captures_and_dismisses",
    "window_animations_stay_smooth",
    "wifi_wizard_joins_after_a_wrong_password",
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
    "display_scale_and_startup_mode_on_a_fixed_mode_screen",
    "trackpad_settings_move_the_pointer",
    "wifi_wizard_joins_after_a_wrong_password",
    "wifi_remembers_a_network_across_a_reboot",
    "wifi_open_network_needs_no_password_and_wpa3_is_refused",
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
        if MACHINE_OPTIONS.get(name, {}).get("image") == GOP_IMAGE:
            build_gop_image()
        with Machine(boot_timeout=boot_timeout, snapshot=use, **MACHINE_OPTIONS.get(name, {})) as m:
            try:
                fn(m)
            except Failure as exc:
                # A check() that fails names no screendump; a wait_for that
                # fails already saved one. Either way the machine is about to
                # be torn down with its log, and a failure with no log is a
                # failure that has to be reproduced before it can be read.
                if "screendump saved" not in str(exc):
                    where = save_failure_shot(m, current_test())
                    raise Failure("%s (screendump and guest log saved to %s)" % (exc, where))
                raise
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
