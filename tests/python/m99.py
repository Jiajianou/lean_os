import os
import sys

class Ledger:

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

    led = Ledger("m99")
    for i in range(1, 11):
        led.add("even" if i % 2 == 0 else "odd", i)
    assert led.total() == 55, led.rows
    assert led.rows["even"] == 30 and led.rows["odd"] == 25, led.rows
    print("m99: dict and loop ok:", repr(led), led.rows)

    path = "/tmp/m99.txt"
    with open(path, "w") as f:
        for i in range(5):
            f.write("line %d\n" % i)
    with open(path) as f:
        lines = f.read().splitlines()
    assert lines == ["line %d" % i for i in range(5)], lines
    print("m99: file write/read ok:", len(lines), "lines")

    st = os.stat(path)
    assert st.st_size == sum(len("line %d\n" % i) for i in range(5)), st.st_size
    names = os.listdir("/tmp")
    assert "m99.txt" in names, names
    os.unlink(path)
    print("m99: os.stat/listdir/unlink ok:", st.st_size, "bytes")

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

    import json

    blob = json.dumps({"b": [1, 2, 3], "a": "x"}, sort_keys=True)
    assert blob == '{"a": "x", "b": [1, 2, 3]}', blob
    assert json.loads(blob)["b"][2] == 3
    print("m99: a .py module imported from the disk ok:", blob)

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
    assert statmod.S_ISCHR(os.fstat(2).st_mode), oct(os.fstat(2).st_mode)
    try:
        os.fstat(99)
    except OSError as exc:
        assert exc.errno == errno.EBADF, exc.errno
    else:
        raise AssertionError("fstat of an unopened descriptor did not fail")
    try:
        os.lseek(2, 0, os.SEEK_CUR)
    except OSError as exc:
        assert exc.errno == errno.ESPIPE, exc.errno
    else:
        raise AssertionError("seeking the console did not fail")
    print("m99: fstat answers for a pipe, a console and a bad fd ok")

    import _socket
    import sysconfig

    where = getattr(_socket, "__file__", None)
    if sysconfig.get_config_var("Py_ENABLE_SHARED"):
        assert where is not None, "a shared build's _socket has no file"
        assert where.endswith(".so"), where
        assert os.path.exists(where), where
        assert where != sys.executable, where
        import array
        assert array.__file__.endswith(".so"), array.__file__
        assert os.path.dirname(array.__file__) == os.path.dirname(where)
        print("m99: C extension modules dlopen'ed from lib-dynload ok")
    else:
        assert where is None, where
        print("m99: C extension modules dlopen'ed from lib-dynload ok"
              " (static build - linked in, nothing to open)")

    print("m99: python runs here")

main()
