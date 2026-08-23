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

# desktop_icons.c: ICON_MARGIN 32, ICON_CELL_H 90, ICON_SIZE 48. Six icons
# in one column at 1024x768. Coordinates are icon-box centers.
ICON_X = 56
ICONS = [
    ("Terminal", "gui_terminal", 56),
    ("Editor", "text_editor", 146),
    ("Files", "file_manager", 236),
    ("Settings", "settings", 326),
    ("Clock", "gui_clock", 416),
    ("Paint", "gui_paint", 506),
]

DESKTOP_BG = 0x203040        # desktop_icons.c BG_COLOR
ICON_BOX = 0x4C99E6          # desktop_icons.c ICON_BOX_COLOR
CTX_MENU_BG = 0x243040       # desktop_icons.c CTX_MENU_BG
EMPTY_DESKTOP = (500, 500)   # far from every icon and every cascaded window

# desktop_shell.c: PANEL_HEIGHT 32 at the screen bottom, taskbar slots
# SLOT_W 96 wide with SLOT_MARGIN 4 between them, SLOT_H 24 tall starting
# SLOT_MARGIN down from the panel's top edge. Reading the taskbar rather
# than hunting for window pixels on the desktop is deliberate: the panel
# is always topmost (compositor.c's z-order rule), so a slot can never be
# covered by the very windows it's reporting - which a probe placed on the
# desktop absolutely can be, by the next window in the cascade.
PANEL_BG = 0x181828
SLOT_BG = 0x263447           # RUNNING_SLOT_BG - running, unfocused
SLOT_FOCUS_BG = 0x2E4A63     # RUNNING_SLOT_FOCUS_BG
SLOT_MIN_BG = 0x352A20       # RUNNING_SLOT_MIN_BG - minimized
SLOT_COLORS = (SLOT_BG, SLOT_FOCUS_BG, SLOT_MIN_BG)

PANEL_TOP = 768 - 32
SLOT_MARGIN = 4
SLOT_W = 96


def slot_probe(i):
    """Center of taskbar slot i - inside the slot's fill, clear of its
    1px border and of the label text drawn near its left edge."""
    return (SLOT_MARGIN + i * (SLOT_W + SLOT_MARGIN) + SLOT_W - 8,
            PANEL_TOP + SLOT_MARGIN + 12)


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


# compositor.c: an ordinary window is placed at (100 + idx*40, 100 + idx*40).
# At boot the desktop background takes slot 0 and the panel slot 1, so the
# first app launched lands at slot 2.
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
    """The desktop background, its first icon, and the panel all actually
    on screen - i.e. all three of the boot clients have connected *and*
    drawn their first frame. Waiting for this instead of a fixed sleep is
    what makes "click something immediately after boot" reliable: the
    serial log's [init] marker fires well before any of these pixels
    exist, and a click that lands in that gap goes nowhere at all."""
    return (shot.px(*EMPTY_DESKTOP) == DESKTOP_BG and
            shot.px(76, 76) == ICON_BOX and
            shot.px(512, PANEL_TOP + 2) == PANEL_BG)


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
    check(before.px(x + 4, y - 8) != DESKTOP_BG,
          "Clock did not appear at the expected cascade position")

    m.drag(x + 100, y - 8, x + 300, y + 120)
    after = wait_for(m, lambda s: s.px(x + 4, y - 8) == DESKTOP_BG,
                     "window still at its original position after a titlebar drag")
    check(after.px(x + 204, y + 120) != DESKTOP_BG,
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


TESTS = [
    ("double_click_launches_every_icon", test_double_click_launches_every_icon),
    ("single_click_does_not_launch", test_single_click_does_not_launch),
    ("titlebar_close_button", test_titlebar_close_button),
    ("titlebar_drag_moves_window", test_titlebar_drag_moves_window),
    ("alt_tab_cycles_focus", test_alt_tab_cycles_focus),
    ("desktop_context_menu", test_desktop_context_menu),
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
