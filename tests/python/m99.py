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

    # The descriptors that are not files, which is where this OS was
    # wrong and where the wrongness was invisible.
    #
    # SYS_fstat refused everything that was not FD_FILE until M99, so
    # CPython's create_stdio() could not stat 0, 1 or 2 and set all three
    # to None - and `print()` with sys.stdout None does nothing and
    # returns successfully. An interpreter with no output and no error.
    # Every assertion here is one of the answers that was missing.
    import errno
    import stat as statmod

    assert sys.stdout is not None and sys.stderr is not None, "no std streams"
    r, w = os.pipe()
    try:
        assert statmod.S_ISFIFO(os.fstat(r).st_mode), oct(os.fstat(r).st_mode)
        assert statmod.S_ISFIFO(os.fstat(w).st_mode), oct(os.fstat(w).st_mode)
    finally:
        os.close(r)
        os.close(w)
    # stderr is the console on this machine, and a console is a character
    # device: no length, no position, and a read that waits for a person.
    assert statmod.S_ISCHR(os.fstat(2).st_mode), oct(os.fstat(2).st_mode)
    # And a descriptor that is not open says so, with a number. `Errno 0`
    # is what this used to answer, and an errno of zero is a failure with
    # no information in it.
    try:
        os.fstat(99)
    except OSError as exc:
        assert exc.errno == errno.EBADF, exc.errno
    else:
        raise AssertionError("fstat of an unopened descriptor did not fail")
    # A seek on something with no position is ESPIPE, which is how a
    # program decides whether a stream can be buffered.
    try:
        os.lseek(2, 0, os.SEEK_CUR)
    except OSError as exc:
        assert exc.errno == errno.ESPIPE, exc.errno
    else:
        raise AssertionError("seeking the console did not fail")
    print("m99: fstat answers for a pipe, a console and a bad fd ok")

    print("m99: python runs here")


main()
