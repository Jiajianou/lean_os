# tests/python/m99.py - M99's fixture, and M80's own bar
#
# M80 was abandoned with `python3 -c "print(1+1)"` not running. Its entry
# named the bar that mattered more than that one: "a real script with a
# dict, a class, a loop and a file open". This is that script, and every
# line of it is chosen to touch something this OS had to grow before
# Python could run at all.
#
# It prints one line per section and a final marker the boot self-test
# greps for. A failure is an exception, which Python reports with a
# traceback - a better diagnostic than anything this project would have
# written for it, which is the whole argument for running somebody
# else's language.
import os
import sys


class Ledger:
    """A class, because M80's bar named one."""

    def __init__(self, name):
        self.name = name
        self.rows = {}

    def add(self, key, value):
        self.rows[key] = self.rows.get(key, 0) + value
        return self

    def total(self):
        return sum(self.rows.values())

    def __repr__(self):
        return "<Ledger %s %d rows>" % (self.name, len(self.rows))


def main():
    print("m99: python", sys.version.split()[0], "on", sys.platform)

    # A dict and a loop.
    led = Ledger("m99")
    for i in range(1, 11):
        led.add("even" if i % 2 == 0 else "odd", i)
    assert led.total() == 55, led.rows
    assert led.rows["even"] == 30 and led.rows["odd"] == 25, led.rows
    print("m99: dict and loop ok:", repr(led), led.rows)

    # A file open, written and read back - the filesystem, through
    # Python's own io stack rather than through this project's.
    path = "/tmp/m99.txt"
    with open(path, "w") as f:
        for i in range(5):
            f.write("line %d\n" % i)
    with open(path) as f:
        lines = f.read().splitlines()
    assert lines == ["line %d" % i for i in range(5)], lines
    print("m99: file write/read ok:", len(lines), "lines")

    # The filesystem through the os module, which is a different set of
    # syscalls again.
    st = os.stat(path)
    assert st.st_size == sum(len("line %d\n" % i) for i in range(5)), st.st_size
    names = os.listdir("/tmp")
    assert "m99.txt" in names, names
    os.unlink(path)
    print("m99: os.stat/listdir/unlink ok:", st.st_size, "bytes")

    # Exceptions, comprehensions, sorting, string formatting - the
    # ordinary machinery a script uses without thinking about it.
    try:
        {}["missing"]
    except KeyError as exc:
        caught = str(exc)
    else:
        raise AssertionError("KeyError not raised")
    squares = sorted((x * x for x in range(6)), reverse=True)
    assert squares == [25, 16, 9, 4, 1, 0], squares
    assert caught == "'missing'", caught
    print("m99: exceptions and comprehensions ok:", squares)

    # An import of a standard-library module that is NOT built in - the
    # thing M80 could not do at all, because its library was frozen into
    # the binary. This one is read as a .py file off this disk.
    import json

    blob = json.dumps({"b": [1, 2, 3], "a": "x"}, sort_keys=True)
    assert blob == '{"a": "x", "b": [1, 2, 3]}', blob
    assert json.loads(blob)["b"][2] == 3
    print("m99: a .py module imported from the disk ok:", blob)

    print("m99: python runs here")


main()
