#!/usr/bin/env python3
"""tools/qemu_input_suite.py - the M40 interactive-input regression suite.

Run it through tools/qemu-input-test.sh. Everything here drives a real
booted guest with real synthetic mouse/keyboard input (tools/qemu_input.py)
and grades the result from real framebuffer pixels - no guest-side hooks,
no protocol shortcuts, nothing calling apply_window_action() directly.

That distinction is the whole point of this file. Every click/drag feature
since M18 already has a protocol-level self-test in kernel/kernel.c, and
all of those passed the entire time the desktop was visibly broken (the
third app you launched from an icon silently never opened - see
milestones.md's M40 section). A test that pokes WM_ACTION_PIPE proves the
action works; only this proves the *path to it* does.

Each test gets its own freshly-booted guest. Slower, but a test that
inherits eight already-open windows from the previous one isn't testing
what it says it is - and window/fd/shm exhaustion is precisely the class
of bug this suite was written to catch.
"""

import os
import shutil
import signal
import sys
import threading
import time
import traceback
from concurrent.futures import ThreadPoolExecutor

from qemu_input import BOOT_MARKER, Machine

# Where a failing test drops the screenshot it gave up on. A pixel
# assertion that fails without showing you the pixels is a bad trade when
# saving them costs one file copy.
ARTIFACT_DIR = os.environ.get("LEANOS_INPUT_ARTIFACTS", "/tmp/leanos-input-failures")

# Which test this thread is running. Thread-local rather than a plain
# global because tests run in parallel now (see main's --jobs): a global
# would name whichever test happened to start most recently, which is
# exactly the wrong answer on the one path that uses it - saving the
# screenshot a *failing* test gave up on.
_current = threading.local()


def current_test():
    return getattr(_current, "name", "unknown")

# ---------------------------------------------------------------------
# Geometry and colors, all mirrored from the source that draws them. Kept
# as named constants rather than inline magic numbers so a layout change
# in a .c file shows up here as a one-line diff to reconcile, the same
# convention tools/qemu-serial-test.sh's REQUIRED_MARKERS uses for klog
# strings.
# ---------------------------------------------------------------------

# desktop_icons.c: ICON_MARGIN 32, ICON_CELL_H 90, ICON_SIZE 48,
# ICON_CELL_W 90. At 1024x768 layout_icons wraps at max_y 660, so the
# first seven fill one column (boxes at y 32..572) and M74's README icon
# is the first thing on this desktop to start a second one, at x 122.
# Coordinates below are icon-box centers, so each entry carries its own
# x now rather than sharing one.
# ICON_X is the first column and is what every test that names one icon
# uses; the fourth field is the per-icon column, which only M74's README
# differs on. Entry [2] is still the y centre everywhere, deliberately -
# renumbering it would have touched twenty call sites to say nothing new.
ICON_X = 56
ICONS = [
    ("Terminal", "gui_terminal", 56, ICON_X),
    ("Editor", "text_editor", 146, ICON_X),
    ("Files", "file_manager", 236, ICON_X),
    ("Settings", "settings", 326, ICON_X),
    ("Clock", "gui_clock", 416, ICON_X),
    ("Paint", "gui_paint", 506, ICON_X),
    ("Tasks", "task_manager", 596, ICON_X),
    # M74: opens text_editor on /home/readme.txt, so double-clicking it
    # produces an editor window like the Editor icon does - which is why
    # the window-count tests below still add up.
    ("README", "text_editor", 56, ICON_X + 90),
]

ICON_BOX = 0x4C99E6          # desktop_icons.c ICON_BOX_COLOR
CTX_MENU_BG = 0x243040       # desktop_icons.c CTX_MENU_BG
EMPTY_DESKTOP = (500, 500)   # far from every icon and every cascaded window

# M44: the desktop is a wallpaper gradient rather than a flat color, so
# "is this bare desktop" is now a question about the row as well as the
# pixel. user_space/lib/wallpaper.c's WALLPAPER_GRADIENT over the
# compositor's DEFAULT_BG_COLOR, which is what a freshly booted desktop
# shows - mirrored here the same way every other geometry constant in this
# file is, and computed rather than tabulated so a change to the ramp
# shows up as a mismatch naming both numbers.
SCREEN_H = 768
WALLPAPER_BASE = 0x1A1A2E    # compositor.c DEFAULT_BG_COLOR
WALLPAPER_TOP_PCT = 155      # wallpaper.c STYLES[WALLPAPER_GRADIENT]
WALLPAPER_BOTTOM_PCT = 60


def _scale(color, pct):
    out = 0
    for shift in (16, 8, 0):
        out |= min(255, ((color >> shift) & 0xFF) * pct // 100) << shift
    return out


def _trunc_div(a, b):
    """C's integer division, which truncates toward zero - Python's // does
    not, and the gradient ramps downward so the operands really are
    negative."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def desktop_px(y, base=WALLPAPER_BASE):
    """The wallpaper's color at row y - what "bare desktop" means now."""
    top = _scale(base, WALLPAPER_TOP_PCT)
    bottom = _scale(base, WALLPAPER_BOTTOM_PCT)
    out = 0
    for shift in (16, 8, 0):
        a = (top >> shift) & 0xFF
        b = (bottom >> shift) & 0xFF
        out |= (a + _trunc_div((b - a) * y, SCREEN_H - 1)) << shift
    return out


DESKTOP_BG = desktop_px(EMPTY_DESKTOP[1])

# M44: the taskbar and the launcher overlay are both blended over whatever
# is composited underneath them, which in every test here is the wallpaper.
TRANSLUCENT_NUM, TRANSLUCENT_DEN = 3, 4          # compositor.c, for a translucent window
LAUNCHER_OPACITY_NUM, LAUNCHER_OPACITY_DEN = 4, 5


def blend(under, over, num, den):
    """compositor.c's fill_rect_blend, in Python - integer, same rounding.
    Recomputed here rather than tabulated as hex literals so a change to a
    ratio shows up as a mismatch naming both numbers, not as an
    unexplained wrong color."""
    out = 0
    for shift in (16, 8, 0):
        u = (under >> shift) & 0xFF
        o = (over >> shift) & 0xFF
        out |= ((u * (den - num) + o * num) // den) << shift
    return out


def panel_px(raw, y):
    """A taskbar color as it actually reaches the screen: M44 made the bar
    translucent, so what desktop_shell.c painted is mixed with the
    wallpaper row underneath it."""
    return blend(desktop_px(y), raw, TRANSLUCENT_NUM, TRANSLUCENT_DEN)

# text_editor.c: its File menu is drawn in the editor's own window again
# (M42 - M41 had it in a shared top bar), one FONT_HEIGHT row at the top,
# with the dropdown hanging below the "File" label inside the same window.
EDITOR_MENU_ROW_H = 16       # UI_FONT_UI_HEIGHT
EDITOR_MENU_BG = 0x242424    # MENU_BG - both the menu row and the dropdown
EDITOR_MENU_ITEM_W = 110     # FILE_MENU_ITEM_W
EDITOR_MENU_ITEM_H = 20      # FILE_MENU_ITEM_H = FONT_HEIGHT + 4
EDITOR_MENU_X = 4            # FILE_MENU_X

# desktop_shell.c (M42): PANEL_HEIGHT 32 at the screen bottom, holding a
# Start button, then the running-app buttons, then the tray - every one of
# them BTN_H 24 tall starting BTN_Y 4 down from the panel's top edge.
# Reading the taskbar rather than hunting for window pixels on the desktop
# is deliberate: the panel is always topmost (compositor.c's z-order
# rule), so a button can never be covered by the very windows it is
# reporting - which a probe placed on the desktop absolutely can be, by
# the next window in the cascade.
FONT_H = 16
PANEL_TOP = SCREEN_H - 32
BTN_Y = 4
BTN_H = 24
SLOT_W = 96
SLOT_GAP = 4
START_X = 4
START_W = 72
SLOTS_X = START_X + START_W + 8
TRAY_W = 112  # M63: TRAY_ICONS_W(67, four workspace dots) + a *measured* "00:00"(35) + TRAY_PAD(10)

# Every button in the bar is read at one of two rows, so the blend is
# resolved once here rather than at each call site.
PANEL_PROBE_Y = PANEL_TOP + 2       # the margin strip above the button row
BTN_PROBE_Y = PANEL_TOP + BTN_Y + 12  # the middle of any button

PANEL_BG = panel_px(0x181828, PANEL_PROBE_Y)
SLOT_BG = panel_px(0x263447, BTN_PROBE_Y)        # RUNNING_SLOT_BG - running, unfocused
SLOT_HOVER_BG = panel_px(0x365070, BTN_PROBE_Y)  # RUNNING_SLOT_HOVER_BG - cursor over it
SLOT_FOCUS_BG = panel_px(0x2E4A63, BTN_PROBE_Y)  # RUNNING_SLOT_FOCUS_BG
SLOT_MIN_BG = panel_px(0x352A20, BTN_PROBE_Y)    # RUNNING_SLOT_MIN_BG - minimized
SLOT_COLORS = (SLOT_BG, SLOT_HOVER_BG, SLOT_FOCUS_BG, SLOT_MIN_BG)

START_PROBE = (71, PANEL_TOP + 6)   # inside the Start button's fill, past its glyph and label
START_CLICK = (40, PANEL_TOP + BTN_Y + BTN_H // 2)
START_BG = panel_px(0x243447, START_PROBE[1])        # at rest
START_HOVER_BG = panel_px(0x365070, START_PROBE[1])
START_PRESS_BG = panel_px(0x4C99E6, START_PROBE[1])  # the click flash
START_COLORS = (START_BG, START_HOVER_BG, START_PRESS_BG)

TRAY_SEP_PROBE = (1024 - TRAY_W, PANEL_TOP + 14)
TRAY_SEP = panel_px(0x303C4E, TRAY_SEP_PROBE[1])     # TRAY_SEP_COLOR

# compositor.c's launcher overlay (M42's surface, M43's contents): 480x320,
# horizontally centered and a third of the way down, with a search field
# LAUNCHER_PAD in from the top and the result rows LAUNCHER_LIST_Y below
# that, LAUNCHER_ROW_H apart.
LAUNCHER_BG_RAW = 0x1C2233   # LAUNCHER_BG, before M44's translucency
LAUNCHER_SEL_BG = 0x335577   # LAUNCHER_SEL_BG - the selected result's fill
LAUNCHER_W, LAUNCHER_H = 480, 320
LAUNCHER_X = (1024 - LAUNCHER_W) // 2
LAUNCHER_Y = (768 - LAUNCHER_H) // 3
LAUNCHER_LIST_Y = 46         # LAUNCHER_PAD(12) + LAUNCHER_INPUT_H(24) + 10
LAUNCHER_ROW_H = 20
LAUNCHER_PAD = 12            # GFX_PAD

# M47: the Power controls along the bottom of the overlay, and the confirm
# box a click on either one raises. Mirrored from compositor.c's POWER_*.
POWER_BTN_W, POWER_BTN_H, POWER_BTN_GAP = 96, 22, 8
POWER_BTN_Y = LAUNCHER_H - LAUNCHER_PAD - POWER_BTN_H
POWER_OFF_X = LAUNCHER_W - LAUNCHER_PAD - 2 * POWER_BTN_W - POWER_BTN_GAP
POWER_REBOOT_X = LAUNCHER_W - LAUNCHER_PAD - POWER_BTN_W
POWER_CONFIRM_W, POWER_CONFIRM_H = 300, 96
POWER_CONFIRM_BG = 0x202838


def power_button_center(which):
    """Screen center of the Shut Down (0) or Restart (1) button."""
    bx = POWER_OFF_X if which == 0 else POWER_REBOOT_X
    return (LAUNCHER_X + bx + POWER_BTN_W // 2, LAUNCHER_Y + POWER_BTN_Y + POWER_BTN_H // 2)


# M47: "is the launcher up" read somewhere the pointer's own travel can't
# disturb. LAUNCHER_PROBE sits on a result row, and launcher_hover selects
# whatever row the pointer passes over - so a click aimed at the Power
# buttons, which the harness reaches by sweeping the pointer from (0, 0)
# straight across the list, leaves a row highlighted underneath it. This
# probe is in the overlay's bottom strip, left of both Power buttons and
# below every row.
LAUNCHER_LOWER_PROBE = (LAUNCHER_X + 20, LAUNCHER_Y + POWER_BTN_Y + POWER_BTN_H // 2)
LAUNCHER_LOWER_BG = blend(desktop_px(LAUNCHER_LOWER_PROBE[1]), LAUNCHER_BG_RAW,
                          LAUNCHER_OPACITY_NUM, LAUNCHER_OPACITY_DEN)


def power_confirm_probe():
    """Inside the confirm box, below its text and right of it - so this
    reads the box's own fill rather than a glyph. The box is drawn opaque
    over the (translucent) launcher, so no blend is involved."""
    return (LAUNCHER_X + (LAUNCHER_W - POWER_CONFIRM_W) // 2 + POWER_CONFIRM_W - 12,
            LAUNCHER_Y + (LAUNCHER_H - POWER_CONFIRM_H) // 2 + POWER_CONFIRM_H - 8)
# Inside the overlay, on row 5 - which is never the selected one in these
# tests, so it reads flat overlay background - and far right of any
# filename text.
LAUNCHER_PROBE = (LAUNCHER_X + 428, LAUNCHER_Y + LAUNCHER_LIST_Y + 5 * LAUNCHER_ROW_H + 10)
LAUNCHER_BG = blend(desktop_px(LAUNCHER_PROBE[1]), LAUNCHER_BG_RAW,
                    LAUNCHER_OPACITY_NUM, LAUNCHER_OPACITY_DEN)

# compositor.c's snap preview: SNAP_PREVIEW_NUM/DEN of the accent color
# blended over whatever is already composited underneath.
ACCENT = 0x4C99E6            # TITLEBAR_FOCUS_COLOR, and the preview's color
SNAP_PREVIEW_NUM, SNAP_PREVIEW_DEN = 1, 4

def launcher_row_probe(i):
    """Inside result row i's selection fill, right of every filename this
    filesystem has."""
    return (LAUNCHER_X + 428, LAUNCHER_Y + LAUNCHER_LIST_Y + i * LAUNCHER_ROW_H + 10)

def slot_probe(i):
    """Where to *read* running-app button i's fill - inside it, clear of
    its 1px border and of the label text drawn near its left edge."""
    return (SLOTS_X + i * (SLOT_W + SLOT_GAP) + SLOT_W - 8,
            PANEL_TOP + BTN_Y + 12)


def slot_click(i):
    """Where to *click* running-app button i. Deliberately not
    slot_probe(i): the compositor draws the 8x8 pointer with its hotspot
    at the cursor position, so a click lands the pointer's own white
    pixels on top of exactly the point that was clicked - and every
    reading here works by sampling that button's fill color. Clicking one
    place and reading another, 16px apart in x, is what keeps "is this
    button focused" a question about the button rather than about the
    pointer parked on it."""
    return (SLOTS_X + i * (SLOT_W + SLOT_GAP) + SLOT_W - 24, PANEL_TOP + BTN_Y)


# compositor.c titlebar geometry: TITLEBAR_H 20, BTN_SIZE 14, BTN_GAP 4,
# BTN_MARGIN 4, and the three buttons are laid out *right to left* in
# titlebar_button_t order - minimize outermost, then maximize, with close
# innermost. Getting that backwards silently minimizes a window instead of
# closing it, which looks close enough to passing to be worth naming here.
TITLEBAR_H = 20
BTN_SIZE = 14
BTN_GAP = 4
BTN_MARGIN = 4
BTN_MINIMIZE, BTN_MAXIMIZE, BTN_CLOSE = 0, 1, 2

# M46: the buttons are circles now, in macOS's colors but still in
# Windows' positions - the rects above are unchanged, only what fills them
# is. BTN_*_COLOR mirrors compositor.c; BTN_*_HOVER is that color halfway
# to white (its `lighten`, BTN_HOVER_LIGHTEN = 2), recomputed here rather
# than tabulated so a change to the ratio shows up naming both numbers.
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
    """A point inside button `button`'s disc and clear of both diagonals
    of the close button's x - the middle column of the circle is exactly
    where those two strokes cross, so reading the fill means staying off
    it. Four pixels left of center, on the button's vertical midline."""
    cx, cy = titlebar_button_center(win_x, win_y, win_w, button)
    return (cx - 4, cy)


def titlebar_button_center(win_x, win_y, win_w, button):
    x = win_x + win_w - BTN_MARGIN - BTN_SIZE - button * (BTN_SIZE + BTN_GAP)
    y = win_y - TITLEBAR_H + (TITLEBAR_H - BTN_SIZE) // 2
    return (x + BTN_SIZE // 2, y + BTN_SIZE // 2)


CLOCK_W = 200  # gui_clock.c WIN_W

# M51: compositor.c's window frame - BORDER 2, filled with BORDER_COLOR
# and then blitted over, so exactly two columns of it survive down each
# side of a window. Reading one of those columns is how the z-order tests
# below ask "which of these two overlapping windows is in front": the
# answer is a compositor constant either way round, rather than whatever
# an app happens to draw at that pixel.
BORDER = 2
BORDER_COLOR = 0x444466

# settings.c's window and the two rows of it this suite clicks.
SETTINGS_W, SETTINGS_H = 320, 680
WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H = 258, 68, 22

# M61: settings.c's Motion switch - one button whose fill says which way
# it is set (BTN_HOVER when animations are on, BTN_COLOR when off).
MOTION_BTN_Y = 288           # MOTION_LABEL_Y(290) - 2
# M62: the volume row shares it - five steps, the leftmost of which is
# mute. Same two flat colours say which one is set.
VOL_Y, VOL_BTN_W, VOL_BTN_H, VOL_BTN_GAP = 288, 22, 20, 4
VOL_X0 = 12 + 58             # GFX_PAD + the label's width allowance
MOTION_BTN_W, MOTION_BTN_H = 50, 20
MOTION_BTN_X = 320 - 12 - MOTION_BTN_W   # WIN_W - GFX_PAD - MOTION_BTN_W
SETTINGS_BTN_ON = 0x607088   # settings.c BTN_HOVER
SETTINGS_BTN_OFF = 0x445566  # settings.c BTN_COLOR


# The cursor is CURSOR_W x CURSOR_H and is drawn from its position
# rightwards and downwards, so it covers whatever was clicked. Reading a
# button's fill therefore has to happen somewhere the cursor cannot be:
# these click on the *right* of a control and probe on its *left*, which
# is the only arrangement that works without moving the mouse away first
# (and moving it away is its own event the window would have to process).
def volume_click(sx, sy, step):
    return (sx + VOL_X0 + step * (VOL_BTN_W + VOL_BTN_GAP) + VOL_BTN_W - 4,
            sy + VOL_Y + VOL_BTN_H - 4)


def volume_probe(sx, sy, step):
    """Inside volume step `step`'s fill, left of anywhere it is clicked.
    Step 0 is mute."""
    return (sx + VOL_X0 + step * (VOL_BTN_W + VOL_BTN_GAP) + 3,
            sy + VOL_Y + 3)


def motion_click(sx, sy):
    return (sx + MOTION_BTN_X + MOTION_BTN_W - 6, sy + MOTION_BTN_Y + MOTION_BTN_H - 4)


def motion_probe(sx, sy):
    """Inside the Motion button's fill, left of both its label and the
    point it is clicked at, so what is read is the button's state rather
    than a glyph or the cursor sitting on it."""
    return (sx + MOTION_BTN_X + 6, sy + MOTION_BTN_Y + 4)

# M58: settings.c's Resolution pane. The offered modes come from
# kernel/drivers/dispi.c's CANDIDATES list, filtered by what the adapter
# reports - on QEMU's stdvga with its default 16 MiB every one of them
# survives, so index 0 is 800x600.
MODE_BTN_Y, MODE_BTN_W, MODE_BTN_H, MODE_BTN_GAP, MODE_COLS = 340, 92, 20, 6, 3
GFX_PAD = 12
CONFIRM_Y = MODE_BTN_Y + 3 * (MODE_BTN_H + 4) + 6
CONFIRM_H = 20
KEEP_BTN_W = 64
KEEP_BTN_X = SETTINGS_W - GFX_PAD - KEEP_BTN_W
SMALL_MODE = (800, 600)
SMALL_MODE_INDEX = 0
# system_api/include/wm.h's WM_MODE_REVERT_MS, plus room for the
# compositor to notice the deadline and for the panel to re-handshake.
MODE_REVERT_S = 10 + 5


def mode_btn_center(sx, sy, i):
    return (sx + GFX_PAD + (i % MODE_COLS) * (MODE_BTN_W + MODE_BTN_GAP) + MODE_BTN_W // 2,
            sy + MODE_BTN_Y + (i // MODE_COLS) * (MODE_BTN_H + 4) + MODE_BTN_H // 2)

# file_manager.c: WIN_W/WIN_H, its two header strips, and one row.
# M59: 280 -> 340 for the size and date columns, and a second header
# strip (the clickable column headings) between the path bar and the
# list - so a row's y is measured from FM_LIST_Y, not from the path bar.
FM_W, FM_H = 340, 360
FM_HEADER_H = 24
FM_COLS_H = 16
FM_LIST_Y = FM_HEADER_H + FM_COLS_H
FM_ROW_H = 16   # M57: LIST_FONT_H (ui_font_small, 12) + 4 - the list is dense-list text now
FM_LIST_W = FM_W - 8  # minus SCROLLBAR_W

# M53: the window opens on /home and row 0 is always "..", so the first
# real file is row 1. Named rather than written as 1 at three call sites,
# because "which row is the first file" is exactly the kind of off-by-one
# a layout change makes wrong silently.
FM_LABEL_COLOR = 0x90A0C0  # file_manager.c LABEL_COLOR - the path bar's text
FM_TEXT_COLOR = 0xD8D8D8   # file_manager.c TEXT_COLOR - a listed name
FM_STATUS_H = 20           # STATUS_H: UI_FONT_UI_HEIGHT + 4
FM_ROWS_VISIBLE = (FM_H - FM_LIST_Y - FM_STATUS_H) // FM_ROW_H

# M53: every directory but the root gets a ".." row at index 0, so the
# first real entry is row 1 - *except* in "/", which has nowhere to go up
# to and therefore lists its own contents from row 0. That asymmetry is
# deliberate (an inert ".." in the root would be a row that does nothing)
# and is exactly the kind of off-by-one worth naming rather than writing
# as a literal at each call site.
FM_UP_ROW = 0
FM_FIRST_FILE_ROW = 1
FM_ROOT_FIRST_ROW = 0


def fm_row_point(x, y, row):
    """A point inside file-manager row `row`, left of the scrollbar and
    right of nothing - the label starts at x+6, so this is on the row's
    fill or its text, either of which is what a click wants."""
    return (x + 60, y + FM_LIST_Y + row * FM_ROW_H + FM_ROW_H // 2)


def fm_rows_with_text(shot, x, y):
    """How many list rows have anything drawn in them. Counting rows
    rather than reading names is what keeps these tests independent of
    what happens to be on disk, while still telling a directory holding
    two things apart from one holding two dozen."""
    n = 0
    for row in range(FM_ROWS_VISIBLE):
        top = y + FM_LIST_Y + row * FM_ROW_H
        if shot.count_color(FM_TEXT_COLOR, x + 6, top, FM_LIST_W - 12, FM_ROW_H) > 0:
            n += 1
    return n

# M49: compositor.c's drag label, which follows the cursor during a
# client-initiated drag.
DRAG_LABEL_BG = 0x335577

# M48: compositor.c's toast surface - 300x56, TOAST_MARGIN in from the
# top-right, stacked downward. Opaque, drawn over everything but the
# cursor, so no blend is involved in any of these reads.
TOAST_W, TOAST_H, TOAST_GAP, TOAST_MARGIN = 300, 56, 8, 12
TOAST_STRIPE_W = 4
TOAST_BG = 0x222A38
TOAST_ERROR_C = 0xE05C55


def toast_rect(i):
    return (1024 - TOAST_W - TOAST_MARGIN, TOAST_MARGIN + i * (TOAST_H + TOAST_GAP))


def toast_stripe_probe(i):
    """On toast i's accent stripe - inset one pixel from its left edge,
    vertically centered."""
    x, y = toast_rect(i)
    return (x + 1 + TOAST_STRIPE_W // 2, y + TOAST_H // 2)


def toast_click_point(i):
    """Well inside toast i and clear of its stripe and its text, so the
    click lands on the toast rather than near it."""
    x, y = toast_rect(i)
    return (x + TOAST_W - 20, y + TOAST_H // 2)
TASKS_W, TASKS_H = 420, 360  # task_manager.c WIN_W/WIN_H
LIST_Y_IN_WIN = 40           # HEADER_H(22) + COLS_H(18)
LIST_H_IN_WIN = TASKS_H - LIST_Y_IN_WIN - 34  # ...minus FOOTER_H
SCROLLBAR_THUMB = 0x506080   # task_manager.c SCROLLBAR_THUMB
# M57: the process list draws in the 12-row face now, so a row is
# LIST_FONT_H(12) + 4 rather than FONT_HEIGHT(16) + 4.
TM_ROW_H = 16
TM_SELECT = 0x4C6699         # task_manager.c SELECT_COLOR
TM_BG = 0x1C1C24             # task_manager.c BG_COLOR


def tm_row_states(shot, tx, ty):
    """(selected_row, last_populated_row) of the task list, or (-1, -1).

    M57 made the rows shorter, which made the whole list fit without
    scrolling - and that quietly broke the precondition this suite used to
    wait on. The scrollbar thumb is flush with the bottom of its track
    both when the list is scrolled to the end *and* when there is nothing
    to scroll, so waiting on it stopped meaning "the selection reached the
    last row" and started meaning nothing at all. Reading the selection
    highlight directly says what was actually wanted."""
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

# M45's two context menus, which are deliberately the same three verbs in
# the same order drawn by two different processes - desktop_shell.c's
# CTX_* for the taskbar one and compositor.c's WMENU_* for the titlebar
# one. Identical geometry, so one set of constants covers both.
MENU_W = 124
MENU_ITEM_H = 22
MENU_MINIMIZE, MENU_CLOSE, MENU_FORCE_QUIT = 0, 1, 2
MENU_BG_RAW = 0x243040       # CTX_BG / WMENU_BG

# The taskbar's copy rises out of the bar itself (WM_ACTION_SET_PANEL_
# OVERHANG), so it is drawn into the panel's buffer and reaches the screen
# through the panel's own translucency - unlike the compositor's copy,
# which is drawn straight into the back buffer and is opaque.
def taskbar_menu_row_center(slot, row):
    """Where row `row` of the menu raised over taskbar slot `slot` is."""
    x = SLOTS_X + slot * (SLOT_W + SLOT_GAP)
    top = PANEL_TOP - MENU_ITEM_H * 3
    return (x + MENU_W // 2, top + row * MENU_ITEM_H + MENU_ITEM_H // 2)


# compositor.c: an ordinary window is placed at (100 + idx*40, 100 + idx*40).
# init spawns the desktop background, then the bottom taskbar, so those
# take slots 0-1 and the first app launched lands at slot 2. (It was 3
# until M42 deleted the top menu bar - a shift this file has to track by
# hand, which is why every geometry constant here names the source it
# mirrors.)
FIRST_APP_IDX = 2


# compositor.c's content_top_limit()/content_bottom_limit(): TITLEBAR_H +
# BORDER at the top, and the screen minus the docked taskbar at the
# bottom. A new window's cascade position is clamped between them.
CONTENT_TOP = 22
CONTENT_BOTTOM = SCREEN_H - 32


def app_origin(slot, height=None):
    """Where the compositor cascades the `slot`-th window.

    `height` mirrors compositor.c's own bottom clamp: a window tall enough
    that the cascade would push its bottom under the taskbar is moved *up*
    instead. Every window in this suite was short enough for that never to
    bite until M58 grew the Settings window for its Resolution pane, at
    which point a test computing the un-clamped cascade was simply
    clicking 76 pixels below everything it meant to click."""
    x = 100 + slot * 40
    y = 100 + slot * 40
    if height is not None:
        y = max(min(y, CONTENT_BOTTOM - height), CONTENT_TOP)
    return (x, y)


class Failure(Exception):
    pass


# ---------------------------------------------------------------------
# M56 geometry, mirrored from the sources that draw it.
# ---------------------------------------------------------------------

# text_editor.c: 80x24 cells of the 8x16 font, with MENU_ROWS(1) of menu
# bar above the text.
ED_W, ED_H = 640, 384
ED_CONTENT_Y0 = 16
ED_TEXT_COLOR = 0xE0E0E0

# gui_terminal.c: 70x21 cells of the same font.
TERM_W, TERM_H = 560, 336
TERM_TEXT_COLOR = 0xD0D0D0


def check(condition, message):
    if not condition:
        raise Failure(message)


def refusals(machine):
    """Every window request the compositor turned away, with its reason.
    M40 gave those a klog line of their own precisely so a test could
    assert on them: a refused connection used to be completely silent,
    which is how the bug this suite was written for stayed hidden."""
    return [line.split("refused: ", 1)[1]
            for line in machine.read_log().splitlines()
            if "[wm] window request refused: " in line]


def count_app_windows(shot):
    """How many apps the taskbar is showing - i.e. how many really have a
    window. Slots are packed left to right with no gaps, so the first
    non-slot-colored probe ends the count."""
    n = 0
    for i in range(10):
        x, y = slot_probe(i)
        if shot.px(x, y) not in SLOT_COLORS:
            break
        n += 1
    return n


def save_failure_shot(machine, name):
    """Copies the last screendump this machine produced somewhere it will
    outlive the guest, and returns the path (or None)."""
    src = os.path.join(machine._dir, "shot%03d.ppm" % machine._shot_seq)
    if not os.path.exists(src):
        return None
    os.makedirs(ARTIFACT_DIR, exist_ok=True)
    dst = os.path.join(ARTIFACT_DIR, "%s.ppm" % name)
    shutil.copyfile(src, dst)
    return dst


def desktop_is_painted(shot):
    """The desktop background, its first icon, the taskbar and the Start
    button in it all actually on screen - i.e. every one of the boot
    clients has connected *and* drawn its first frame. Waiting for this
    instead of a fixed sleep is what makes "click something immediately
    after boot" reliable: the serial log's [init] marker fires well before
    any of these pixels exist, and a click that lands in that gap goes
    nowhere at all."""
    return (shot.px(*EMPTY_DESKTOP) == DESKTOP_BG and
            shot.px(76, 76) == ICON_BOX and
            shot.px(512, PANEL_PROBE_Y) == PANEL_BG and
            shot.px(*TRAY_SEP_PROBE) == TRAY_SEP and
            shot.px(*START_PROBE) in START_COLORS)


def boot(machine, timeout=None):
    """M51-M56: 90 -> 300. The same growth tools/qemu-serial-test.sh's own
    SECONDS_TO_RUN took, and for the same reason - six new boot
    self-tests, several of which have to wait on real processes. 90 was
    measured failing during M55 with the boot only as far as [m49].

    A quiet-host boot is around 60 seconds and a busy-host one has been
    measured past 180, so this is five times the good case and not much
    over the bad one. That is deliberate: the two failures that pushed
    this past 150 were guests that had genuinely stopped making progress
    (both passed on a plain re-run, in 69s), and a *hung* guest is
    something the timeout can only report, never fix. Making the number
    generous costs nothing - this returns as soon as the marker appears -
    and keeps a slow host from being reported as a bug in whatever change
    is under test, which is the most expensive kind of test failure
    there is."""
    machine.boot_to_desktop(settle=0.0, timeout=timeout or machine.boot_timeout or 300)
    deadline = time.time() + 30
    shot = None
    while time.time() < deadline:
        shot = machine.screenshot()
        if desktop_is_painted(shot):
            return
        time.sleep(0.5)
    # Say *which* of the five probes is wrong, and keep the evidence.
    # "The desktop never finished painting" names a symptom with five
    # possible causes - the wallpaper, the first icon, the panel, the tray
    # separator, the Start button - and which one it is decides whether to
    # look at the compositor, desktop_icons or desktop_shell.
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
    """Re-screenshot until `predicate(shot)` holds, or fail saying what was
    expected and what the last look actually showed.

    Polling rather than one fixed sleep after each action: the guest is a
    software compositor doing full-screen redraws, so "how long until this
    is on screen" varies by a lot with how many windows are open. A sleep
    long enough to always be safe would make the suite slow, and one tuned
    to the common case makes it flaky - which is a particularly bad trade
    here, since flakiness in a test whose whole job is to catch a subtle
    bug just teaches you to re-run it."""
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
    """Which taskbar slot is drawn focused, or -1. The taskbar's focus
    accent is fed straight from wm_window_info_t.focused, so this reads
    the compositor's real focus state through the same pixels a person
    would look at."""
    for i in range(10):
        x, y = slot_probe(i)
        px = shot.px(x, y)
        if px not in SLOT_COLORS:
            break
        if px == SLOT_FOCUS_BG:
            return i
    return -1


# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------

def test_double_click_launches_every_icon(m):
    """M40's proving ground: double-click every desktop icon and require
    that many real windows. Before M40 this stopped at two - the third
    icon onward silently got window_id = -1 because the compositor's fd
    table was already full of pipes it had inherited from the kernel's own
    boot self-tests. Which two icons "didn't work" depended purely on
    launch order, which is why the bug got reported as being about the
    Editor and the Clock specifically.

    M74 added an eighth (README, in a second column), so this now spans
    both columns - which is also the only test that would notice if
    layout_icons ever stopped wrapping."""
    boot(m)
    for i, (name, _program, y, x) in enumerate(ICONS):
        m.double_click(x, y)
        wait_for_windows(m, i + 1)

    refused = refusals(m)
    check(not refused,
          "compositor refused %d window request(s): %s" % (len(refused), refused))


def test_single_click_does_not_launch(m):
    """The other half of "double-click launches": a single click must
    not. Cheap, and it's the check that would catch a future change
    turning DOUBLE_CLICK_MS into an always-true comparison."""
    boot(m)
    check(count_app_windows(m.screenshot()) == 0, "the desktop did not start empty")
    m.click(ICON_X, ICONS[0][2])
    # A fixed wait is right here, unlike everywhere else: the assertion is
    # that nothing happens, and there's no state to poll toward. Long
    # enough to comfortably cover a launch that shouldn't be happening.
    time.sleep(4.0)
    after = count_app_windows(m.screenshot())
    check(after == 0, "a single click opened %d window(s)" % after)


def test_titlebar_close_button(m):
    """M30's close button, reached by a real click on real pixels for the
    first time. Its existing self-test drives WM_ACTION_CLOSE down
    WM_ACTION_PIPE, which never touches the hit-test that turns a cursor
    position into that action."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock: small, no confirm-close prompt
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    m.click(*titlebar_button_center(x, y, CLOCK_W, BTN_CLOSE))
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "clicking the titlebar close button did not close the window")


def test_titlebar_drag_moves_window(m):
    """M31's move-drag: press on a titlebar, move, release. Only one
    window is open, so probing the desktop directly is unambiguous here -
    nothing can be occluding anything."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock
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
    """M32's Alt-Tab, through a real key chord rather than a direct call
    into alt_tab_cycle(). Focus is read off the taskbar's own focus accent,
    which the compositor feeds from wm_window_info_t.focused - the same
    thing a person looks at to see which app is in front."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[5][2])  # Paint
    before = wait_for_windows(m, 2)
    was = focused_slot(before)
    check(was >= 0, "no app is focused before Alt-Tab")

    # A deliberately short hold (QEMU's own default is 100ms). Holding it
    # for a human-length half second would paper over exactly the bug this
    # found: a chord whose modifier is released before the compositor gets
    # around to reading the character used to be dropped entirely.
    m.sendkey("alt-tab")
    shot = wait_for(m, lambda s: focused_slot(s) not in (was,),
                    "Alt-Tab left focus on the same app (taskbar slot %d)" % was)
    check(focused_slot(shot) >= 0, "nothing is focused after Alt-Tab")


def test_desktop_context_menu(m):
    """M35's right-click menu. Also the only test here that exercises the
    right mouse button end to end - QEMU's HMP numbers the buttons
    differently from the PS/2 wire protocol (see Machine.BTN_RIGHT), so
    this is what proves that translation is right."""
    boot(m)
    m.right_click(*EMPTY_DESKTOP)
    wait_for(m,
             lambda s: s.count_color(CTX_MENU_BG, EMPTY_DESKTOP[0],
                                      EMPTY_DESKTOP[1], 120, 40) > 500,
             "right-clicking the desktop did not draw the context menu")

    # First item is "Terminal" (desktop_icons.c CTX_MENU_PROGRAMS[0]).
    m.click(EMPTY_DESKTOP[0] + 40, EMPTY_DESKTOP[1] + 8)
    wait_for_windows(m, 1)


def test_launch_close_stress(m):
    """M40's stress requirement: repeatedly launch and close windows and
    confirm M29's reclaim path really holds under load. The failure this
    guards against is a slot/fd/shm leak that only shows after several
    cycles - exactly the shape of the bug M40 opened on, which needed no
    stress at all to hit but was invisible without a test that watched
    what happens after the second window."""
    boot(m)
    # M54: 5 -> 60, and the number is chosen rather than picked. Under
    # the old rules a task slot was claimed for the machine's whole
    # uptime, and the boot self-tests left 49 of MAX_TASKS unspent - so
    # the 50th launch here would have failed outright with
    # SPAWN_ERR_NO_TASK_SLOT and the window simply would not have opened.
    # 60 is past that and comfortably under the 119 a post-M54 boot
    # leaves, which makes this a test that passes now and could not have
    # before, rather than one that merely takes longer.
    #
    # It needs both halves of M54 to hold: the kernel recycling a reaped
    # slot, and the desktop actually reaping - an app launched from an
    # icon is desktop_icons.c's child, and nothing waited on it before
    # user_space/lib/children.h.
    rounds = 60
    for round_no in range(rounds):
        m.double_click(ICON_X, ICONS[4][2])  # Clock
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
    """M42's Start button, end to end through a real click: hovering it
    lights it, clicking it opens the compositor-owned launcher overlay,
    and clicking it again closes it.

    The action itself (WM_ACTION_TOGGLE_LAUNCHER down WM_ACTION_PIPE) has
    a boot-time self-test of its own (kernel.c's [m42]); what only this
    can show is that a click at the pixel a person would aim at reaches
    it - and, just as importantly, that it reaches the panel *at all*,
    since M42 stopped a click on a panel from focusing it and routes
    events to it by hover instead."""
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
    """M42's panel-focus rule, which is what makes the taskbar behave like
    Windows': clicking the Start button must not deactivate the app you
    were using. Before this the panel won the focus hit-test like any
    other window, so every taskbar click silently unfocused whatever was
    in front - which is also why M35's right-click desktop menu was
    unreachable from a fresh boot until M40 worked around it."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock
    wait_for_windows(m, 1)
    check(focused_slot(m.screenshot()) == 0, "the Clock did not take focus when it opened")

    m.click(*START_CLICK)
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) == LAUNCHER_BG,
             "clicking Start did not open the launcher overlay")
    check(focused_slot(m.screenshot()) == 0,
          "clicking the taskbar took focus away from the running app")
    m.click(*START_CLICK)  # leave the desktop as we found it


def test_taskbar_button_focus_and_minimize(m):
    """M22's one-click-does-both running-app button, reached by a real
    click for the first time. Its self-test drives WM_ACTION_FOCUS/
    TOGGLE_MINIMIZE down WM_ACTION_PIPE, which never touches the hit-test
    that turns a cursor position into either - and M42 moved every button
    in this row right of the new Start button, which is exactly the kind
    of shift only a real click can catch."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[5][2])  # Paint - takes focus off the Clock
    wait_for_windows(m, 2)
    check(focused_slot(m.screenshot()) == 1, "Paint did not take focus when it opened")

    # Clicking an unfocused button focuses it...
    m.click(*slot_click(0))
    wait_for(m, lambda s: focused_slot(s) == 0,
             "clicking the Clock's taskbar button did not focus it")
    # ...and clicking the focused one minimizes it.
    m.click(*slot_click(0))
    wait_for(m, lambda s: s.px(*slot_probe(0)) == SLOT_MIN_BG,
             "clicking the focused app's taskbar button did not minimize it")


def test_editor_in_window_file_menu(m):
    """M42 put text_editor's File menu back inside the editor's own window
    (M35's original shape) after M41 had moved it to a shared top bar.
    Same end-to-end claim M41's own test made, just against the window
    that owns the menu: clicking "File" opens the dropdown, and picking
    Quit closes the app."""
    boot(m)
    m.double_click(ICON_X, ICONS[1][2])  # Editor
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    m.click(x + EDITOR_MENU_X + 12, y + EDITOR_MENU_ROW_H // 2)
    wait_for(m,
             lambda s: s.count_color(EDITOR_MENU_BG, x + EDITOR_MENU_X,
                                      y + EDITOR_MENU_ROW_H + 2,
                                      EDITOR_MENU_ITEM_W, EDITOR_MENU_ITEM_H) > 200,
             "clicking File in the editor's own menu row did not open its dropdown")

    # File > Quit is item 3; text_editor exits cleanly on it for an
    # unmodified buffer.
    m.click(x + EDITOR_MENU_X + EDITOR_MENU_ITEM_W // 2,
            y + EDITOR_MENU_ROW_H + 3 * EDITOR_MENU_ITEM_H + EDITOR_MENU_ITEM_H // 2)
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "picking File > Quit did not close the Editor")


def test_launcher_keychord_types_and_launches(m):
    """M43's launcher, end to end from the keyboard: Ctrl+Space opens it
    over everything, typing narrows the list, and Enter spawns the top
    match. The chord is intercepted in the compositor next to M32's
    Alt+Tab and for the same reason - it must not be something a focused
    client can see or swallow, which is only true if it works with an app
    focused, so this opens one first."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock, so something holds focus
    wait_for_windows(m, 1)

    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) == LAUNCHER_BG,
             "Ctrl+Space did not open the launcher")
    check(m.screenshot().px(*launcher_row_probe(0)) == LAUNCHER_SEL_BG,
          "the launcher's first result is not drawn selected")

    m.type_text("gui_pai")  # gui_paint, and nothing else on disk
    m.sendkey("ret")
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) != LAUNCHER_BG,
             "Enter did not dismiss the launcher")
    wait_for_windows(m, 2)


def test_launcher_click_launches_a_result(m):
    """The pointer half of the same thing: opened from the Start button,
    filtered by typing, then a real click on the result row rather than
    Enter. Filtering first is what makes the click meaningful - row 0 of
    an unfiltered list is a coreutil that opens no window at all, so
    clicking it would prove nothing either way."""
    boot(m)
    m.click(*START_CLICK)
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) == LAUNCHER_BG,
             "the Start button did not open the launcher")

    m.type_text("gui_clo")  # gui_clock, and nothing else on disk
    m.click(*launcher_row_probe(0))
    wait_for_windows(m, 1)


def test_launcher_escape_dismisses(m):
    """Escape closes it and launches nothing. The launcher takes the
    keyboard away from the focused window for as long as it is up, so
    "there is always a way out that doesn't run something" is a real
    requirement, not a nicety."""
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
    """M43's edge snapping, as the gesture rather than as the action: hold
    the titlebar, shove the pointer into the right edge, and check both
    that the preview appears *before* release and that releasing puts the
    window in exactly the right half.

    The snap math itself has a boot-time self-test (kernel.c's [m43]),
    driven through WM_ACTION_SNAP_RIGHT - the same function this gesture
    calls. What only this can show is that the drag recognizes the edge at
    all, and that what the preview promises is what release delivers."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock: 200x90, narrower than half the screen
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    m.press(x + 100, y - TITLEBAR_H // 2)
    m.move_held(1020, 400)

    # The preview covers the outer (border- and titlebar-inclusive) rect
    # the window will land in: content 200x90 at (514, 22), so
    # x:[512, 716), y:[0, 112). (600, 60) is inside it and nowhere near
    # the dragged window, which is clamped to the far right at y ~ 410.
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
    """M45's headline: a real right-click on a taskbar button raises a menu
    that rises *out* of a 32px panel, and Force Quit on it removes the
    window.

    Everything here is new plumbing that no protocol-level test reaches.
    The menu is drawn by desktop_shell.c into rows of its own buffer that
    sit above the docked strip, and it is only visible - and only
    clickable - because the compositor was told to raise the overhang
    (WM_ACTION_SET_PANEL_OVERHANG). A test that sent WM_ACTION_KILL down
    the action pipe would pass with every one of those pieces missing."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock
    wait_for_windows(m, 1)

    m.right_click(*slot_click(0))
    # The menu occupies rows the panel did not previously composite at
    # all, so "is it up" is a question about a pixel that was bare desktop
    # a moment ago. Read at the menu's own top row, clear of its label
    # text and of its 1px border.
    probe = (SLOTS_X + MENU_W - 12, PANEL_TOP - MENU_ITEM_H * 3 + 6)
    expected = panel_px(MENU_BG_RAW, probe[1])
    wait_for(m, lambda s: s.px(*probe) == expected,
             "right-clicking a taskbar button did not raise its context menu "
             "(the panel overhang never came up)")

    m.click(*taskbar_menu_row_center(0, MENU_FORCE_QUIT))
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "Force Quit on the taskbar context menu did not remove the window")
    # And the overhang goes back down with it - a menu that stayed
    # composited would be a strip of stale pixels sitting over the desktop.
    wait_for(m, lambda s: s.px(*probe) == desktop_px(probe[1]),
             "the panel overhang stayed raised after the menu closed")


def test_titlebar_right_click_force_quit(m):
    """The same three verbs from the other entry point - a right-click on
    the window's own titlebar, drawn by the compositor rather than by the
    taskbar. Both end at apply_window_action, which is exactly why both
    are worth driving from real pixels: the two menus are separate code,
    and only the action underneath them is shared."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    menu_x, menu_y = x + 60, y - TITLEBAR_H // 2
    m.right_click(menu_x, menu_y)
    # Compositor-drawn, so opaque - no panel blend. Probed on the *second*
    # row: the pointer comes to rest inside the first one, which the
    # compositor then draws hovered, so a probe there would be reading the
    # highlight rather than the menu.
    probe = (menu_x + MENU_W - 12, menu_y + MENU_ITEM_H + MENU_ITEM_H // 2)
    wait_for(m, lambda s: s.px(*probe) == MENU_BG_RAW,
             "right-clicking a titlebar did not raise the window context menu")

    m.click(menu_x + MENU_W // 2, menu_y + MENU_FORCE_QUIT * MENU_ITEM_H + MENU_ITEM_H // 2)
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "Force Quit on the titlebar context menu did not remove the window")


def test_ctrl_shift_esc_opens_task_manager(m):
    """The chord every desktop reserves for this, intercepted in
    handle_keyboard beside Alt+Tab. Nothing else can open the task manager
    without a click, and a chord that never reaches the compositor looks
    exactly like a chord that isn't bound."""
    boot(m)
    check(count_app_windows(m.screenshot()) == 0, "the desktop did not start empty")
    m.sendkey("ctrl-shift-esc")
    wait_for_windows(m, 1)


def test_task_manager_end_task(m):
    """M45's task manager acting on a real process, through its own list
    and its own button.

    The victim is launched *after* the task manager, so it is the newest
    task in the table and therefore the last row - tasks are appended and
    never reordered (their slots are never recycled either, see
    sched.c). Holding Down past the end of the list is what selects it
    without this test having to know how many tasks a boot happens to
    leave behind, which is a number no test should be asserting on."""
    boot(m)
    m.double_click(ICON_X, ICONS[6][2])  # Tasks
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[4][2])  # Clock - the victim, and the newest task
    wait_for_windows(m, 2)

    tx, ty = app_origin(FIRST_APP_IDX)
    # The task manager's own header strip: focuses its window without
    # also landing on a list row. The clock's window (slot 3, at
    # (220, 220), 200x90) is nowhere near this point.
    m.click(tx + TASKS_W - 40, ty + 8)

    # Far more Downs than there are tasks: the handler clamps at
    # task_count - 1, so overshooting is how "select the last row" is
    # expressed without knowing the count.
    for _ in range(90):
        m.sendkey("down")

    # ...and then wait for the guest to have actually consumed them all
    # before clicking anything. This is not politeness: 90 keystrokes is
    # more than a 1024-byte event pipe holds (wm_event_t is 20 bytes), so
    # the compositor blocks part-way through forwarding them - and its
    # main loop services the mouse *before* the keyboard, which means a
    # click issued while that backlog is draining lands in the middle of
    # it. That is exactly how this test first failed: End Task fired with
    # the selection still up in the desktop processes.
    #
    # The selection highlight sitting on the last populated row is what
    # says "done" precisely - a direct read of the precondition this test
    # needs, not a proxy for elapsed time.
    def selection_at_end(shot):
        selected, last = tm_row_states(shot, tx, ty)
        return selected >= 0 and selected == last

    wait_for(m, selection_at_end,
             "the selection never reached the last row of the task list", timeout=25.0)

    # End Task, the leftmost of the two buttons (task_manager.c's
    # END_BTN_X / BTN_Y).
    end_x = tx + TASKS_W - 96 - 8 - 96 - 8 + 48
    end_y = ty + TASKS_H - 22 - 6 + 11
    m.click(end_x, end_y)
    wait_for(m, lambda s: count_app_windows(s) == 1,
             "End Task in the task manager did not terminate the selected process")


def test_titlebar_button_hover_lights_and_still_closes(m):
    """M46's hover feedback, which no titlebar button in this project has
    ever had - and, in the same test, proof that adding it didn't cost the
    click. Hover state is the kind of thing that is easy to get subtly
    wrong (lit while the pointer is elsewhere, or a lit button that isn't
    the one that acts, since the highlight and the click now share one
    hit-test); reading the pixel and then clicking the same place is what
    checks both halves against each other."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    # Read the *minimize* button while hovering it: it is the outermost of
    # the three, so the 8x8 pointer parked on the close button (which is
    # where a click would go) can't be sitting on the pixel being read.
    probe = titlebar_button_disc(x, y, CLOCK_W, BTN_MINIMIZE)
    m.move_to(x + 60, y - TITLEBAR_H // 2)  # over the titlebar, off every button
    shot = m.screenshot()
    check(shot.px(*probe) == BTN_MINIMIZE_COLOR,
          "the minimize button was already lit with the pointer elsewhere on the titlebar "
          "(got 0x%06X)" % shot.px(*probe))

    m.move_to(*titlebar_button_center(x, y, CLOCK_W, BTN_MINIMIZE))
    expected = lighten(BTN_MINIMIZE_COLOR, 1, BTN_HOVER_LIGHTEN)
    wait_for(m, lambda s: s.px(*probe) == expected,
             "hovering the minimize button did not light it")

    # And the close button still closes, from a real click at a real pixel.
    m.click(*titlebar_button_center(x, y, CLOCK_W, BTN_CLOSE))
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "the titlebar close button stopped closing once hover tracking was added")


def test_titlebar_double_click_maximizes(m):
    """M46: double-clicking a titlebar toggles maximize/restore, through
    apply_window_action like everything else. M40 made double-click
    detection latency-independent by timestamping events in the PS/2
    handler; this is that machinery's second user, and the first one
    inside the compositor itself."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock: 200x90, so maximizing visibly moves it
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    # Somewhere on the titlebar body: left of the buttons, right of the
    # title text, and not a resize edge.
    m.double_click(x + 100, y - TITLEBAR_H // 2)
    # Maximized puts the window at (BORDER, content_top_limit) = (2, 22),
    # clamped to its own 200x90 buffer - so its titlebar lands in
    # y:[2, 22) at the far left, where bare desktop was a moment ago.
    wait_for(m, lambda s: s.px(60, 12) == ACCENT,
             "double-clicking the titlebar did not maximize the window")

    m.double_click(2 + 100, 12)
    wait_for(m, lambda s: s.px(60, 12) == desktop_px(12),
             "double-clicking the titlebar again did not restore the window")


def test_resize_edge_changes_cursor(m):
    """M31's resize zones are 5px wide and were completely invisible: the
    only way to find one was to guess. The compositor picks a cursor shape
    from the same resize mask the drag itself uses, so this checks the one
    thing no protocol-level test can - that moving the pointer onto an
    edge changes what is drawn under it.

    Matched against the harness's own copies of compositor.c's cursor
    bitmaps (qemu_input.CURSOR_SHAPES) rather than by counting white
    pixels, because a count is not specific: the arrow's 12 pixels and
    whatever the window underneath happens to be drawing are the same
    color."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)

    def shape_at(at):
        m.move_to(*at)
        found = m.find_cursor_shape(m.screenshot(), at)
        check(found is not None, "no cursor found at (%d, %d)" % at)
        return found[2]

    got = shape_at((x + 100, y + 45))  # over the window's own content
    check(got == "arrow", "expected the plain arrow over window content, got %r" % got)

    # The right border, clear of both corners.
    got = shape_at((x + CLOCK_W + 2, y + 45))
    check(got == "horizontal",
          "moving onto a resize edge did not change the drawn cursor (got %r)" % got)

    # The bottom-right corner, where two edges overlap.
    got = shape_at((x + CLOCK_W + 2, y + 90))
    check(got == "diag_nw_se",
          "the bottom-right corner did not get the diagonal resize cursor (got %r)" % got)

    # The titlebar body: M46's move cursor, the one genuinely new shape.
    got = shape_at((x + 100, y - TITLEBAR_H // 2))
    check(got == "move", "the titlebar did not get the move cursor (got %r)" % got)


def test_shutdown_powers_off_the_machine(m):
    """M47, and the first test in this project to watch the guest exit on
    its own.

    Every other test here - and tools/qemu-serial-test.sh - ends by
    killing QEMU, so "does S5 actually fire" had never once been observed
    from either side. QEMU's own process terminating is the only real
    proof: a guest that merely halted, or wrote the wrong port and kept
    running, leaves the process exactly where it was.

    Driven through the UI rather than by spawning the `shutdown` command,
    because the path being checked is the whole one: Start -> the
    launcher's Power row -> a confirm step -> SYS_shutdown -> an orderly
    stop -> ACPI."""
    boot(m)
    m.click(*START_CLICK)
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "the Start button did not open the launcher")

    m.click(*power_button_center(0))  # Shut Down
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
    """The other half of putting a confirm step in front of it: Cancel
    must leave the machine running. This is the check that would catch a
    confirm box wired to the wrong key, which is a mistake you only find
    out about once."""
    boot(m)
    m.click(*START_CLICK)
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "the Start button did not open the launcher")

    m.click(*power_button_center(0))
    wait_for(m, lambda s: s.px(*power_confirm_probe()) == POWER_CONFIRM_BG,
             "clicking Shut Down did not raise a confirm box")

    m.sendkey("n")
    # The box goes away and the launcher is still up underneath it.
    wait_for(m, lambda s: s.px(*power_confirm_probe()) != POWER_CONFIRM_BG and
                          s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "cancelling the confirm box did not return to the launcher")
    # And, crucially, nothing happened. A fixed wait is right here for the
    # same reason it is in single_click_does_not_launch: the assertion is
    # that the machine is still on, and there is no state to poll toward.
    time.sleep(4.0)
    check(m.exit_status is None,
          "cancelling the shutdown confirm powered the machine off anyway")


def test_settings_persist_across_a_reboot(m):
    """M47's best available end-to-end proof that a shutdown really wrote
    the disk: pick a wallpaper, restart, and see it still set.

    Deliberately a *restart* rather than two separate boots - a test that
    booted twice would only prove SYS_writefile works, which M33 already
    covers. What this adds is that the write survived the whole power
    path, and that the compositor reads it back before the first client
    connects. The guest's disk writes do survive: snapshot=on keeps them
    in an overlay for the lifetime of the QEMU process, and a reset is the
    same process.

    The restart is triggered by typing `reboot` into the launcher rather
    than by clicking its Power row - not to avoid the Power row (
    shutdown_powers_off_the_machine drives that) but because every pixel
    constant in this file is computed against the *default gradient*
    desktop, and this test's whole point is that the desktop is no longer
    that. Reading the taskbar or the launcher's own fill after the
    wallpaper changes would be measuring the wrong blend."""
    boot(m)
    m.double_click(ICON_X, ICONS[3][2])  # Settings
    wait_for_windows(m, 1)

    # settings.c's wallpaper row: four preview buttons under the
    # "Wallpaper" label, WALL_BTN_X(i) = GFX_PAD + i * (WALL_BTN_W + gap).
    # The first is WALLPAPER_FLAT, which is the one choice a screenshot
    # can tell apart from the default gradient with a single pair of
    # pixels: flat means two rows 400px apart read identical.
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

    # The guest resets and boots all the way through its self-tests again,
    # which takes about as long as the original boot did - so this budget
    # tracks boot()'s own and is generous for the same reason. 200 was
    # measured failing at the end of M55 with the second boot only as far
    # as [m52].
    deadline = time.time() + 300
    while time.time() < deadline:
        if m.read_log().count(BOOT_MARKER) > boots_before:
            break
        time.sleep(1.0)
    else:
        raise Failure("the machine never came back up after `reboot` (log tail: %r)"
                      % m.read_log()[-400:])
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
    """M74's whole point, graded on pixels rather than on protocol.

    Two windows are opened and one of them is dragged somewhere the
    cascade would never put it. The machine is restarted. Both windows
    have to come back, and the dragged one has to be *where it was left* -
    which is the assertion that separates "the session file was written
    and read" from "the desktop remembers". A compositor that relaunched
    both programs and let them cascade would pass a window count and fail
    this.

    The restart goes through the launcher's `reboot`, for the same reason
    settings_persist_across_a_reboot does: it is the path that actually
    exercises SYS_shutdown, and the guest's disk writes survive a reset
    because snapshot=on keeps them in an overlay for the lifetime of the
    QEMU process."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock
    wait_for_windows(m, 1)
    x, y = app_origin(FIRST_APP_IDX)

    # Somewhere the cascade cannot land: the cascade starts at (100, 100)
    # and steps 40px per window, so the second slot is (140, 140). This
    # moves the Clock a long way right and down from both.
    dest_x, dest_y = x + 380, y + 260
    m.drag(x + 100, y - 8, dest_x + 100, dest_y - 8)
    moved = wait_for(m, lambda s: s.px(dest_x + 4, dest_y - 8) != desktop_px(dest_y - 8),
                     "the Clock did not move to where it was dragged")
    titlebar = moved.px(dest_x + 4, dest_y - 8)

    m.double_click(ICON_X, ICONS[5][2])  # Paint, so the session has two entries
    wait_for_windows(m, 2)

    # The compositor writes the session at most twice a second and only
    # when the layout has changed, so this waits out one whole check
    # interval plus margin rather than assuming the write already
    # happened. Nothing to poll for from out here - the file is inside
    # the guest.
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
        raise Failure("the machine never came back up after `reboot` (log tail: %r)"
                      % m.read_log()[-400:])

    # Both windows, and the moved one at its own coordinates. The window
    # count is checked first because "nothing came back" and "the wrong
    # thing came back" are different failures and the first would
    # otherwise be reported as the second.
    wait_for(m, lambda s: count_app_windows(s) >= 2,
             "the session did not bring both windows back", timeout=90.0)
    wait_for(m, lambda s: s.px(dest_x + 4, dest_y - 8) == titlebar,
             "a window came back, but not where it was left - the session restored "
             "the program and let the cascade place it", timeout=30.0)


def test_launcher_does_not_offer_data_files(m):
    """M53, and the retirement of what this test used to check.

    It used to type "m33test" - an ordinary text file the boot self-tests
    leave on disk - into the launcher and require M48's "That file is not
    a program." toast, because a flat filesystem left the launcher
    listing every file there was. The launcher lists /bin now, so a data
    file is not something it can offer to run at all: the right assertion
    is that there is *no result and no toast*, which is a better outcome
    than a good error message about a thing that should never have been
    offered.

    /home/readme.txt is the file to try: real, seeded on every fresh
    disk, and visible in the file manager one window away - which is
    exactly the confusion the old behavior caused."""
    boot(m)
    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "Ctrl+Space did not open the launcher")
    check(m.screenshot().px(*launcher_row_probe(0)) == LAUNCHER_SEL_BG,
          "the launcher's first result is not drawn selected before typing")

    m.type_text("readme")
    # No results means row 0 is no longer drawn with the selection fill -
    # there is nothing to select.
    wait_for(m, lambda s: s.px(*launcher_row_probe(0)) != LAUNCHER_SEL_BG,
             "the launcher still offered a result for a file that is not a program")

    m.sendkey("ret")
    wait_for(m, lambda s: s.px(*LAUNCHER_PROBE) != LAUNCHER_BG,
             "Enter did not dismiss the launcher")
    # Nothing to poll toward: the assertion is that nothing happened.
    time.sleep(4.0)
    shot = m.screenshot()
    check(count_app_windows(shot) == 0,
          "Enter on an empty launcher opened %d window(s)" % count_app_windows(shot))
    check(shot.px(*toast_stripe_probe(0)) != TOAST_ERROR_C,
          "a toast was raised for a file the launcher should never have offered")


def test_clicking_a_toast_dismisses_it(m):
    """A toast is dismissable early, and the click is consumed rather
    than falling through to whatever is underneath it - which matters
    because a toast lands in the top-right corner, exactly where a
    maximized window's close button is."""
    boot(m)
    # M53: the toast this used to raise - "that file is not a program",
    # from launching a data file - cannot happen any more, because the
    # launcher only lists /bin. The crash toast M52 gave a real source is
    # what a toast is for now, so that is what this dismisses.
    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "Ctrl+Space did not open the launcher")
    m.type_text("wm_faulter")
    m.sendkey("ret")

    probe = toast_stripe_probe(0)
    wait_for(m, lambda s: s.px(*probe) == TOAST_ERROR_C, "no toast was raised", timeout=15.0)

    m.click(*toast_click_point(0))
    # Dismissed well inside its own 4s deadline, so this can only be the
    # click - a wait that outlived the timer would prove nothing.
    shot = wait_for(m, lambda s: s.px(*probe) != TOAST_ERROR_C,
                    "clicking the toast did not dismiss it", timeout=2.5)
    check(shot.px(*probe) == desktop_px(probe[1]),
          "the toast went away but left something behind at 0x%06X" % shot.px(*probe))


def test_wheel_scrolls_the_file_list_one_row_per_detent(m):
    """M49's wheel, end to end: a real emulated IntelliMouse packet
    through QEMU's own `mouse_move dx dy dz`, negotiated by
    kernel/drivers/mouse.c's 200/100/80 knock, routed as
    WM_EVENT_MOUSE_WHEEL to whatever the pointer is over, and turned into
    exactly one row of scroll per detent.

    "Exactly one row" is checked without reading any filename: capture the
    pixels of list row N before scrolling, scroll N detents, and require
    row 0 to be those same pixels afterwards. That is content-independent
    and off-by-one-proof in a way that counting rows by eye is not."""
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])  # Files
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    list_x = x + 6
    row0_y = y + FM_LIST_Y

    # M53: the window opens on /home, which is deliberately short. /bin is
    # where a list long enough to scroll lives, so this navigates there
    # first - up to "/" through the ".." row, then into "bin", which is
    # the root's own row 0 (the root has no ".." to push it down).
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

    # The pointer has to be *over* the list: the wheel acts on what it is
    # over, not on what holds focus, which is the whole point of routing
    # it that way.
    m.move_to(x + FM_W // 2, y + FM_H // 2)
    before = m.screenshot()
    want = row_pixels(before, 3)
    check(any(p != want[0] for p in want),
          "the row this test scrolls to is blank - the file list is too short to scroll")

    m.wheel(3)
    after = m.screenshot()
    got = row_pixels(after, 0)
    if got != want:
        # QEMU's dz sign is the emulator's convention, not the guest's;
        # try the other direction before calling this a failure. The
        # assertion that matters is the distance, not which way QEMU
        # spells "down".
        m.wheel(-6)
        after = m.screenshot()
        got = row_pixels(after, 0)
    check(got == want,
          "three wheel detents did not move the file list by exactly three rows")


def test_alt_f4_closes_the_focused_window(m):
    """One of the chords a desktop is expected to have, and the one whose
    key did not previously exist at all: kernel/drivers/keyboard.c decoded
    printable ASCII and four arrows, so F4 produced nothing. Alt+F4 goes
    through WM_ACTION_CLOSE, so it honors an app's confirm_close opt-in -
    it is the polite verb, not the forceful one, which is why the victim
    here is the Clock rather than the Editor."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock
    wait_for_windows(m, 1)
    m.sendkey("alt-f4")
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "Alt+F4 did not close the focused window")


def test_ctrl_alt_arrows_snap_and_maximize(m):
    """M43's snap actions and M30's maximize, reached from the keyboard.
    Every one of these already had an action to call - what was missing
    was only the binding, so this is checking the binding, using the same
    pixels M43's own drag test grades its result on."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock: 200x90, narrower than half the screen
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
    """M49's one genuinely new piece of plumbing: a payload carried across
    two clients that know nothing about each other. Dragging a filename
    out of file_manager.c and dropping it on the desktop opens it in the
    editor - which means the drag label appeared, the compositor held the
    payload across the gesture, and desktop_icons.c got both the
    WM_EVENT_DROP and the payload behind it."""
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])  # Files
    wait_for_windows(m, 1)

    x, y = app_origin(FIRST_APP_IDX)
    # Press on the first row, then drag well clear of the window onto
    # empty desktop. The press alone only selects; the drag begins once
    # the pointer has moved past file_manager.c's own DRAG_THRESHOLD.
    # M53: row 0 is the ".." entry now, so the first real file is row 1 -
    # dragging a directory would be a different (and meaningless) gesture.
    m.press(*fm_row_point(x, y, FM_FIRST_FILE_ROW))
    m.move_held(700, 500)

    shot = m.screenshot()
    check(shot.count_color(DRAG_LABEL_BG, 700, 500, 200, 32) > 0,
          "no drag label followed the cursor - the drag never started")

    m.release()
    wait_for(m, lambda s: count_app_windows(s) == 2,
             "dropping a file on the desktop did not open it in the editor")


# ---------------------------------------------------------------------
# M51: overlapping windows. Everything below needs two windows that
# really cover each other, which is why they use Files (280x360 at the
# slot-2 cascade position, so x:[180,460) y:[180,540)) and Tasks (420x360
# at slot 3, x:[220,640) y:[220,580)) rather than the small Clock every
# other test here reaches for.
#
# Files' *right* border column and Tasks' *left* border column both fall
# inside the other window, so each is visible exactly when its own window
# is in front - two probes that answer "which one is on top" positively in
# both directions instead of one probe that only says "something changed".
# ---------------------------------------------------------------------

FILES_ORIGIN = app_origin(FIRST_APP_IDX)
TASKS_ORIGIN = app_origin(FIRST_APP_IDX + 1)
# Taskbar slots are numbered over *apps* only - the panel and the desktop
# background are filtered out of the running list - so the first app
# launched is slot 0 even though its window is the third the compositor
# holds and therefore lands on the FIRST_APP_IDX cascade position. Two
# different numbering schemes for the same two windows, which is worth
# naming rather than writing 0 and 1 inline.
FILES_SLOT = 0
TASKS_SLOT = 1
# Files' right border, at a row well below both windows' rounded top
# corners and clear of any cursor parked on a titlebar.
FILES_IN_FRONT_PROBE = (FILES_ORIGIN[0] + FM_W, 400)
# Tasks' left border, in the same row.
TASKS_IN_FRONT_PROBE = (TASKS_ORIGIN[0] - BORDER, 400)
# Titlebar-body click points: each one is on its own window's titlebar and
# outside the *other* window's frame entirely, so which window a click
# there reaches is not itself the thing under test.
FILES_TITLEBAR_CLICK = (FILES_ORIGIN[0] + 20, FILES_ORIGIN[1] - TITLEBAR_H // 2)
# M59: Files grew for its new columns, which narrowed the band of Tasks'
# titlebar that is both clear of Files' frame (x > FILES right edge) and
# clear of Tasks' own three buttons (54px in from its right edge:
# 3 * BTN_SIZE(14) + 2 * BTN_GAP(4) + BTN_MARGIN(4)). This picks the
# middle of that band rather than a fixed offset, so it stays correct if
# either window changes width again - and it is an error, not a silent
# mis-click, if the band ever closes.
TASKS_W = 400  # task_manager.c WIN_W
_TASKS_BTN_BAND = TASKS_ORIGIN[0] + TASKS_W - 54
_TASKS_FREE_LO = FILES_ORIGIN[0] + FM_W + 4
if _TASKS_FREE_LO >= _TASKS_BTN_BAND:
    raise SystemExit("qemu_input_suite: Files is now wide enough to cover every clickable "
                     "part of Tasks' titlebar - the overlap tests need a different pair")
TASKS_TITLEBAR_CLICK = ((_TASKS_FREE_LO + _TASKS_BTN_BAND) // 2, TASKS_ORIGIN[1] - TITLEBAR_H // 2)
# Inside both windows' content, and well away from both probe rows.
OVERLAP_CLICK = (400, 480)


def open_overlapping_pair(m):
    """Files then Tasks, so Tasks (launched last, and therefore focused)
    starts in front. Returns nothing - the two probes above are how every
    caller reads the result."""
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])   # Files
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[6][2])   # Tasks
    wait_for_windows(m, 2)
    wait_for(m, lambda s: s.px(*TASKS_IN_FRONT_PROBE) == BORDER_COLOR,
             "the second window launched did not start in front of the first")


def test_clicking_a_window_raises_it(m):
    """M51's headline: clicking a window you can see brings it forward.
    Before this milestone the compositor painted in creation order, so the
    window in front was permanently whichever client connected last and a
    click could only ever change a titlebar color."""
    open_overlapping_pair(m)

    # The raise and the focus accent are asserted in one predicate rather
    # than one after the other: the compositor reorders and repaints
    # immediately, while the taskbar only learns about the new focus on
    # its next poll of the query protocol - so a shot taken the instant
    # the window moved forward can legitimately still show the old
    # button lit, and asserting on it would be a race, not a check.
    m.click(*FILES_TITLEBAR_CLICK)
    shot = wait_for(m, lambda s: (s.px(*FILES_IN_FRONT_PROBE) == BORDER_COLOR and
                                  focused_slot(s) == FILES_SLOT),
                    "clicking the covered window's titlebar did not raise and focus it")
    check(shot.px(*TASKS_IN_FRONT_PROBE) != BORDER_COLOR,
          "the window that was raised did not cover the one that had been in front")

    # And back the other way, which is what says this is an order rather
    # than a one-time swap.
    m.click(*TASKS_TITLEBAR_CLICK)
    shot = wait_for(m, lambda s: (s.px(*TASKS_IN_FRONT_PROBE) == BORDER_COLOR and
                                  focused_slot(s) == TASKS_SLOT),
                    "clicking the other window's titlebar did not raise and focus it back")
    check(shot.px(*FILES_IN_FRONT_PROBE) != BORDER_COLOR,
          "both windows claim to be in front after the second raise")


def test_overlap_click_reaches_the_front_window(m):
    """The occlusion bug, as a click. With Files raised over Tasks, a
    press in the region both cover has to reach Files. Before M51 every
    hit-test walked windows[] backwards and took the first *match*, so it
    would have reached Tasks - a window that at that pixel is not visible
    at all."""
    open_overlapping_pair(m)
    m.click(*FILES_TITLEBAR_CLICK)
    wait_for(m, lambda s: s.px(*FILES_IN_FRONT_PROBE) == BORDER_COLOR,
             "the covered window did not raise, so there is no occlusion to test")

    m.click(*OVERLAP_CLICK)
    # Nothing to poll toward if the click is correctly a no-op for focus
    # (it lands on the already-focused window), so this waits out a focus
    # change that must not happen and then reads the result.
    time.sleep(2.0)
    shot = m.screenshot()
    check(focused_slot(shot) == FILES_SLOT,
          "a click in the overlap region focused slot %d - the window behind, "
          "which is not visible at that pixel" % focused_slot(shot))
    check(shot.px(*FILES_IN_FRONT_PROBE) == BORDER_COLOR,
          "the front window stopped being in front after being clicked")


def test_alt_tab_visits_windows_in_use_order(m):
    """M51 makes Alt+Tab walk the z-order, which - now that focus raises -
    is the most-recently-used order. It used to walk windows[] by slot
    index, i.e. by launch order, regardless of what you had been using.

    Three windows, launched Clock, Files, Tasks. Alt+Tab from Tasks must
    reach Files (the one used before it) and not Clock; a second Alt+Tab
    comes back to Tasks, which is what tapping it does on a real desktop
    once focus raises. Shift+Alt+Tab then goes the other way, wrapping
    past the top of the order to the window used longest ago."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])   # Clock
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[2][2])   # Files
    wait_for_windows(m, 2)
    m.double_click(ICON_X, ICONS[6][2])   # Tasks
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
    """M52's headline claim, from the side a person actually sees it.

    Before this milestone isr.c panicked on every fault regardless of
    ring, so a null dereference anywhere in user space stopped the whole
    machine - by a distance the largest source of "you have to reset it"
    this project has had. Now it kills one process, the compositor
    notices the death it did not ask for, and M48's crash toast finally
    has something real to report rather than only ever firing for a
    failed spawn.

    The proof that the machine survived is deliberately not "the
    screenshot still looks like a desktop" - a hung guest would keep
    showing the last frame it painted. It is that a *new* program
    launched afterwards opens a window, which needs the compositor, the
    taskbar, the scheduler and the filesystem all still working."""
    boot(m)
    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "Ctrl+Space did not open the launcher")
    m.type_text("wm_faulter")
    m.sendkey("ret")

    # It connects, paints, and only then dereferences null - see
    # user_space/bin/wm_faulter.c's ALIVE_MS. Waiting for its window
    # first is what makes the crash a crash of something that was alive.
    wait_for_windows(m, 1)

    probe = toast_stripe_probe(0)
    wait_for(m, lambda s: s.px(*probe) == TOAST_ERROR_C,
             "a client that faulted raised no crash toast", timeout=15.0)
    wait_for(m, lambda s: count_app_windows(s) == 0,
             "the faulting client's window was never reclaimed")

    # The real assertion: the machine is still a working desktop.
    m.double_click(ICON_X, ICONS[4][2])  # Clock
    wait_for_windows(m, 1, timeout=15.0)


def test_file_manager_navigates_directories(m):
    """M53's file manager, driven as a person would.

    The window opens on /home rather than on the whole namespace, and the
    two things that were impossible before this milestone are entering a
    directory and leaving one.

    Asserted by *how many rows have anything in them* rather than by
    reading names: /home holds ".." and one seeded readme, while /bin
    holds every program this OS ships and fills the window. That is a
    content-independent way to say "these are different directories, and
    the deeper one is the one that should be full" without depending on a
    font's shape or on what happens to be on disk."""
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])  # Files
    wait_for_windows(m, 1)
    x, y = app_origin(FIRST_APP_IDX)

    home_rows = fm_rows_with_text(m.screenshot(), x, y)
    check(0 < home_rows < FM_ROWS_VISIBLE,
          "/home did not open as a short list (%d rows)" % home_rows)

    # Up to "/", then into "bin" - the root's own row 0, since the root
    # has no ".." row to push its contents down.
    m.double_click(*fm_row_point(x, y, FM_UP_ROW))
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) > 0,
             "double-clicking .. left the file manager showing nothing")
    m.double_click(*fm_row_point(x, y, FM_ROOT_FIRST_ROW))
    wait_for(m, lambda s: fm_rows_with_text(s, x, y) >= FM_ROWS_VISIBLE,
             "double-clicking /bin did not enter a directory full of programs")

    # And back out. /bin's ".." leads to "/", which lists four
    # directories and nothing else - so the list must go short again.
    m.double_click(*fm_row_point(x, y, FM_UP_ROW))
    wait_for(m, lambda s: 0 < fm_rows_with_text(s, x, y) < FM_ROWS_VISIBLE,
             "double-clicking .. did not leave /bin")


def test_desktop_survives_losing_the_compositor(m):
    """M55, and the claim the whole milestone rests on: the desktop is no
    longer the thing that has to be alive for anything else to be.

    A compositor that died used to take every client with it - not by
    killing them, but by wedging them. Their pixels were its shm segment,
    their events came down its pipe, and neither survives it; a client
    sat there owning a window nobody was compositing. So the assertion
    here is deliberately about an app launched *before* the crash still
    being on screen *after* it, which is the difference between "the
    session restarted" and "the session survived".

    The compositor is killed by `wm_crash`, a self-test-only program that
    finds it by name in SYS_taskinfo and SIGKILLs it - see its own header
    for why that had to exist. Launched from the launcher, so the whole
    path is real user input."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock - the app that must outlive the crash
    wait_for_windows(m, 1)

    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "Ctrl+Space did not open the launcher")
    m.type_text("wm_crash")
    m.sendkey("ret")

    # The taskbar is desktop_shell's window, so "the desktop is painted"
    # is a direct read of init having rebuilt the session - the panel
    # cannot be there unless a compositor is compositing it.
    wait_for(m, lambda s: desktop_is_painted(s),
             "the desktop never came back after the compositor was killed",
             timeout=40.0)

    # And the app is still there, with a taskbar button of its own - which
    # needs the Clock process to have reconnected, been given a new
    # window, and repainted it.
    wait_for(m, lambda s: count_app_windows(s) == 1,
             "the app that was open before the crash did not come back",
             timeout=30.0)

    # Still a working desktop afterwards, not just a picture of one.
    m.double_click(ICON_X, ICONS[5][2])  # Paint
    wait_for_windows(m, 2, timeout=20.0)


def _settled_row(machine, sample, tries=8):
    """The value of `sample` once two consecutive screenshots agree.

    Every pixel baseline in the M56 tests is a *whole row*, captured to be
    compared against later - so a baseline snatched mid-repaint is one
    nothing can ever match again, and the failure looks exactly like the
    feature being broken. Polling until it stops moving is the honest fix;
    a fixed sleep would be a guess that gets worse on a loaded host."""
    last = sample(machine.screenshot())
    for _ in range(tries):
        time.sleep(0.4)
        now = sample(machine.screenshot())
        if now == last:
            return now
        last = now
    return last


def test_editor_undo_restores_the_buffer(m):
    """M56's undo, end to end from the keyboard: type, paste, undo, and
    require the window to look exactly as it did before the paste.

    "Exactly" is the point. A paste that silently did nothing, or an undo
    that removed one character of six, would both satisfy "the text got
    shorter" - so this compares the *whole* first text row pixel for
    pixel, before and after, and requires the two to be identical.

    The clipboard is loaded from the terminal rather than by selecting
    text in the editor: gui_terminal.c's Ctrl+C with nothing selected
    copies the current input line (M32), which is one keystroke instead
    of a drag whose exact pixel path this test would then also be
    asserting on."""
    boot(m)
    m.double_click(ICON_X, ICONS[0][2])  # Terminal
    wait_for_windows(m, 1)
    m.type_text("HELLO")

    time.sleep(0.5)
    m.sendkey("ctrl-c")   # clipboard := the input line
    time.sleep(0.5)

    m.double_click(ICON_X, ICONS[1][2])  # Editor - takes focus, lands at slot 3
    wait_for_windows(m, 2)
    ex, ey = app_origin(FIRST_APP_IDX + 1)

    def first_row(shot):
        return [shot.px(px, py)
                for py in range(ey + ED_CONTENT_Y0, ey + ED_CONTENT_Y0 + 16)
                for px in range(ex, ex + 200)]

    m.type_text("AB")
    wait_for(m, lambda s: any(p == ED_TEXT_COLOR for p in first_row(s)),
             "nothing was typed into the editor")
    # ...and then wait for the row to stop changing before capturing it.
    # "Something is drawn" fires as soon as the *first* character lands,
    # and a baseline taken between the A and the B is one the undo below
    # can never get back to - which is exactly how this test first
    # failed, on a genuinely correct undo.
    before = _settled_row(m, first_row)

    m.sendkey("ctrl-v")
    wait_for(m, lambda s: first_row(s) != before,
             "Ctrl+V pasted nothing - the editor had no paste at all before M56")

    m.sendkey("ctrl-z")
    wait_for(m, lambda s: first_row(s) == before,
             "one undo did not put the buffer back exactly as it was before the paste")


def test_terminal_scrollback_scrolls_with_the_wheel(m):
    """M56's scrollback, through the wheel M49 gave this window and which
    until now had nothing to reveal here - grid_scroll discarded the top
    line outright, which is why a command whose output was longer than the
    window was a command whose output you could not read.

    `ls /bin` is more lines than the 21-row grid holds, so the top of that
    output has necessarily scrolled off. Compared as whole-row pixels, so
    this asserts on "a different line is at the top" without depending on
    which filename leanfs happens to return first."""
    boot(m)
    m.double_click(ICON_X, ICONS[0][2])  # Terminal
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

    # The wheel acts on whatever the pointer is over, so it has to be over
    # this window - which is also the only way a person would do it.
    m.move_to(tx + TERM_W // 2, ty + TERM_H // 2)

    # QEMU's dz sign is the emulator's convention, not the guest's, so
    # which way is "back" is discovered rather than assumed - the same
    # note the file manager's own wheel test carries. Discovering it once
    # and then reversing *exactly* it is what makes the return trip a
    # real assertion instead of a guess about magnitudes.
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
    # And past the end: the view is clamped at live, not scrolled into
    # whatever is after the last line.
    m.wheel(-back)
    wait_for(m, lambda s: top_row(s) == before,
             "scrolling forward past the live view moved it")


def test_copying_a_file_shows_up_in_another_window(m):
    """M56's file operations, and the reason the file manager refreshes on
    a timer at all: two windows onto the same directory must agree about
    what is in it.

    Copy rather than rename or delete, deliberately - it is the one of the
    three that *adds* a row, so the second window's list growing is a
    positive assertion rather than "something is missing now", and it
    leaves the disk in a state the next test can still boot from."""
    boot(m)
    m.double_click(ICON_X, ICONS[2][2])  # Files
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[2][2])  # Files again - a second window on /home
    wait_for_windows(m, 2)

    ax, ay = app_origin(FIRST_APP_IDX)      # the first window, now behind
    bx, by = app_origin(FIRST_APP_IDX + 1)  # the second, focused

    before = fm_rows_with_text(m.screenshot(), bx, by)
    check(before > 0, "the second Files window listed nothing")

    # Select the first real row (row 0 is ".."), then C to copy.
    m.click(*fm_row_point(bx, by, FM_FIRST_FILE_ROW))
    time.sleep(0.4)
    m.sendkey("c")
    time.sleep(0.6)
    m.type_text("m56copy")
    m.sendkey("ret")

    wait_for(m, lambda s: fm_rows_with_text(s, bx, by) == before + 1,
             "the copy did not appear in the window that made it", timeout=15.0)
    # The other window has its own listing on its own timer - it is only
    # partly visible behind this one, so raising it is what makes its
    # rows readable.
    m.click(ax + 20, ay - TITLEBAR_H // 2)
    wait_for(m, lambda s: fm_rows_with_text(s, ax, ay) == before + 1,
             "the other window's listing never caught up with the new file", timeout=15.0)


def test_soak_desktop_stays_usable(m):
    """M50's soak: leave the desktop up with everything that ticks on a
    timer running, then require the machine to still work.

    The three timer-driven loops that allocate are all live here - the
    clock redrawing every second, the taskbar refreshing every 300ms (a
    WM_QUERY_PIPE round trip each time), and the file manager re-reading
    the whole namespace once a second. Between them that is thousands of
    pipe round trips and redraws over the soak.

    What "nothing grew" is checked as, deliberately: at the end, launch
    one more app and require it to open. That is not a proxy for a leak,
    it is the *symptom* a leak in any of these caps actually produces -
    exhausting MAX_TASKS, MAX_FDS, MAX_SHM_SEGMENTS or the window table
    all present as "the next thing you launch silently doesn't", which is
    precisely how M40's bug and M48's own MAX_TASKS bug both showed up.
    The compositor also refuses windows out loud now (M40's klog line,
    M48's toast), so the log is checked for a refusal too."""
    boot(m)
    m.double_click(ICON_X, ICONS[4][2])  # Clock: redraws every second
    wait_for_windows(m, 1)
    m.double_click(ICON_X, ICONS[2][2])  # Files: re-lists the namespace every second
    wait_for_windows(m, 2)

    # Long enough for hundreds of taskbar refreshes and file-list reloads.
    # A fixed wait is right here: the assertion is that nothing degraded,
    # and there is no state to poll toward.
    soak_seconds = float(os.environ.get("LEANOS_SOAK_SECONDS", "180"))
    deadline = time.time() + soak_seconds
    while time.time() < deadline:
        time.sleep(5.0)
        # Keep the pointer moving over the taskbar so its hover path and
        # the compositor's partial-redraw path are exercised too, not just
        # the timers.
        m.move_to(*slot_click(0))

    check(count_app_windows(m.screenshot()) == 2,
          "a window disappeared during the soak")

    m.double_click(ICON_X, ICONS[0][2])  # Terminal
    wait_for_windows(m, 3, timeout=20.0)

    refused = refusals(m)
    check(not refused,
          "the compositor refused %d window request(s) during the soak: %s"
          % (len(refused), refused))


def _bar_spans(shot):
    """The taskbar reaching both edges of whatever screen this is - and
    genuinely being a bar, not bare desktop, which is uniform too. Read at
    the panel's own top margin strip (two rows below its top edge, above
    the button row), so both probes land on plain panel fill whatever
    happens to be running."""
    y = shot.height - 30
    left = shot.px(2, y)
    right = shot.px(shot.width - 3, y)
    above = shot.px(shot.width // 2, shot.height // 2)
    return left == right and left != above


def test_display_resolution_changes_and_persists(m):
    """M58, driven the way a person drives it: open Settings, click a
    resolution, click Keep, and see the desktop actually be that size.

    The screenshot's own dimensions are the assertion. Everything else in
    this file grades colors at coordinates; this is the one test where the
    *size of the framebuffer QEMU dumps* is the thing under test, and it
    is unfakeable - it comes from the display device, not from anything
    the guest tells us.

    Keeping it matters as much as changing it: nothing is written to disk
    until the mode is confirmed, so this also proves the confirm path
    reaches settings_file_save_display."""
    boot(m)
    m.double_click(ICON_X, ICONS[3][2])  # Settings
    wait_for_windows(m, 1)

    sx, sy = app_origin(FIRST_APP_IDX, SETTINGS_H)
    m.click(*mode_btn_center(sx, sy, SMALL_MODE_INDEX))
    shot = wait_for(m, lambda s: (s.width, s.height) == SMALL_MODE,
                    "clicking a resolution did not change the display size")
    check(_bar_spans(shot),
          "the taskbar does not span the new %dx%d display - its buffer was not reallocated"
          % SMALL_MODE)

    # The Settings window was clamped back on screen by the change (a
    # 632-tall window does not fit under a 600-row display), so the Keep
    # button is wherever the clamp put it: hard against the top limit.
    kx = sx + KEEP_BTN_X + KEEP_BTN_W // 2
    ky = CONTENT_TOP + CONFIRM_Y + CONFIRM_H // 2
    m.click(kx, ky)

    # Well past the revert deadline. If Keep did not land, this is where
    # the desktop snaps back and the check below fails.
    time.sleep(MODE_REVERT_S)
    shot = m.screenshot()
    check((shot.width, shot.height) == SMALL_MODE,
          "the confirmed resolution did not stick - the display is %dx%d again"
          % (shot.width, shot.height))
    check(_bar_spans(shot), "the taskbar stopped spanning the display after the mode was kept")


def test_display_resolution_reverts_when_not_confirmed(m):
    """The path that only ever runs when something has already gone
    wrong, which is exactly why it is worth a test: pick a resolution,
    confirm nothing, and the desktop comes back at the old size on its
    own.

    This is the whole reason a person can try a resolution on this
    machine at all. There is no second machine to log in from and no
    config file to edit blind - if a mode the display cannot show were
    permanent, the honest thing would have been not to ship the
    feature."""
    boot(m)
    before = m.screenshot()
    original = (before.width, before.height)
    check(original != SMALL_MODE, "the desktop already boots at the mode this test switches to")

    m.double_click(ICON_X, ICONS[3][2])  # Settings
    wait_for_windows(m, 1)
    sx, sy = app_origin(FIRST_APP_IDX, SETTINGS_H)
    m.click(*mode_btn_center(sx, sy, SMALL_MODE_INDEX))
    wait_for(m, lambda s: (s.width, s.height) == SMALL_MODE,
             "clicking a resolution did not change the display size")

    # Nothing confirms. Poll rather than sleep-then-look, so a revert that
    # happens late still passes and one that never happens fails with the
    # size it was stuck at.
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
    """M61's animation switch and M62's volume, and the half of both that
    matters: that they are *settings* rather than toggles that forget.

    Motion that cannot be disabled is an accessibility problem rather
    than a preference, and a switch that resets on every boot is not much
    better than none - so this restarts the machine the same way
    settings_persist_across_a_reboot does and reads the button back.

    The assertion is the button's own fill, not its label: the fill is
    two flat colours settings.c names, which a screenshot can compare
    exactly, where a label needs the text to be found first."""
    boot(m)
    m.double_click(ICON_X, ICONS[3][2])  # Settings
    wait_for_windows(m, 1)

    sx, sy = app_origin(FIRST_APP_IDX, SETTINGS_H)
    probe = motion_probe(sx, sy)
    shot = wait_for(m, lambda s: s.px(*probe) == SETTINGS_BTN_ON,
                    "Settings did not open with animations on, which is the default")

    m.click(*motion_click(sx, sy))
    wait_for(m, lambda s: s.px(*probe) == SETTINGS_BTN_OFF,
             "clicking the Motion switch did not turn animations off")

    # M62: and mute, which is volume step 0. Muting is the one audio
    # setting a screenshot can check, and it is also the one that
    # matters most - a machine that forgets it was muted is a machine
    # that beeps at somebody who asked it not to.
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
    deadline = time.time() + 120
    while time.time() < deadline and m.read_log().count(BOOT_MARKER) <= boots_before:
        time.sleep(1.0)
    check(m.read_log().count(BOOT_MARKER) > boots_before, "the machine never restarted")

    # M74: Settings is not reopened here, because the session brings it
    # back by itself - it was on screen when the machine was restarted,
    # so it is on screen again, at the same coordinates the probe above
    # was computed from. Double-clicking the icon as this test used to
    # would open a *second* Settings window on top of the restored one.
    # That the window is here at all is a second assertion this test now
    # makes for free.
    boot(m)
    shot = wait_for(m, lambda s: count_app_windows(s) >= 1 and
                                 s.px(*probe) in (SETTINGS_BTN_ON, SETTINGS_BTN_OFF),
                    "Settings did not come back with a readable Motion switch",
                    timeout=60.0)
    check(shot.px(*probe) == SETTINGS_BTN_OFF,
          "the Motion switch forgot it had been turned off across a restart")
    check(shot.px(*volume_probe(sx, sy, 0)) == SETTINGS_BTN_ON,
          "the volume forgot it had been muted across a restart")


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
    ("desktop_survives_losing_the_compositor", test_desktop_survives_losing_the_compositor),
    ("editor_undo_restores_the_buffer", test_editor_undo_restores_the_buffer),
    ("terminal_scrollback_scrolls_with_the_wheel", test_terminal_scrollback_scrolls_with_the_wheel),
    ("copying_a_file_shows_up_in_another_window", test_copying_a_file_shows_up_in_another_window),
    ("soak_desktop_stays_usable", test_soak_desktop_stays_usable),
    ("launch_close_stress", test_launch_close_stress),
]


def _die_on_signal(signum, _frame):
    """Turn a TERM/INT into an ordinary exception, so the `with Machine()`
    below unwinds and kills its QEMU child.

    Learned the expensive way. Interrupting this suite used to leave its
    guest running - Python dies on SIGTERM without unwinding, and QEMU is
    a separate process that nothing then reaps. Three such orphans
    accumulated over an afternoon of interrupted runs, each burning a
    quarter of a core, and the symptom was boots timing out at 240s in
    tests that pass in 70 on an idle machine. That reads exactly like a
    guest that has hung, which is the worst thing it could have looked
    like: it sent a real debugging session after a kernel bug that was
    never there."""
    raise KeyboardInterrupt("received signal %d" % signum)


# ---------------------------------------------------------------------
# Running them
# ---------------------------------------------------------------------

# The subset that covers the most ground per minute: one test each for
# launching, closing, focus and z-order, the two apps people live in, the
# filesystem, the settings that persist, and crash recovery. Meant as a
# pre-commit tier - it is not a substitute for the full run and is not
# supposed to be, but "did I break the desktop" is a question worth being
# able to ask in five minutes rather than an hour.
QUICK_TESTS = [
    "double_click_launches_every_icon",
    "titlebar_close_button",
    "clicking_a_window_raises_it",
    "editor_undo_restores_the_buffer",
    "terminal_scrollback_scrolls_with_the_wheel",
    "file_manager_navigates_directories",
    "settings_persist_across_a_reboot",
    "a_crashing_program_only_takes_itself_down",
]


def default_jobs():
    """How many guests to run at once.

    Each one is a whole QEMU with a core's worth of work in it, so this is
    about cores rather than about tests. A third of them, capped at four:
    the cap is not arithmetic but experience - the failures this suite
    reports most often on a loaded machine are *boot timeouts*, which are
    the harness giving up rather than the desktop being wrong, and every
    extra parallel guest makes one more likely."""
    try:
        n = os.cpu_count() or 2
    except Exception:
        n = 2
    return max(1, min(4, n // 3))


def run_one(name, fn, boot_timeout):
    """One test, in its own guest, with its output collected rather than
    printed - parallel tests interleaving their lines would make the
    result unreadable, so each one's report is emitted whole by the
    caller."""
    _current.name = name
    started = time.time()
    try:
        with Machine(boot_timeout=boot_timeout) as m:
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


def usage():
    print("usage: qemu_input_suite.py [--jobs N] [--quick] [test ...]")
    print()
    print("  --jobs N   run N guests at once (default: %d here)" % default_jobs())
    print("  --quick    the pre-commit subset (%d tests)" % len(QUICK_TESTS))
    print()
    print("known tests:")
    for n, _ in TESTS:
        print("  %s" % n)


def main(argv):
    signal.signal(signal.SIGTERM, _die_on_signal)
    signal.signal(signal.SIGINT, _die_on_signal)

    jobs = int(os.environ.get("LEANOS_INPUT_JOBS", "0")) or default_jobs()
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
    # Every parallel guest is competing for the same cores, so a boot that
    # takes 110 seconds alone can take three times that with four of them
    # running - and a boot timeout is the harness giving up, not a
    # verdict. The allowance grows with the job count for exactly that
    # reason.
    boot_timeout = 300 + 90 * (jobs - 1)

    print("running %d test(s), %d at a time" % (len(selected), jobs), flush=True)
    started_all = time.time()
    results = []

    if jobs == 1:
        for name, fn in selected:
            print("== %s" % name, flush=True)
            r = run_one(name, fn, boot_timeout)
            print(r[2], flush=True)
            results.append(r)
    else:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = [(n, pool.submit(run_one, n, f, boot_timeout)) for n, f in selected]
            for name, fut in futures:
                r = fut.result()
                print("== %s" % name, flush=True)
                print(r[2], flush=True)
                results.append(r)

    failures = [(n, d) for n, d, _ in results if d is not None]
    print()
    print("%d test(s) in %.0fs" % (len(selected), time.time() - started_all))
    if failures:
        print("FAIL: %d/%d interactive test(s) failed:" % (len(failures), len(selected)))
        for name, detail in failures:
            print("  - %s: %s" % (name, detail))
        return 1
    print("PASS: %d/%d interactive tests passed." % (len(selected), len(selected)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
