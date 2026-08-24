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
import sys
import time
import traceback

from qemu_input import BOOT_MARKER, Machine

# Where a failing test drops the screenshot it gave up on. A pixel
# assertion that fails without showing you the pixels is a bad trade when
# saving them costs one file copy.
ARTIFACT_DIR = os.environ.get("LEANOS_INPUT_ARTIFACTS", "/tmp/leanos-input-failures")
CURRENT_TEST = "unknown"

# ---------------------------------------------------------------------
# Geometry and colors, all mirrored from the source that draws them. Kept
# as named constants rather than inline magic numbers so a layout change
# in a .c file shows up here as a one-line diff to reconcile, the same
# convention tools/qemu-serial-test.sh's REQUIRED_MARKERS uses for klog
# strings.
# ---------------------------------------------------------------------

# desktop_icons.c: ICON_MARGIN 32, ICON_CELL_H 90, ICON_SIZE 48. Seven
# icons (M45 added Tasks) in one column at 1024x768 - the column wraps at
# max_y 660, and the seventh sits at 572, so they still all fit in one.
# Coordinates are icon-box centers.
ICON_X = 56
ICONS = [
    ("Terminal", "gui_terminal", 56),
    ("Editor", "text_editor", 146),
    ("Files", "file_manager", 236),
    ("Settings", "settings", 326),
    ("Clock", "gui_clock", 416),
    ("Paint", "gui_paint", 506),
    ("Tasks", "task_manager", 596),
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
EDITOR_MENU_ROW_H = 16       # FONT_HEIGHT
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
TRAY_W = 92

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

# settings.c's wallpaper row.
WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H = 270, 68, 22

# file_manager.c: WIN_W/WIN_H, its header, and one row.
FM_W, FM_H = 280, 360
FM_HEADER_H = 24
FM_ROW_H = 20   # FONT_HEIGHT + 4
FM_LIST_W = FM_W - 8  # minus SCROLLBAR_W

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


def app_origin(slot):
    return (100 + slot * 40, 100 + slot * 40)


class Failure(Exception):
    pass


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


def boot(machine, timeout=90):
    machine.boot_to_desktop(settle=0.0, timeout=timeout)
    deadline = time.time() + 30
    while time.time() < deadline:
        if desktop_is_painted(machine.screenshot()):
            return
        time.sleep(0.5)
    raise Failure("the desktop never finished painting after boot")


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
                     save_failure_shot(machine, CURRENT_TEST)))


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
    """M40's proving ground: double-click all six desktop icons and
    require six real windows. Before M40 this stopped at two - the third
    icon onward silently got window_id = -1 because the compositor's fd
    table was already full of pipes it had inherited from the kernel's own
    boot self-tests. Which two icons "didn't work" depended purely on
    launch order, which is why the bug got reported as being about the
    Editor and the Clock specifically."""
    boot(m)
    for i, (name, _program, y) in enumerate(ICONS):
        m.double_click(ICON_X, y)
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
    rounds = 5
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
    # The scrollbar is what says "done" precisely. gfx_draw_scrollbar puts
    # the thumb's bottom edge flush with the track's only when scroll_top
    # is at its maximum, which happens exactly when the selection has
    # reached the last row - so this pixel is a direct read of the
    # precondition this test needs, not a proxy for elapsed time.
    sb = (tx + TASKS_W - 4, ty + LIST_Y_IN_WIN + LIST_H_IN_WIN - 2)
    wait_for(m, lambda s: s.px(*sb) == SCROLLBAR_THUMB,
             "the task list never scrolled to its last row", timeout=25.0)

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
    sx, sy = app_origin(FIRST_APP_IDX)
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
    # which takes about as long as the original boot did.
    deadline = time.time() + 200
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


def test_missing_program_raises_a_toast(m):
    """M48, and M40's exact symptom finally given a voice: a desktop icon
    whose program isn't on disk used to do *nothing at all* - which is
    what let an fd-table exhaustion bug hide for four milestones.

    The icon is real and its program is deliberately not: the test
    removes nothing (there is no unlink in this OS) and instead uses the
    launcher to spawn a name that was never a program. That reaches the
    same failure through the same SYS_spawn, and the launcher is one of
    the two places M48 gave a voice to."""
    boot(m)
    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "Ctrl+Space did not open the launcher")

    # "m33test" is an ordinary text file the boot self-tests leave on
    # disk, so it is genuinely in the launcher's list and genuinely not a
    # program - SPAWN_ERR_BAD_IMAGE rather than a name that matches
    # nothing at all.
    m.type_text("m33test")
    m.sendkey("ret")

    probe = toast_stripe_probe(0)
    wait_for(m, lambda s: s.px(*probe) == TOAST_ERROR_C,
             "launching a non-program raised no toast - the failure is still silent")

    # And it goes away on its own, without anyone touching it.
    wait_for(m, lambda s: s.px(*probe) != TOAST_ERROR_C,
             "the toast never expired on its own deadline", timeout=12.0)


def test_clicking_a_toast_dismisses_it(m):
    """A toast is dismissable early, and the click is consumed rather
    than falling through to whatever is underneath it - which matters
    because a toast lands in the top-right corner, exactly where a
    maximized window's close button is."""
    boot(m)
    m.sendkey("ctrl-spc")
    wait_for(m, lambda s: s.px(*LAUNCHER_LOWER_PROBE) == LAUNCHER_LOWER_BG,
             "Ctrl+Space did not open the launcher")
    m.type_text("m33test")
    m.sendkey("ret")

    probe = toast_stripe_probe(0)
    wait_for(m, lambda s: s.px(*probe) == TOAST_ERROR_C, "no toast was raised")

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
    row0_y = y + FM_HEADER_H

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
    m.press(x + 60, y + FM_HEADER_H + FM_ROW_H // 2)
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
TASKS_TITLEBAR_CLICK = (FILES_ORIGIN[0] + FM_W + 60, TASKS_ORIGIN[1] - TITLEBAR_H // 2)
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
    ("missing_program_raises_a_toast", test_missing_program_raises_a_toast),
    ("clicking_a_toast_dismisses_it", test_clicking_a_toast_dismisses_it),
    ("wheel_scrolls_the_file_list_one_row_per_detent", test_wheel_scrolls_the_file_list_one_row_per_detent),
    ("alt_f4_closes_the_focused_window", test_alt_f4_closes_the_focused_window),
    ("ctrl_alt_arrows_snap_and_maximize", test_ctrl_alt_arrows_snap_and_maximize),
    ("drag_a_file_onto_the_desktop_opens_it", test_drag_a_file_onto_the_desktop_opens_it),
    ("clicking_a_window_raises_it", test_clicking_a_window_raises_it),
    ("overlap_click_reaches_the_front_window", test_overlap_click_reaches_the_front_window),
    ("alt_tab_visits_windows_in_use_order", test_alt_tab_visits_windows_in_use_order),
    ("a_crashing_program_only_takes_itself_down", test_a_crashing_program_only_takes_itself_down),
    ("soak_desktop_stays_usable", test_soak_desktop_stays_usable),
    ("launch_close_stress", test_launch_close_stress),
]


def main(argv):
    wanted = argv[1:]
    selected = [(n, f) for n, f in TESTS if not wanted or n in wanted]
    if not selected:
        print("no test matched %r; known tests: %s"
              % (wanted, ", ".join(n for n, _ in TESTS)))
        return 2

    failures = []
    global CURRENT_TEST
    for name, fn in selected:
        CURRENT_TEST = name
        print("== %s" % name, flush=True)
        started = time.time()
        try:
            with Machine() as m:
                fn(m)
        except Failure as exc:
            failures.append((name, str(exc)))
            print("   FAIL (%.0fs): %s" % (time.time() - started, exc), flush=True)
        except Exception:  # a harness/QEMU problem, not a guest verdict
            failures.append((name, "harness error:\n" + traceback.format_exc()))
            print("   ERROR (%.0fs):\n%s" % (time.time() - started,
                                             traceback.format_exc()), flush=True)
        else:
            print("   pass (%.0fs)" % (time.time() - started), flush=True)

    print()
    if failures:
        print("FAIL: %d/%d interactive test(s) failed:" % (len(failures), len(selected)))
        for name, detail in failures:
            print("  - %s: %s" % (name, detail))
        return 1
    print("PASS: %d/%d interactive tests passed." % (len(selected), len(selected)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
