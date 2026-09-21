#!/usr/bin/env python3
"""The picture Chromium drew, read back and graded.

The browser writes a PNG on the machine and the boot prints it over the
serial line in base64, because that is the only wire out of a booted machine
a harness already reads.  This decodes it here.

The decoder is written out rather than handed to an image library on purpose.
What is being graded is pixel values - a rasteriser that links and produces a
blank bitmap passes every test that only asks whether the calls returned, and
that rule is M157's - so the thing doing the reading has to be something this
project can say is right.  zlib is the one piece taken from elsewhere, and it
is the format's own compressor rather than an opinion about pictures.
"""

import argparse
import base64
import sys
import zlib


def shot_from_log(path):
    payload = []
    seen_begin = False
    seen_end = False
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            if not line.startswith("[shot] "):
                continue
            body = line[len("[shot] "):]
            if body == "begin":
                payload = []
                seen_begin = True
                seen_end = False
                continue
            if body.startswith("end "):
                seen_end = True
                continue
            payload.append(body)
    if not seen_begin:
        raise SystemExit("browser-shot: the log has no picture in it")
    if not seen_end:
        raise SystemExit("browser-shot: the picture was cut off mid-transfer")
    return base64.b64decode("".join(payload))


def decode_png(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("browser-shot: that is not a PNG")
    offset = 8
    width = height = depth = colour = 0
    compressed = b""
    while offset < len(data):
        length = int.from_bytes(data[offset:offset + 4], "big")
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
            depth = body[8]
            colour = body[9]
            if body[12]:
                raise SystemExit("browser-shot: interlaced PNGs are not read here")
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
    if depth != 8 or colour not in (2, 6):
        raise SystemExit(
            "browser-shot: depth %d colour type %d is not read here" % (depth, colour))
    per_pixel = 3 if colour == 2 else 4
    raw = zlib.decompress(compressed)
    stride = width * per_pixel
    rows = []
    previous = bytearray(stride)
    at = 0
    for _ in range(height):
        filter_type = raw[at]
        at += 1
        row = bytearray(raw[at:at + stride])
        at += stride
        for x in range(stride):
            left = row[x - per_pixel] if x >= per_pixel else 0
            up = previous[x]
            upleft = previous[x - per_pixel] if x >= per_pixel else 0
            if filter_type == 1:
                row[x] = (row[x] + left) & 0xFF
            elif filter_type == 2:
                row[x] = (row[x] + up) & 0xFF
            elif filter_type == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 0xFF
            elif filter_type == 4:
                pa = abs(up - upleft)
                pb = abs(left - upleft)
                pc = abs(left + up - 2 * upleft)
                if pa <= pb and pa <= pc:
                    guess = left
                elif pb <= pc:
                    guess = up
                else:
                    guess = upleft
                row[x] = (row[x] + guess) & 0xFF
            elif filter_type != 0:
                raise SystemExit("browser-shot: filter %d is not a filter" % filter_type)
        rows.append(row)
        previous = row
    return width, height, per_pixel, rows


def pixel(rows, per_pixel, x, y):
    row = rows[y]
    at = x * per_pixel
    return (row[at], row[at + 1], row[at + 2])


def box_of(rows, width, height, per_pixel, colour):
    left = width
    top = height
    right = -1
    bottom = -1
    count = 0
    for y in range(height):
        for x in range(width):
            if pixel(rows, per_pixel, x, y) == colour:
                count += 1
                left = min(left, x)
                right = max(right, x)
                top = min(top, y)
                bottom = max(bottom, y)
    return left, top, right, bottom, count


def encode_png(width, height, rows, filters):
    """A PNG written here, to grade the decoder above.

    Every scanline is written with a DIFFERENT filter, because a decoder that
    implements only filter 0 reads a real picture perfectly until the encoder
    on the far side changes its mind - and Chromium's does, per line, by
    whichever is smallest.
    """
    raw = bytearray()
    previous = bytearray(width * 3)
    for y in range(height):
        line = bytearray(rows[y])
        kind = filters[y % len(filters)]
        out = bytearray()
        for x in range(len(line)):
            left = line[x - 3] if x >= 3 else 0
            up = previous[x]
            upleft = previous[x - 3] if x >= 3 else 0
            if kind == 0:
                out.append(line[x])
            elif kind == 1:
                out.append((line[x] - left) & 0xFF)
            elif kind == 2:
                out.append((line[x] - up) & 0xFF)
            elif kind == 3:
                out.append((line[x] - ((left + up) >> 1)) & 0xFF)
            else:
                pa = abs(up - upleft)
                pb = abs(left - upleft)
                pc = abs(left + up - 2 * upleft)
                if pa <= pb and pa <= pc:
                    guess = left
                elif pb <= pc:
                    guess = up
                else:
                    guess = upleft
                out.append((line[x] - guess) & 0xFF)
        raw.append(kind)
        raw += out
        previous = line

    def chunk(kind, body):
        return (len(body).to_bytes(4, "big") + kind + body +
                zlib.crc32(kind + body).to_bytes(4, "big"))

    header = (width.to_bytes(4, "big") + height.to_bytes(4, "big") +
              bytes([8, 2, 0, 0, 0]))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def self_test():
    """The decoder, graded against a picture whose pixels are known.

    This exists because of what the thing it grades is for: a picture that
    comes back wrong and a decoder that reads it wrong are indistinguishable
    from one end, and the browser test believes this file.
    """
    width, height = 37, 11
    want = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            row += bytes([(x * 7 + y) & 0xFF, (x ^ (y * 3)) & 0xFF,
                          (x * x + y) & 0xFF])
        want.append(bytes(row))
    data = encode_png(width, height, want, [0, 1, 2, 3, 4])
    got_width, got_height, per_pixel, rows = decode_png(data)
    if (got_width, got_height, per_pixel) != (width, height, 3):
        print("FAIL: the decoder read %dx%d at %d bytes a pixel"
              % (got_width, got_height, per_pixel))
        return 1
    for y in range(height):
        if bytes(rows[y]) != want[y]:
            print("FAIL: row %d came back different - filter %d"
                  % (y, y % 5))
            return 1
    # And the base64 the machine prints, round-tripped through the same
    # reader the serial log goes through.
    encoded = base64.b64encode(data).decode("ascii")
    lines = ["[shot] begin"]
    for at in range(0, len(encoded), 60):
        lines.append("[shot] " + encoded[at:at + 60])
    lines.append("[shot] end %d bytes" % len(data))
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as handle:
        handle.write("\n".join(lines) + "\n")
        name = handle.name
    back = shot_from_log(name)
    import os
    os.unlink(name)
    if back != data:
        print("FAIL: the base64 a boot prints does not come back as itself")
        return 1
    print("browser-shot: the decoder reads all five PNG filters and the "
          "serial-log transfer round trips (%d bytes)" % len(data))
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log", nargs="?")
    parser.add_argument("--self-test", action="store_true",
                        help="grade this decoder against a picture made here")
    parser.add_argument("--save", help="write the decoded PNG here as well")
    parser.add_argument("--width", type=int, default=400)
    parser.add_argument("--height", type=int, default=300)
    parser.add_argument("--describe", action="store_true",
                        help="print what is in the picture and grade nothing")
    arguments = parser.parse_args()

    if arguments.self_test:
        return self_test()
    if not arguments.log:
        parser.error("a serial log, or --self-test")

    data = shot_from_log(arguments.log)
    if arguments.save:
        with open(arguments.save, "wb") as handle:
            handle.write(data)
    width, height, per_pixel, rows = decode_png(data)

    if arguments.describe:
        tally = {}
        for y in range(height):
            for x in range(width):
                here = pixel(rows, per_pixel, x, y)
                tally[here] = tally.get(here, 0) + 1
        print("%dx%d, %d bytes, %d colours" % (width, height, len(data), len(tally)))
        for colour, count in sorted(tally.items(), key=lambda kv: -kv[1])[:8]:
            print("  %-16s %d" % (str(colour), count))
        return 0

    # Every number below is worked out here from the page the machine was
    # given rather than read off a picture somebody looked at once.
    #
    # The window is BIGGER than the page: content_shell asks ozone for a
    # window sized to hold a 400x300 document and Chromium draws its own
    # frame around it, because a headless ozone window has no decorations
    # from anybody else. How thick that frame is, is Chromium's business, so
    # nothing here knows: the page is found by looking for it.
    page = (0, 160, 0)
    box = (200, 0, 0)
    page_width, page_height = arguments.width, arguments.height
    box_left, box_top, box_width, box_height = 40, 30, 120, 60

    failures = []

    left, top, right, bottom, ground = box_of(rows, width, height, per_pixel, page)
    if ground == 0:
        print("FAIL: the page was never drawn - no pixel is rgb%s in the "
              "%dx%d picture" % (str(page), width, height))
        return 1
    if (right - left + 1, bottom - top + 1) != (page_width, page_height):
        failures.append("the body covers %dx%d and the document is %dx%d"
                        % (right - left + 1, bottom - top + 1,
                           page_width, page_height))

    box_l, box_t, box_r, box_b, box_count = box_of(rows, width, height,
                                                   per_pixel, box)
    if box_count == 0:
        failures.append("the box is not in the picture at all")
    else:
        want = (left + box_left, top + box_top,
                left + box_left + box_width - 1, top + box_top + box_height - 1)
        if (box_l, box_t, box_r, box_b) != want:
            failures.append("the box is at %s and the stylesheet put it at %s"
                            % ((box_l, box_t, box_r, box_b), want))
        if box_count != box_width * box_height:
            failures.append("the box is %d pixels and %dx%d is %d"
                            % (box_count, box_width, box_height,
                               box_width * box_height))

    if ground != page_width * page_height - box_width * box_height:
        failures.append("the body is %d pixels and the document minus the box "
                        "is %d" % (ground,
                                   page_width * page_height - box_width * box_height))

    for message in failures:
        print("FAIL: " + message)
    if failures:
        return 1
    print("PASS: a %dx%d document inside a %dx%d window - the body fills %d "
          "pixels of rgb%s and the box is %d pixels of rgb%s with its edges "
          "exactly where the stylesheet put them"
          % (page_width, page_height, width, height, ground, page, box_count, box))
    return 0


if __name__ == "__main__":
    sys.exit(main())
