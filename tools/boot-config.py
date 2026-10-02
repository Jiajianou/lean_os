#!/usr/bin/env python3
"""\\EFI\\BOOT\\lean_os.cfg as a run of sectors the kernel can rewrite (M213).

The kernel has no FAT driver. So the image tool writes the file at a fixed
size, padded with newlines, finds which sectors of the EFI partition it
landed in, and records them in the file itself as config=<lba>+<sectors> -
written in place, at the same size, so the FAT never changes.

  --pad <file> <bytes>                         pad a config file to its size
  --place <image> <esp_lba> <esp_sectors> <file>
                                               find the file's sectors, add
                                               config= to them, print the LBA
"""

import sys

SECTOR = 512
HEADER = b"# lean_os boot options"


def pad(path, size):
    with open(path, "rb") as f:
        text = f.read()
    if len(text) > size:
        sys.exit("boot-config: %s is %d bytes, more than the %d it has" % (path, len(text), size))
    if size % SECTOR:
        sys.exit("boot-config: a config area is whole sectors")
    with open(path, "wb") as f:
        f.write(text + b"\n" * (size - len(text)))


def place(image, esp_lba, esp_sectors, path):
    with open(path, "rb") as f:
        wanted = f.read()
    if not wanted.startswith(HEADER):
        sys.exit("boot-config: %s does not start with the header the kernel looks for" % path)
    with open(image, "rb") as f:
        f.seek(esp_lba * SECTOR)
        esp = f.read(esp_sectors * SECTOR)
    found = [i for i in range(0, len(esp), SECTOR) if esp[i:i + len(wanted)] == wanted]
    if len(found) != 1:
        sys.exit("boot-config: expected the config file once in the EFI partition, found it %d times" % len(found))
    lba = esp_lba + found[0] // SECTOR
    sectors = len(wanted) // SECTOR
    content = wanted.rstrip(b"\n") + b"\n"
    content += b"# These sectors, so Settings can change video= for the next boot.\n"
    content += b"config=%d+%d\n" % (lba, sectors)
    if len(content) > len(wanted):
        sys.exit("boot-config: no room left in the file for config=")
    content += b"\n" * (len(wanted) - len(content))
    with open(image, "r+b") as f:
        f.seek(lba * SECTOR)
        f.write(content)
    print(lba)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--pad":
        pad(sys.argv[2], int(sys.argv[3]))
        return
    if len(sys.argv) == 6 and sys.argv[1] == "--place":
        place(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5])
        return
    sys.exit(__doc__)


if __name__ == "__main__":
    main()
