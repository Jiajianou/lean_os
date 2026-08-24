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

from qemu_input import Machine

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


def titlebar_button_center(win_x, win_y, win_w, button):
    x = win_x + win_w - BTN_MARGIN - BTN_SIZE - button * (BTN_SIZE + BTN_GAP)
    y = win_y - TITLEBAR_H + (TITLEBAR_H - BTN_SIZE) // 2
    return (x + BTN_SIZE // 2, y + BTN_SIZE // 2)


CLOCK_W = 200  # gui_clock.c WIN_W
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
