#!/usr/bin/env python3
"""tools/qemu_input.py - M40's real interactive-input harness engine.

Every click/drag/double-click feature this project has shipped since M18
has only ever been proven by a self-test *calling the action directly*
(`apply_window_action()` / a hand-written `WM_ACTION_PIPE` request).
Nothing has ever driven a real mouse to a real pixel coordinate and
pressed a real button, which is exactly the gap the M40 section of
milestones.md opens on: a bug (double-clicking the Editor/Clock desktop
icons doing nothing) that hid behind it for several milestones because
no test could see it.

This module closes that gap. It drives a booted `build/os-image.bin`
through QEMU's own HMP monitor - `sendkey`, `mouse_move`, `mouse_button`
- and reads results back out of real framebuffer pixels via `screendump`
(a P6 PPM of exactly what the display device holds). No guest-side
cooperation of any kind: the guest cannot tell these events from a
human's, because at the PS/2 controller they *are* the same events.

Deliberately a Python module rather than another bash script (the shape
tools/qemu-serial-test.sh has): pixel readback means parsing a binary
PPM and comparing 32-bit colors, and a monitor conversation means
speaking to a Unix socket with timeouts - both things bash can only do
badly. tools/qemu-input-test.sh is the thin `run the suite` wrapper that
keeps this reachable the same way every other tool here is.

Two facts about QEMU that shape the API below, both learned the hard way
rather than assumed:

  * The guest's mouse is a *relative* PS/2 device (kernel/drivers/
    mouse.c), so `mouse_move` takes deltas, not absolute coordinates.
    QEMU's ps2 model additionally clamps each packet's delta to +/-127
    and carries the remainder over to the next one, so a single large
    move silently arrives short. `move_to` therefore homes the pointer
    into the top-left corner (both the kernel's cursor.c and the
    compositor's own cursor_x/y clamp there, so over-shooting is the
    reliable way to reach a known origin) and then steps to the target
    in <= MAX_STEP chunks.

  * A HMP monitor echoes every character back, plus readline escape
    sequences. Nothing here parses that echo for meaning - commands are
    fire-and-forget and results are read from pixels or the serial log,
    which are the only two things that actually prove guest behavior.
"""

import os
import shutil
import socket
import subprocess
import tempfile
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The image to boot. Overridable so a long suite run can be pointed at a
# *frozen copy* while development carries on in the same tree - a `make`
# rewrites build/os-image.bin under any guest still reading it, which
# silently invalidates every test that has not started yet. Snapshot the
# image, export LEANOS_IMAGE, and the two are independent.
IMAGE = os.environ.get("LEANOS_IMAGE") or os.path.join(REPO_ROOT, "build", "os-image.bin")
# Where a failure's evidence is kept once the guest that produced it is
# gone - the same directory tools/qemu_input_suite.py saves screendumps
# into, since both answer the same question.
ARTIFACT_DIR = os.environ.get("LEANOS_INPUT_ARTIFACTS", "/tmp/leanos-input-failures")
OVMF_CODE = os.path.join(REPO_ROOT, "build", "ovmf", "OVMF_CODE.fd")
OVMF_VARS = os.path.join(REPO_ROOT, "build", "ovmf", "OVMF_VARS.fd")

# The "desktop is up and steady" marker, same literal-substring convention
# tools/qemu-serial-test.sh's REQUIRED_MARKERS uses - a wording change in
# kernel/kernel.c is a visible one-line diff here too.
BOOT_MARKER = "[init] PID 1 spawned"

# Per-`mouse_move` delta cap. QEMU's own ps2 packet clamp is 127; staying
# under it means one command == one fully-delivered packet, so a caller
# counting steps can reason about where the pointer ended up.
MAX_STEP = 100

# How far past the screen's own dimensions `home()` pushes. Anything
# >= the display size works; the clamp does the rest.
HOME_OVERSHOOT = 1200

# The pointer the compositor draws (user_space/bin/compositor.c's
# cursor_shape / CURSOR_COLOR / CURSOR_SIZE), as the list of pixels that
# are actually part of the arrow. Used to locate the real pointer in a
# screendump - see Machine.find_cursor.
CURSOR_COLOR = 0xFFFFFF
CURSOR_W = CURSOR_H = 8
_CURSOR_ROWS = (
    0b10000000,
    0b11000000,
    0b10100000,
    0b10010000,
    0b10001000,
    0b10111000,
    0b11000100,
    0b10000100,
)
CURSOR_PIXELS = tuple((x, y)
                      for y, bits in enumerate(_CURSOR_ROWS)
                      for x in range(CURSOR_W)
                      if bits & (0x80 >> x))

# M46: the arrow is no longer the only shape the compositor ever draws.
# M38 already gave it resize cursors and M46 adds a move cursor for the
# titlebar band, which broke move_to outright: its verification finds the
# pointer by matching the arrow's bitmap, so it simply could not locate a
# pointer parked on a window edge or titlebar. Every shape compositor.c
# can select lives here now, mirrored from its own tables (cursor_shape,
# cursor_shape_horizontal/vertical/diag_*/move), and find_cursor tries all
# of them. The name is worth having as well as the position: "which
# cursor is drawn here" is exactly what M46's resize-zone test asks.
_CURSOR_SHAPE_ROWS = {
    "arrow": _CURSOR_ROWS,
    "horizontal": (0b00011000, 0b00111100, 0b01100110, 0b11000011,
                   0b11000011, 0b01100110, 0b00111100, 0b00011000),
    "vertical": (0b00011000, 0b00111100, 0b01111110, 0b00011000,
                 0b00011000, 0b01111110, 0b00111100, 0b00011000),
    "diag_nw_se": (0b11110000, 0b11000000, 0b10000000, 0b00000000,
                   0b00000000, 0b00000001, 0b00000011, 0b00001111),
    "diag_ne_sw": (0b00001111, 0b00000011, 0b00000001, 0b00000000,
                   0b00000000, 0b10000000, 0b11000000, 0b11110000),
    "move": (0b00011000, 0b00111100, 0b01011010, 0b11011011,
             0b11011011, 0b01011010, 0b00111100, 0b00011000),
}

CURSOR_SHAPES = {
    name: tuple((x, y)
                for y, bits in enumerate(rows)
                for x in range(CURSOR_W)
                if bits & (0x80 >> x))
    for name, rows in _CURSOR_SHAPE_ROWS.items()
}


class Ppm:
    """A parsed P6 screendump. px(x, y) returns 0x00RRGGBB, the same
    packed form every color constant in this project's user space is
    written in (gfx.h), so a comparison here reads like the source it's
    checking against."""

    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        pos = 0

        def token():
            nonlocal pos
            while data[pos:pos + 1].isspace():
                pos += 1
            start = pos
            while pos < len(data) and not data[pos:pos + 1].isspace():
                pos += 1
            return data[start:pos]

        magic = token()
        if magic != b"P6":
            raise ValueError("screendump was not a P6 PPM: %r" % magic)
        self.width = int(token())
        self.height = int(token())
        maxval = int(token())
        if maxval != 255:
            raise ValueError("unexpected PPM maxval %d" % maxval)
        pos += 1  # the single whitespace byte before the pixel data
        self._pixels = data[pos:]

    def px(self, x, y):
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError("(%d, %d) outside %dx%d screendump"
                             % (x, y, self.width, self.height))
        off = (y * self.width + x) * 3
        return (self._pixels[off] << 16) | (self._pixels[off + 1] << 8) | self._pixels[off + 2]

    def count_color(self, color, x0, y0, w, h):
        """How many pixels in the given rect are exactly `color`. Used
        instead of single-point probes wherever the thing being checked
        is "did a region change", which tolerates a one-pixel cursor
        overlap that a single probe point would fail on."""
        n = 0
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                if self.px(x, y) == color:
                    n += 1
        return n


class Machine:
    """One booted guest, driven through its HMP monitor."""

    def __init__(self, extra_args=(), quiet=False):
        if not os.path.exists(IMAGE):
            raise RuntimeError("no image at %s - run 'make' first" % IMAGE)
        if not os.path.exists(OVMF_CODE):
            raise RuntimeError("no OVMF at build/ovmf - run tools/build-ovmf.sh")

        # A short base dir on purpose: a Unix socket path has a hard
        # ~104-byte limit, and this project's own scratch/temp paths can
        # already exceed that on their own.
        self._dir = tempfile.mkdtemp(prefix="leanos-input-", dir="/tmp")
        self._mon_path = os.path.join(self._dir, "mon.sock")
        self.log_path = os.path.join(self._dir, "serial.log")
        self._quiet = quiet
        vars_rt = os.path.join(self._dir, "OVMF_VARS.fd")
        shutil.copyfile(OVMF_VARS, vars_rt)

        self._proc = subprocess.Popen([
            "qemu-system-x86_64",
            "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
            "-drive", "if=pflash,format=raw,file=" + vars_rt,
            # snapshot=on: guest writes (leanfs formats the disk on its
            # first boot, so this can't be a read-only device) land in a
            # throwaway overlay and the real image is opened read-only.
            # That means a run holds no write lock on build/os-image.bin -
            # so `make`, tools/qemu-serial-test.sh and a second copy of
            # this suite can all proceed while one is in flight - and
            # every test starts from the same pristine, never-booted image
            # rather than from whatever the previous one left on disk.
            "-drive", "format=raw,snapshot=on,file=" + IMAGE,
            "-display", "none",
            # Same SLIRP NAT tools/run-qemu.sh explains: the boot self-test
            # pings the gateway and panics with no NIC attached at all.
            "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0",
            "-serial", "file:" + self.log_path,
            "-monitor", "unix:" + self._mon_path + ",server,nowait",
        ] + list(extra_args), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        self._sock = None
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(self._mon_path)
                self._sock = s
                break
            except OSError:
                time.sleep(0.2)
        if self._sock is None:
            self.kill()
            raise RuntimeError("QEMU monitor socket never appeared")
        self._drain()

        self._shot_seq = 0
        # Mirrors the compositor's own starting cursor_x/y (fb center).
        # Only ever a hint: home() re-establishes it for real.
        self.cursor = None

    # ---- monitor plumbing -------------------------------------------

    def _drain(self):
        """Non-blocking on purpose. The monitor echoes every keystroke
        back plus readline escapes, and none of it means anything here
        (results are read from pixels and the serial log) - but a
        *blocking* drain would add its own timeout to every single
        command, and input timing is load-bearing: desktop_icons.c's
        DOUBLE_CLICK_MS is 500ms, so a harness that spends a quarter
        second per monitor command can't produce a double-click the
        guest would ever recognize as one. This is the difference
        between the harness measuring the guest and the harness
        measuring itself."""
        self._sock.setblocking(False)
        try:
            while self._sock.recv(65536):
                pass
        except (BlockingIOError, socket.timeout):
            pass
        finally:
            self._sock.setblocking(True)

    def monitor(self, command, settle=0.0):
        self._sock.sendall(command.encode() + b"\n")
        self._drain()
        if settle:
            time.sleep(settle)

    # ---- boot / logs -------------------------------------------------

    def read_log(self):
        try:
            with open(self.log_path, "r", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_for_marker(self, marker=BOOT_MARKER, timeout=90):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if marker in self.read_log():
                return True
            time.sleep(0.5)
        return False

    def boot_to_desktop(self, settle=4.0, timeout=90):
        """Waits for init's handoff, then lets the compositor, the
        desktop background and the panel all connect and paint their
        first frame. `settle` is a real budget measured against the
        boot this project actually does, not a magic number - every
        pixel assertion downstream depends on the first frame being up."""
        if not self.wait_for_marker(timeout=timeout):
            raise RuntimeError("guest never reached %r - see %s"
                               % (BOOT_MARKER, self.save_log("boot-timeout")))
        time.sleep(settle)

    # ---- input -------------------------------------------------------

    def sendkey(self, keys, hold_ms=None):
        """`keys` is QEMU's own key syntax, e.g. "a", "ret", "alt-tab"."""
        if hold_ms is None:
            self.monitor("sendkey %s" % keys)
        else:
            self.monitor("sendkey %s %d" % (keys, hold_ms))

    def type_text(self, text):
        for ch in text:
            self.sendkey(_qemu_keyname(ch), None)
            time.sleep(0.03)

    def home(self):
        """Pins the pointer at (0, 0) by over-shooting the top-left
        clamp. The only way to get a known absolute position out of a
        relative device - see this module's header."""
        steps = (HOME_OVERSHOOT + MAX_STEP - 1) // MAX_STEP
        for _ in range(steps):
            self.monitor("mouse_move -%d -%d" % (MAX_STEP, MAX_STEP), settle=0.02)
        time.sleep(0.15)
        self.cursor = (0, 0)

    def _step_by(self, dx, dy):
        while dx or dy:
            sx = max(-MAX_STEP, min(MAX_STEP, dx))
            sy = max(-MAX_STEP, min(MAX_STEP, dy))
            self.monitor("mouse_move %d %d" % (sx, sy), settle=0.02)
            dx -= sx
            dy -= sy

    def find_cursor_shape(self, shot, near, radius=48):
        """Where the compositor is actually drawing the pointer and which
        of its shapes it is drawing - matched against every bitmap in
        CURSOR_SHAPES inside a window around where we believe it to be.
        Returns (x, y, name) or None.

        Only a shape's own lit pixels are matched, never its gaps - the
        gaps show whatever is underneath, so requiring them to be
        non-white would make this fail over pale content. The flip side is
        that one shape's pixel set can be a subset of another's, so ties
        are broken toward the shape with the most lit pixels: the move
        cursor's 24 pixels contain the vertical resize cursor's 20, and
        answering "vertical" for a move cursor would be wrong in exactly
        the test that cares."""
        cx, cy = near
        best = None
        for oy in range(max(0, cy - radius), min(shot.height - CURSOR_H, cy + radius + 1)):
            for ox in range(max(0, cx - radius), min(shot.width - CURSOR_W, cx + radius + 1)):
                for name, pixels in CURSOR_SHAPES.items():
                    if all(shot.px(ox + px, oy + py) == CURSOR_COLOR for px, py in pixels):
                        # Nearest match to the expected point wins, so a
                        # white glyph elsewhere in the window can't outrank
                        # the real pointer.
                        key = (abs(ox - cx) + abs(oy - cy), -len(pixels))
                        if best is None or key < best[0]:
                            best = (key, ox, oy, name)
        return None if best is None else (best[1], best[2], best[3])

    def find_cursor(self, shot, near, radius=48):
        """find_cursor_shape without the shape name - the position is all
        move_to's verification loop needs."""
        found = self.find_cursor_shape(shot, near, radius)
        return None if found is None else (found[0], found[1])

    def move_to(self, x, y, verify=True):
        """Puts the pointer at exactly (x, y), then checks that it got
        there and corrects if it didn't.

        The checking is not belt-and-braces. The device is relative, the
        emulated PS/2 controller coalesces and clamps deltas, and the
        guest is a software compositor that can be mid-redraw when a
        packet lands - so "I sent moves totalling (x, y)" is genuinely not
        the same claim as "the pointer is at (x, y)". Skipping this
        produced a real, confusing failure: a right-click meant for the
        empty desktop landed 100px short, opened the context menu
        somewhere else entirely, and read as a guest bug rather than a
        harness one. A test suite that can't say where it clicked can't
        blame the guest for what happened."""
        if self.cursor != (0, 0):
            self.home()
        self._step_by(x, y)
        time.sleep(0.15)
        self.cursor = (x, y)
        if not verify:
            return
        for _attempt in range(3):
            found = self.find_cursor(self.screenshot(), (x, y))
            if found == (x, y):
                return
            if found is None:
                self.home()
                self._step_by(x, y)
            else:
                self._step_by(x - found[0], y - found[1])
            time.sleep(0.15)
        raise RuntimeError("could not place the pointer at (%d, %d)" % (x, y))

    # `help mouse_button` in the QEMU monitor describes this bitmask as
    # "1=L, 2=M, 4=R". That help string is wrong - verified against the
    # guest, not assumed: pressing 2 is what reaches kernel/drivers/mouse.c
    # as the right button (packet status bit 1), and 4 as the middle one.
    # QEMU's own MOUSE_EVENT_* constants are ordered L/R/M, and the help
    # text simply doesn't match them. Named constants here so the one
    # place that knows this is the only place that has to.
    BTN_LEFT = 1
    BTN_RIGHT = 2
    BTN_MIDDLE = 4

    def wheel(self, detents):
        """M49: `detents` clicks of the scroll wheel, through QEMU's own
        `mouse_move dx dy dz` third argument - the same emulated
        IntelliMouse packet a real wheel produces, so the guest cannot
        tell this from hardware.

        One monitor command per detent rather than one with a large dz:
        the PS/2 wheel field is a 4-bit signed value, so a big dz is not
        a bigger scroll, it is a wrapped one - and a test that scrolled
        the wrong distance because the harness overflowed a nibble would
        look exactly like a guest bug."""
        step = 1 if detents > 0 else -1
        for _ in range(abs(detents)):
            self.monitor("mouse_move 0 0 %d" % step, settle=0.06)
        time.sleep(0.2)

    def button(self, mask):
        self.monitor("mouse_button %d" % mask, settle=0.03)

    def click(self, x=None, y=None, mask=BTN_LEFT, hold=0.04):
        if x is not None:
            self.move_to(x, y)
        self.button(mask)
        time.sleep(hold)
        self.button(0)
        time.sleep(0.04)

    def right_click(self, x=None, y=None):
        self.click(x, y, mask=self.BTN_RIGHT)

    def double_click(self, x=None, y=None, gap=0.05):
        """Two presses at the same point, well inside desktop_icons.c's
        own DOUBLE_CLICK_MS (500) window as the guest measures it. The
        whole sequence has to fit in that budget in *guest* time, which
        is why `monitor` refuses to block - see _drain."""
        if x is not None:
            self.move_to(x, y)
        self.click()
        time.sleep(gap)
        self.click()

    def press(self, x=None, y=None, mask=BTN_LEFT):
        """Button down, and stay down. Pair with move_held/release for a
        drag a test needs to look at *mid*-gesture (M43's snap preview) -
        drag() below is the whole gesture in one call for the common case
        where only the end state matters."""
        if x is not None:
            self.move_to(x, y)
        self.button(mask)
        time.sleep(0.1)

    def move_held(self, x, y, steps=8):
        """Steps the pointer to (x, y) without move_to's verification.
        Deliberately unverified: verification re-homes the pointer against
        the top-left clamp, which mid-drag would drag the window there
        with it. Stepped rather than one jump so the guest sees real
        intermediate motion - a drag state machine that only ever gets
        press-then-release-far-away isn't being tested at all."""
        x0, y0 = self.cursor
        for i in range(1, steps + 1):
            nx = x0 + (x - x0) * i // steps
            ny = y0 + (y - y0) * i // steps
            self._step_by(nx - self.cursor[0], ny - self.cursor[1])
            self.cursor = (nx, ny)
            time.sleep(0.05)
        time.sleep(0.15)

    def release(self):
        self.button(0)
        time.sleep(0.25)

    def drag(self, x0, y0, x1, y1, steps=8):
        """Press at (x0,y0), move to (x1,y1) with the button held, release."""
        self.press(x0, y0)
        self.move_held(x1, y1, steps=steps)
        self.release()

    # ---- readback ----------------------------------------------------

    def screenshot(self):
        self._shot_seq += 1
        path = os.path.join(self._dir, "shot%03d.ppm" % self._shot_seq)
        self.monitor("screendump %s" % path, settle=0.0)
        # screendump is asynchronous with respect to the monitor echo, so
        # wait for the file to both exist and stop growing rather than
        # guessing a fixed sleep.
        deadline = time.time() + 10
        last = -1
        while time.time() < deadline:
            if os.path.exists(path):
                size = os.path.getsize(path)
                if size > 0 and size == last:
                    return Ppm(path)
                last = size
            time.sleep(0.1)
        raise RuntimeError("screendump never produced %s" % path)

    # ---- lifecycle ---------------------------------------------------

    def wait_for_exit(self, timeout=30.0):
        """M47: how long the guest took to exit on its own, or None if it
        never did.

        Nothing in this harness had ever observed a clean guest exit -
        every test before this one ends by killing QEMU - so "did S5
        actually fire" had no way to be answered from inside the guest or
        out. QEMU's own process terminating is the only real proof: a
        guest that merely halted, or that wrote the wrong port and kept
        running, leaves the process exactly where it was."""
        deadline = time.time() + timeout
        started = time.time()
        while time.time() < deadline:
            if self._proc.poll() is not None:
                return time.time() - started
            time.sleep(0.25)
        return None

    @property
    def exit_status(self):
        """QEMU's exit status, or None if it is still running."""
        return self._proc.poll()

    def kill(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        if self._proc is not None:
            self._proc.kill()
            try:
                self._proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
            self._proc = None

    def __enter__(self):
        return self

    def save_log(self, name):
        """Copies this guest's serial log somewhere it will outlive the
        guest, and returns that path (or the live one if the copy fails).

        A boot that never finishes is the one failure whose *only*
        evidence is this log - there are no pixels to screenshot and no
        assertion that got far enough to say anything - and it used to be
        deleted by __exit__ before anyone could read it. That is how an
        intermittent hang stays intermittent."""
        try:
            os.makedirs(ARTIFACT_DIR, exist_ok=True)
            dest = os.path.join(ARTIFACT_DIR, "%s-%d.log" % (name, os.getpid()))
            shutil.copyfile(self.log_path, dest)
            return dest
        except OSError:
            return self.log_path

    def __exit__(self, *_exc):
        self.kill()
        shutil.rmtree(self._dir, ignore_errors=True)


_SHIFTED = {
    "_": "shift-minus", ":": "shift-semicolon", "?": "shift-slash",
    "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4",
    "%": "shift-5", "^": "shift-6", "&": "shift-7", "*": "shift-8",
    "(": "shift-9", ")": "shift-0", "+": "shift-equal", "\"": "shift-apostrophe",
}
_NAMED = {
    " ": "spc", ".": "dot", ",": "comma", "-": "minus", "=": "equal",
    "/": "slash", ";": "semicolon", "'": "apostrophe", "\n": "ret",
}


def _qemu_keyname(ch):
    if ch in _SHIFTED:
        return _SHIFTED[ch]
    if ch in _NAMED:
        return _NAMED[ch]
    if ch.isupper():
        return "shift-" + ch.lower()
    return ch
