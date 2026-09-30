#!/usr/bin/env python3
"""The log a lean_os machine wrote onto the disk it booted from.

A machine booted from an image made by tools/make-hardware-image.sh mirrors
its kernel log - everything the serial port would have carried, including
what programs print - into \\LOGS\\LEANOS.LOG on the stick's EFI partition,
once a second. Put the stick back in this computer and:

    sudo tools/read-usb-log.py /dev/diskN            # the whole log
    sudo tools/read-usb-log.py /dev/diskN --last     # only the last boot
    tools/read-usb-log.py /Volumes/NO\\ NAME/LOGS/LEANOS.LOG
    tools/read-usb-log.py build/os-image-hardware.bin

Given a disk or an image it finds the file by its header without mounting
anything; given the file itself it reads that. The file is a ring - once it
is full the kernel goes back to its start - so opened in an editor after it
has wrapped, the newest text is at the top. This puts it back in order.

A busy second's [t+] line names where the kernel's time went (M202):
"sys#N" becomes the system call's name from system_api/include/syscall.h,
and an address after "at" becomes function+offset from build/kernel.elf -
which is only right while that file is the kernel the stick booted, so
--raw leaves both alone.

Two other forms are for tools/make-hardware-image.sh:
    --make-area <sectors>                           an empty area, to stdout
    --locate <image> <esp_lba> <esp_sectors> <file> the file's first LBA
"""

import argparse
import os
import re
import sys

REPOSITORY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECTOR = 512
MAGIC = b"LEAN_OS LOG AREA 1\n"


def header(next_offset, wrapped, boots):
    text = MAGIC + b"next=%d\nwrapped=%d\nboots=%d\n" % (next_offset, wrapped, boots)
    return text + b"\n" * (SECTOR - len(text))


def make_area(sectors):
    if sectors < 2:
        sys.exit("a log area needs a header sector and at least one for text")
    sys.stdout.buffer.write(header(0, 0, 0) + b"\n" * ((sectors - 1) * SECTOR))


def locate(image, esp_lba, esp_sectors, log_file):
    with open(log_file, "rb") as f:
        wanted = f.read()
    with open(image, "rb") as f:
        f.seek(esp_lba * SECTOR)
        esp = f.read(esp_sectors * SECTOR)
    found = [i for i in range(0, len(esp), SECTOR) if esp.startswith(MAGIC, i)]
    if len(found) != 1:
        sys.exit("expected one log header in the EFI partition, found %d" % len(found))
    start = found[0]
    if esp[start:start + len(wanted)] != wanted:
        sys.exit("the log file is not one contiguous run of sectors - the kernel "
                 "cannot write it without a FAT driver")
    print(esp_lba + start // SECTOR)


def parse_header(sector):
    if not sector.startswith(MAGIC):
        return None
    fields = dict(re.findall(rb"^(next|wrapped|boots)=(\d+)$", sector, re.M))
    if set(fields) != {b"next", b"wrapped", b"boots"}:
        return None
    return {k.decode(): int(v) for k, v in fields.items()}


def area_from_disk(path):
    """An MBR disk or image: the 0xEF partition, scanned for the header."""
    try:
        f = open(path, "rb")
    except PermissionError:
        sys.exit("%s needs root to read - run this with sudo" % path)
    with f:
        mbr = f.read(SECTOR)
        if len(mbr) < SECTOR or mbr[510:512] != b"\x55\xaa":
            return None
        for entry in range(4):
            base = 0x1BE + entry * 16
            if mbr[base + 4] != 0xEF:
                continue
            lba = int.from_bytes(mbr[base + 8:base + 12], "little")
            count = int.from_bytes(mbr[base + 12:base + 16], "little")
            f.seek(lba * SECTOR)
            esp = f.read(count * SECTOR)
            for i in range(0, len(esp), SECTOR):
                if esp.startswith(MAGIC, i) and parse_header(esp[i:i + SECTOR]):
                    return esp[i:]
        return None


def area_bytes(path):
    if not os.path.exists(path):
        sys.exit("%s does not exist - 'diskutil list external' names the stick" % path)
    try:
        with open(path, "rb") as f:
            start = f.read(SECTOR)
    except PermissionError:
        sys.exit("%s needs root to read - run this with sudo" % path)
    if parse_header(start):
        with open(path, "rb") as f:
            return f.read()
    area = area_from_disk(path)
    if area is None:
        sys.exit("%s is neither LEANOS.LOG nor a disk with one on its EFI partition" % path)
    return area


def in_order(area):
    fields = parse_header(area[:SECTOR])
    data = area[SECTOR:]
    next_offset = min(fields["next"], len(data))
    if fields["wrapped"]:
        text = data[next_offset:] + data[:next_offset]
    else:
        text = data[:next_offset]
    return fields, text.strip(b"\n") + b"\n"


def syscall_names():
    names = {}
    path = os.path.join(REPOSITORY, "system_api", "include", "syscall.h")
    try:
        with open(path) as f:
            for line in f:
                m = re.match(r"#define SYS_(\w+)\s+(\d+)\b", line)
                if m:
                    names[int(m.group(2))] = m.group(1)
    except OSError:
        pass
    return names


def kernel_symbols():
    elf = os.path.join(REPOSITORY, "build", "kernel.elf")
    if not os.path.exists(elf):
        return []
    import subprocess
    for nm in ("x86_64-elf-nm", "nm"):
        try:
            out = subprocess.run([nm, "-n", elf], capture_output=True, text=True, check=True).stdout
        except (OSError, subprocess.CalledProcessError):
            continue
        symbols = []
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "tTwW":
                symbols.append((int(parts[0], 16), parts[2]))
        return symbols
    return []


def annotate(text):
    names = syscall_names()
    symbols = kernel_symbols()
    import bisect
    starts = [a for a, _ in symbols]

    def where(address):
        i = bisect.bisect_right(starts, address) - 1
        if i < 0:
            return "0x%x" % address
        return "%s+0x%x" % (symbols[i][1], address - symbols[i][0])

    def line(m):
        body = m.group(0)
        body = re.sub(r"sys#(\d+)", lambda n: names.get(int(n.group(1)), n.group(0)), body)
        if symbols:
            head, sep, tail = body.partition(", at ")
            if sep:
                at, rest_sep, rest = tail.partition(";")
                at = re.sub(r"0x([0-9a-f]+)", lambda n: where(int(n.group(1), 16)), at)
                body = head + sep + at + rest_sep + rest
        return body

    return re.sub(r"^\[t\+[^\n]*kernel samples[^\n]*$", line, text, flags=re.M)


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--make-area":
        make_area(int(sys.argv[2]))
        return
    if len(sys.argv) >= 2 and sys.argv[1] == "--locate":
        locate(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5])
        return

    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("source", help="/dev/diskN, an image, or a copied LEANOS.LOG")
    parser.add_argument("--last", action="store_true", help="only the most recent boot")
    parser.add_argument("-o", "--output", help="write here instead of to stdout")
    parser.add_argument("--raw", action="store_true", help="leave sys#N and kernel addresses as numbers")
    arguments = parser.parse_args()

    fields, text = in_order(area_bytes(arguments.source))
    if arguments.last:
        marks = [m.start() for m in re.finditer(rb"^===== lean_os boot \d+ =====$", text, re.M)]
        if marks:
            text = text[marks[-1]:]
    if not arguments.raw:
        text = annotate(text.decode("utf-8", "replace")).encode("utf-8")
    sys.stderr.write("%s: %d boots recorded%s\n" % (
        arguments.source, fields["boots"],
        ", the oldest text overwritten" if fields["wrapped"] else ""))
    if arguments.output:
        with open(arguments.output, "wb") as f:
            f.write(text)
        if os.environ.get("SUDO_UID"):
            os.chown(arguments.output, int(os.environ["SUDO_UID"]), int(os.environ["SUDO_GID"]))
    else:
        sys.stdout.buffer.write(text)


if __name__ == "__main__":
    main()
