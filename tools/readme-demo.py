#!/usr/bin/env python3
"""A recording of this desktop being used, for the top of README.md.

Boots the image under QEMU, drives it through the input suite's own
machinery - the same monitor socket, the same coordinates, the same waits -
and keeps every screendump taken along the way as a frame of an animated GIF.
A run replaces demo.gif and puts the one line that shows it into README.md if
the line is not already there, so the picture follows the tree rather than the
day somebody last remembered to take one.

The GIF encoder is written out rather than taken from an image library, for
the reason tools/browser-shot.py gives: there is none on a clean Mac, and the
project would rather have its own than an install step. The file's size and
length are held to ceilings and a run over either replaces nothing.
"""

import argparse
import hashlib
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
import zlib

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GIF_NAME = "demo.gif"
GIF_PATH = os.path.join(REPO_ROOT, GIF_NAME)
README_PATH = os.path.join(REPO_ROOT, "README.md")
README_LINE = "![lean_os, used: a wallpaper changed, a terminal, Files, the Start menu and Paint](%s)" % GIF_NAME
README_ANCHOR = "## What it does"

MAX_BYTES = 3 * 1024 * 1024
MAX_SECONDS = 40.0
LONGEST_FRAME = 2.5
GLIDE_FRAME = 0.07
TYPING_FRAME = 0.06
LAST_FRAME_HOLD = 3.0
TRANSPARENT = 255


def lzw_encode(indices, minimum_code_size=8):
    clear = 1 << minimum_code_size
    end = clear + 1
    out = bytearray()
    accumulator = 0
    bits = 0

    def emit(code, width):
        nonlocal accumulator, bits
        accumulator |= code << bits
        bits += width
        while bits >= 8:
            out.append(accumulator & 0xFF)
            accumulator >>= 8
            bits -= 8

    width = minimum_code_size + 1
    table = {}
    next_code = end + 1
    emit(clear, width)
    prefix = -1
    for value in indices:
        if prefix < 0:
            prefix = value
            continue
        key = (prefix << 8) | value
        code = table.get(key)
        if code is not None:
            prefix = code
            continue
        emit(prefix, width)
        if next_code < 4096:
            table[key] = next_code
            if next_code == (1 << width) and width < 12:
                width += 1
            next_code += 1
        else:
            emit(clear, width)
            table = {}
            next_code = end + 1
            width = minimum_code_size + 1
        prefix = value
    if prefix >= 0:
        emit(prefix, width)
    emit(end, width)
    if bits:
        out.append(accumulator & 0xFF)
    blocks = bytearray([minimum_code_size])
    for at in range(0, len(out), 255):
        chunk = out[at:at + 255]
        blocks.append(len(chunk))
        blocks += chunk
    blocks.append(0)
    return bytes(blocks)


def lzw_decode(data, minimum_code_size):
    clear = 1 << minimum_code_size
    end = clear + 1
    out = bytearray()
    accumulator = 0
    bits = 0
    width = minimum_code_size + 1
    table = None
    previous = None
    at = 0
    while True:
        while bits < width:
            if at >= len(data):
                return bytes(out)
            accumulator |= data[at] << bits
            at += 1
            bits += 8
        code = accumulator & ((1 << width) - 1)
        accumulator >>= width
        bits -= width
        if code == clear:
            table = [bytes([i]) for i in range(clear)] + [b"", b""]
            width = minimum_code_size + 1
            previous = None
            continue
        if code == end:
            return bytes(out)
        if previous is None:
            entry = table[code]
        elif code < len(table):
            entry = table[code]
            table.append(previous + entry[:1])
        elif code == len(table):
            entry = previous + previous[:1]
            table.append(entry)
        else:
            raise ValueError("LZW code %d is ahead of a table of %d" % (code, len(table)))
        out += entry
        previous = entry
        if len(table) == (1 << width) and width < 12:
            width += 1


def median_cut(histogram, colours):
    def measured(box):
        weight = sum(n for _, n in box)
        best = None
        for channel in range(3):
            low = min(c[channel] for c, _ in box)
            high = max(c[channel] for c, _ in box)
            if high > low and (best is None or high - low > best[1]):
                best = (channel, high - low)
        if best is None:
            return (-1.0, 0, box)
        return ((best[1]) * weight ** 0.5, best[0], box)

    boxes = [measured(list(histogram.items()))]
    while len(boxes) < colours:
        number = max(range(len(boxes)), key=lambda i: boxes[i][0])
        score, channel, box = boxes[number]
        if score < 0:
            break
        boxes.pop(number)
        box.sort(key=lambda item: item[0][channel])
        total = sum(n for _, n in box)
        running = 0
        cut = 1
        for cut in range(1, len(box)):
            running += box[cut - 1][1]
            if running * 2 >= total:
                break
        boxes.append(measured(box[:cut]))
        boxes.append(measured(box[cut:]))
    palette = []
    lookup = {}
    for index, (_, _, box) in enumerate(boxes):
        weight = sum(n for _, n in box)
        palette.append(tuple(
            (sum(c[channel] * n for c, n in box) + weight // 2) // weight
            for channel in range(3)))
        for colour, _ in box:
            lookup[colour] = index
    return palette, lookup


class Frames:
    """Screens kept as zlib'd RGB, one per change, each with how long it shows.

    A frame of a deliberate pause or of a wait shows for as long as it really
    did. A frame of the pointer moving or of a key being typed shows for a
    fixed moment instead, because a screendump costs a fifth of a second and a
    glide played back at that rate is a slide show of a pointer."""

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.kept = []
        self._last_digest = None

    def add(self, pixels, at, fixed=None):
        digest = hashlib.sha1(pixels).digest()
        if digest == self._last_digest:
            if fixed is not None and self.kept and self.kept[-1][2] is not None:
                self.kept[-1][2] += fixed
            return False
        self._last_digest = digest
        self.kept.append([zlib.compress(pixels, 1), at, fixed])
        return True

    def durations(self, finished_at):
        times = [at for _, at, _ in self.kept] + [finished_at]
        return [fixed if fixed is not None else
                min(max(times[i + 1] - times[i], 0.02), LONGEST_FRAME)
                for i, (_, _, fixed) in enumerate(self.kept)]

    def pixels(self, number):
        return zlib.decompress(self.kept[number][0])


def changed_box(previous, current, width, height):
    stride = width * 3
    rows = [y for y in range(height)
            if previous[y * stride:(y + 1) * stride] != current[y * stride:(y + 1) * stride]]
    if not rows:
        return None
    left, right = width, -1
    for y in rows:
        base = y * stride
        x = 0
        while x < left and previous[base + 3 * x:base + 3 * x + 3] == current[base + 3 * x:base + 3 * x + 3]:
            x += 1
        left = min(left, x)
        x = width - 1
        while x > right and previous[base + 3 * x:base + 3 * x + 3] == current[base + 3 * x:base + 3 * x + 3]:
            x -= 1
        right = max(right, x)
    return left, rows[0], right - left + 1, rows[-1] - rows[0] + 1


def encode_gif(width, height, screens, durations):
    """screens: RGB byte strings, consecutive ones different; durations in seconds."""
    boxes = []
    histogram = {}
    previous = None
    for pixels in screens:
        box = (0, 0, width, height) if previous is None else changed_box(previous, pixels, width, height)
        boxes.append(box)
        if box is not None:
            x0, y0, w, h = box
            for y in range(y0, y0 + h):
                row = pixels[(y * width + x0) * 3:(y * width + x0 + w) * 3]
                for at in range(0, len(row), 3):
                    colour = row[at:at + 3]
                    histogram[colour] = histogram.get(colour, 0) + 1
        previous = pixels

    palette, lookup = median_cut(histogram, TRANSPARENT)
    shown = list(durations)
    for number in range(len(screens) - 1, 0, -1):
        if boxes[number] is None:
            shown[number - 1] += shown[number]
    table = bytearray()
    for colour in palette:
        table += bytes(colour)
    table += bytes(3 * (256 - len(palette)))

    out = bytearray(b"GIF89a")
    out += width.to_bytes(2, "little") + height.to_bytes(2, "little")
    out += bytes([0xF7, 0, 0]) + table
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"

    previous = None
    centiseconds_owed = 0.0
    for number, pixels in enumerate(screens):
        box = boxes[number]
        if box is None:
            continue
        x0, y0, w, h = box
        indices = bytearray(w * h)
        put = 0
        for y in range(y0, y0 + h):
            start = (y * width + x0) * 3
            row = pixels[start:start + w * 3]
            before = previous[start:start + w * 3] if previous is not None else None
            for at in range(0, w * 3, 3):
                colour = row[at:at + 3]
                if before is not None and before[at:at + 3] == colour:
                    indices[put] = TRANSPARENT
                else:
                    indices[put] = lookup[colour]
                put += 1
        centiseconds_owed += shown[number] * 100
        delay = max(2, int(round(centiseconds_owed)))
        centiseconds_owed -= delay
        out += bytes([0x21, 0xF9, 4, 0x05]) + delay.to_bytes(2, "little") + bytes([TRANSPARENT, 0])
        out += b"\x2C" + x0.to_bytes(2, "little") + y0.to_bytes(2, "little")
        out += w.to_bytes(2, "little") + h.to_bytes(2, "little") + b"\x00"
        out += lzw_encode(indices)
        previous = pixels
    out += b"\x3B"
    return bytes(out), palette


def decode_gif(data):
    """Every frame as composed RGB, with its delay - the half that grades the other."""
    if data[:6] != b"GIF89a":
        raise ValueError("not a GIF89a")
    width = int.from_bytes(data[6:8], "little")
    height = int.from_bytes(data[8:10], "little")
    flags = data[10]
    at = 13
    global_table = None
    if flags & 0x80:
        size = 3 << ((flags & 7) + 1)
        global_table = data[at:at + size]
        at += size
    canvas = bytearray(width * height * 3)
    frames = []
    transparent = None
    delay = 0
    disposal = 0
    while True:
        kind = data[at]
        at += 1
        if kind == 0x3B:
            return width, height, frames
        if kind == 0x21:
            label = data[at]
            at += 1
            body = bytearray()
            while data[at]:
                body += data[at + 1:at + 1 + data[at]]
                at += 1 + data[at]
            at += 1
            if label == 0xF9:
                disposal = (body[0] >> 2) & 7
                delay = int.from_bytes(body[1:3], "little")
                transparent = body[3] if body[0] & 1 else None
            continue
        if kind != 0x2C:
            raise ValueError("unexpected block 0x%02X at %d" % (kind, at - 1))
        x0 = int.from_bytes(data[at:at + 2], "little")
        y0 = int.from_bytes(data[at + 2:at + 4], "little")
        w = int.from_bytes(data[at + 4:at + 6], "little")
        h = int.from_bytes(data[at + 6:at + 8], "little")
        local = data[at + 8]
        at += 9
        table = global_table
        if local & 0x80:
            size = 3 << ((local & 7) + 1)
            table = data[at:at + size]
            at += size
        minimum = data[at]
        at += 1
        stream = bytearray()
        while data[at]:
            stream += data[at + 1:at + 1 + data[at]]
            at += 1 + data[at]
        at += 1
        indices = lzw_decode(bytes(stream), minimum)
        if len(indices) < w * h:
            raise ValueError("a %dx%d frame decoded to %d pixels" % (w, h, len(indices)))
        for y in range(h):
            for x in range(w):
                index = indices[y * w + x]
                if index == transparent:
                    continue
                to = ((y0 + y) * width + x0 + x) * 3
                canvas[to:to + 3] = table[index * 3:index * 3 + 3]
        frames.append((bytes(canvas), delay, (x0, y0, w, h)))
        if disposal == 2:
            for y in range(y0, y0 + h):
                canvas[(y * width + x0) * 3:(y * width + x0 + w) * 3] = bytes(w * 3)
        elif disposal == 3:
            canvas[:] = frames[-2][0] if len(frames) > 1 else bytes(len(canvas))
        transparent = None
        delay = 0
        disposal = 0


class Recorder:
    """Every screendump the scenario takes becomes a frame - including the
    ones the input suite's own waits take, so a window opening is recorded at
    whatever rate the wait looked at it."""

    def __init__(self, machine):
        self.machine = machine
        self.frames = None
        self.recording = False
        self._screenshot = machine.screenshot
        machine.screenshot = self.screenshot

    def screenshot(self, fixed=None):
        shot = self._screenshot()
        if self.recording:
            if self.frames is None:
                self.frames = Frames(shot.width, shot.height)
            self.frames.add(bytes(shot._pixels[:shot.width * shot.height * 3]),
                            time.time(), fixed)
        try:
            os.unlink(os.path.join(self.machine._dir, "shot%03d.ppm" % (self.machine._shot_seq - 1)))
        except OSError:
            pass
        return shot

    def hold(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.screenshot()

    def glide(self, x, y, frames=None):
        machine = self.machine
        x_from, y_from = machine.cursor
        distance = max(abs(x - x_from), abs(y - y_from))
        frames = frames or max(3, min(8, distance // 70))
        for step in range(1, frames + 1):
            t = step / frames
            eased = t * t * (3 - 2 * t)
            next_x = x_from + int(round((x - x_from) * eased))
            next_y = y_from + int(round((y - y_from) * eased))
            machine._step_by(next_x - machine.cursor[0], next_y - machine.cursor[1])
            machine.cursor = (next_x, next_y)
            self.screenshot(GLIDE_FRAME)
        for _attempt in range(3):
            found = machine.find_cursor(self.screenshot(GLIDE_FRAME), (x, y))
            if found == (x, y):
                return
            if found is not None:
                machine._step_by(x - found[0], y - found[1])
        raise RuntimeError("the pointer did not arrive at (%d, %d)" % (x, y))

    def type(self, text, per_frame=3):
        for number, character in enumerate(text):
            self.machine.type_text(character)
            if number % per_frame == per_frame - 1:
                self.screenshot(TYPING_FRAME * per_frame)
        self.screenshot(TYPING_FRAME)

    def command(self, text, settle=1.2):
        self.type(text)
        self.machine.sendkey("ret")
        self.hold(settle)


def scenario(machine, recorder):
    sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))
    import qemu_input_suite as suite

    suite.boot(machine)
    machine.move_to(*suite.EMPTY_DESKTOP)
    recorder.recording = True
    recorder.hold(1.5)

    desktop_spot = (620, 140)
    recorder.glide(*desktop_spot)
    machine.right_click()
    suite.wait_for(machine,
                   lambda s: s.count_color(suite.CTX_MENU_BG, desktop_spot[0],
                                           desktop_spot[1], 120, 40) > 500,
                   "right-clicking the desktop drew no menu")
    recorder.hold(0.6)
    mark = len(machine.read_log())
    recorder.glide(*suite.desktop_menu_item(0, desktop_spot))
    machine.click()
    suite.wait_for_windows(machine, 1)
    suite.wait_for_log_after(machine, "[settings] showing Wallpaper", mark,
                             "Change Wallpaper... did not open Settings on its Wallpaper pane")
    settings = suite.app_origin(suite.FIRST_APP_IDX, suite.SETTINGS_H)
    recorder.hold(0.8)
    mark = len(machine.read_log())
    recorder.glide(*suite.widget_center(machine, settings, "wallpaper%d" % suite.WALLPAPER_AURORA))
    machine.click()
    suite.wait_for_log_after(machine, "[desktop] wallpaper Aurora", mark,
                             "picking Aurora did not repaint the desktop")
    recorder.hold(1.2)
    recorder.glide(*suite.titlebar_button_center(settings[0], settings[1], suite.SETTINGS_W,
                                                 suite.BTN_CLOSE))
    machine.click()
    suite.wait_for_windows(machine, 0)
    recorder.hold(0.8)

    recorder.glide(suite.ICON_X, suite.ICONS[0][2])
    machine.double_click()
    suite.wait_for_windows(machine, 1)
    terminal = suite.app_origin(suite.FIRST_APP_IDX)
    recorder.glide(terminal[0] + suite.TERM_W - 40, terminal[1] + suite.TERM_H - 30)
    recorder.command("echo Hello from lean_os")
    recorder.command("ls /")
    recorder.command("cat /proc/cpuinfo", settle=1.6)

    recorder.glide(suite.ICON_X, suite.ICONS[2][2])
    machine.double_click()
    suite.wait_for_windows(machine, 2)
    recorder.hold(2.0)

    recorder.glide(*suite.START_CLICK)
    closed = recorder.screenshot().px(*suite.LAUNCHER_PROBE)
    machine.click()
    suite.wait_for(machine, lambda s: s.px(*suite.LAUNCHER_PROBE) != closed,
                   "the Start button did not open the Start menu")
    recorder.hold(1.0)
    recorder.type("paint", per_frame=1)
    recorder.hold(0.6)
    machine.sendkey("ret")
    suite.wait_for_windows(machine, 3)
    paint = suite.app_origin(suite.FIRST_APP_IDX + 2, suite.PAINT_H)
    recorder.hold(0.6)
    recorder.glide(paint[0] + 10 + 26 + 10, paint[1] + 20)
    machine.click()
    top = paint[1] + suite.PAINT_TOOLBAR_H + 60
    recorder.glide(paint[0] + 60, top + 120)
    machine.button(machine.BTN_LEFT)
    for step in range(1, 25):
        x = paint[0] + 60 + step * 17
        y = top + 120 - int(round(90 * (1 - ((step - 12) / 12.0) ** 2)))
        machine._step_by(x - machine.cursor[0], y - machine.cursor[1])
        machine.cursor = (x, y)
        if step % 2 == 0:
            recorder.screenshot(GLIDE_FRAME)
    machine.button(0)
    recorder.hold(2.0)
    recorder.recording = False
    return recorder.frames, time.time()


def place_in_readme():
    with open(README_PATH, "r", encoding="utf-8") as handle:
        text = handle.read()
    if README_LINE in text:
        return False
    lines = [line for line in text.split("\n") if not line.startswith("![") or ("](%s)" % GIF_NAME) not in line]
    text = "\n".join(lines)
    at = text.find(README_ANCHOR)
    if at < 0:
        raise RuntimeError("README.md has no %r to put the demo above" % README_ANCHOR)
    text = text[:at] + README_LINE + "\n\n" + text[at:]
    with open(README_PATH, "w", encoding="utf-8") as handle:
        handle.write(text)
    return True


def record(arguments):
    sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))
    import qemu_input

    if not arguments.no_build:
        subprocess.check_call(["make", "--no-print-directory", "all"], cwd=REPO_ROOT,
                              stdout=subprocess.DEVNULL)
    started = time.time()
    with qemu_input.Machine(quiet=True, boot_timeout=300) as machine:
        recorder = Recorder(machine)
        frames, finished = scenario(machine, recorder)
    print("readme-demo: recorded %d distinct screens in %.1f s"
          % (len(frames.kept), time.time() - started))

    durations = frames.durations(finished)
    durations[-1] = LAST_FRAME_HOLD
    screens = [frames.pixels(i) for i in range(len(frames.kept))]
    data, palette = encode_gif(frames.width, frames.height, screens, durations)
    seconds = sum(durations)
    print("readme-demo: %dx%d, %.1f s, %d colours, %d KiB"
          % (frames.width, frames.height, seconds, len(palette), len(data) // 1024))

    if len(data) > arguments.max_bytes:
        print("readme-demo: %d bytes is over the ceiling of %d - %s left as it was"
              % (len(data), arguments.max_bytes, GIF_NAME), file=sys.stderr)
        return 1
    if seconds > arguments.max_seconds:
        print("readme-demo: %.1f s is over the ceiling of %.0f s - %s left as it was"
              % (seconds, arguments.max_seconds, GIF_NAME), file=sys.stderr)
        return 1

    staging = tempfile.NamedTemporaryFile(dir=os.path.dirname(arguments.output),
                                          prefix=".demo-", suffix=".gif", delete=False)
    with staging:
        staging.write(data)
    os.replace(staging.name, arguments.output)
    os.chmod(arguments.output, 0o644)
    if arguments.output == GIF_PATH and place_in_readme():
        print("readme-demo: README.md now shows %s" % GIF_NAME)
    print("readme-demo: wrote %s" % os.path.relpath(arguments.output, REPO_ROOT))
    return 0


def sips_first_frame(data):
    """The host's own GIF decoder on the first frame, where the host has one.
    A decoder written beside the encoder can share its misunderstanding."""
    if shutil.which("sips") is None:
        return None
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "in.gif")
        target = os.path.join(directory, "out.bmp")
        with open(source, "wb") as handle:
            handle.write(data)
        if subprocess.call(["sips", "-s", "format", "bmp", source, "--out", target],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0:
            return None
        with open(target, "rb") as handle:
            bmp = handle.read()
    offset = int.from_bytes(bmp[10:14], "little")
    width = int.from_bytes(bmp[18:22], "little", signed=True)
    height = int.from_bytes(bmp[22:26], "little", signed=True)
    depth = int.from_bytes(bmp[28:30], "little")
    per_pixel = depth // 8
    stride = (width * per_pixel + 3) & ~3
    rows = []
    for y in range(abs(height)):
        source_row = y if height < 0 else abs(height) - 1 - y
        base = offset + source_row * stride
        row = bytearray()
        for x in range(width):
            b, g, r = bmp[base + x * per_pixel:base + x * per_pixel + 3]
            row += bytes([r, g, b])
        rows.append(bytes(row))
    return width, abs(height), b"".join(rows)


def self_test():
    width, height = 97, 61
    colours = [bytes([(i * 37) & 0xFF, (i * 91 + 13) & 0xFF, (i * 53 + 7) & 0xFF]) for i in range(200)]
    base = bytearray()
    for y in range(height):
        for x in range(width):
            base += colours[(x // 7 + y // 5) % len(colours)]
    screens = [bytes(base)]
    moved = bytearray(base)
    for y in range(20, 30):
        for x in range(60, 72):
            moved[(y * width + x) * 3:(y * width + x) * 3 + 3] = colours[199]
    screens.append(bytes(moved))
    screens.append(bytes(moved))
    chooser = random.Random(1)
    noisy = bytearray(moved)
    for i in range(width * height):
        noisy[i * 3:i * 3 + 3] = colours[chooser.randrange(150)]
    screens.append(bytes(noisy))
    screens.append(bytes(base))
    durations = [0.5, 1 / 3.0, 1 / 3.0, 1 / 3.0, 0.73]

    data, palette = encode_gif(width, height, screens, durations)
    got_width, got_height, frames = decode_gif(data)
    if (got_width, got_height) != (width, height):
        print("FAIL: the GIF says %dx%d" % (got_width, got_height))
        return 1
    distinct = [screens[0], screens[1], screens[3], screens[4]]
    if len(frames) != len(distinct):
        print("FAIL: %d frames came back for %d distinct screens" % (len(frames), len(distinct)))
        return 1
    for number, (want, (got, _, _)) in enumerate(zip(distinct, frames)):
        if got != want:
            wrong = sum(1 for i in range(0, len(want), 3) if got[i:i + 3] != want[i:i + 3])
            print("FAIL: frame %d came back with %d wrong pixels" % (number, wrong))
            return 1
    delays = [delay for _, delay, _ in frames]
    if delays != [50, 67, 33, 73]:
        print("FAIL: delays came back as %r, not [50, 67, 33, 73] - an identical "
              "screen's time goes to the frame before it, and a third of a "
              "second three times is a second rather than 99 centiseconds" % delays)
        return 1
    if frames[1][2] != (60, 20, 12, 10):
        print("FAIL: a 12x10 change was sent as the rectangle %r - every frame "
              "is meant to carry only what changed" % (frames[1][2],))
        return 1

    small = [bytes([i]) * 3 * 4 for i in range(3)]
    _, _, thirds = decode_gif(encode_gif(2, 2, small, [1 / 3.0] * 3)[0])
    if [delay for _, delay, _ in thirds] != [33, 34, 33]:
        print("FAIL: three thirds of a second came back as %r centiseconds - "
              "what one frame's rounding gains, the next has to give back"
              % [delay for _, delay, _ in thirds])
        return 1

    side = 128
    chooser = random.Random(228)
    noise = bytearray()
    for _ in range(side * side * 3 // 4):
        noise += colours[chooser.randrange(150)]
    full = bytes(noise + noise[-side * side * 3 // 4:])
    data_full, _ = encode_gif(side, side, [full], [1.0])
    if decode_gif(data_full)[2][0][0] != full:
        print("FAIL: a picture that fills the code table and then repeats "
              "itself does not come back")
        return 1

    many = bytearray()
    for y in range(64):
        for x in range(64):
            many += bytes([x * 4, y * 4, (x ^ y) * 4])
    data_many, palette_many = encode_gif(64, 64, [bytes(many)], [1.0])
    if len(palette_many) > TRANSPARENT:
        print("FAIL: %d colours in a palette with %d to spare" % (len(palette_many), TRANSPARENT))
        return 1
    _, _, frames_many = decode_gif(data_many)
    worst = max(abs(a - b) for a, b in zip(frames_many[0][0], many))
    if worst > 40:
        print("FAIL: 4,096 colours reduced to %d, and one channel moved by %d"
              % (len(palette_many), worst))
        return 1

    host = sips_first_frame(data)
    if host is not None:
        if host[:2] != (width, height) or host[2] != screens[0]:
            print("FAIL: the host's own decoder reads the first frame differently")
            return 1
        if sips_first_frame(data_full)[2] != full:
            print("FAIL: the host's own decoder reads a frame that filled the code "
                  "table differently - the clear code or a width change is wrong")
            return 1
        verdict = "and the host's sips reads two of them identically"
    else:
        verdict = "(no sips here to read it a second way)"
    print("readme-demo: %d screens encode to %d frames that decode exactly, "
          "4,096 colours reduce to %d within %d a channel, %s"
          % (len(screens), len(frames), len(palette_many), worst, verdict))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true",
                        help="grade the GIF encoder and run no machine")
    parser.add_argument("--no-build", action="store_true",
                        help="record the image as it is rather than running make all first")
    parser.add_argument("--output", default=GIF_PATH,
                        help="where to write the GIF; README.md is only touched for %s" % GIF_NAME)
    parser.add_argument("--max-bytes", type=int, default=MAX_BYTES)
    parser.add_argument("--max-seconds", type=float, default=MAX_SECONDS)
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    arguments.output = os.path.abspath(arguments.output)
    return record(arguments)


if __name__ == "__main__":
    sys.exit(main())
