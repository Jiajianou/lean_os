#!/usr/bin/env python3

# The lean_os fork of the Rust standard library, expressed as anchored edits
# against an unpacked rust-src rather than as a patch series, for the reason
# tools/clang-port/apply.py gives: a line number is not a place, and upstream
# moves. Every edit here attaches to code, and a moved anchor is a loud
# failure that names what it was trying to do.

import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT = os.path.join(ROOT, "tools", "rust-port")


class MissingAnchor(Exception):
    pass


def edit(path, anchor, replacement, why):
    with open(path) as handle:
        text = handle.read()
    if replacement in text:
        return "already applied"
    if anchor not in text:
        raise MissingAnchor(
            "%s: could not find the anchor for '%s'.\n"
            "The upstream file has moved. The edit itself is still what it\n"
            "was; what needs updating is where it attaches.\n"
            "Anchor: %r" % (path, why, anchor[:200]))
    with open(path, "w") as handle:
        handle.write(text.replace(anchor, replacement, 1))
    return "applied"


def install(source, destination, why):
    with open(source) as handle:
        content = handle.read()
    if os.path.exists(destination):
        with open(destination) as handle:
            if handle.read() == content:
                return "already applied"
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    with open(destination, "w") as handle:
        handle.write(content)
    return "applied"


# The libc crate. src/unix/mod.rs already ends its per-OS dispatch with a bare
# "Unknown target_os" arm, so an unknown unix gets the generic POSIX surface
# and nothing else - no struct stat, no O_*, no errno. That is why lean_os
# needs a module of its own rather than a target_os = "linux" that lies: this
# project's struct stat is not Linux's, and its O_RDONLY is 1.

LIBC_UNIX_ANCHOR = """    } else {
        // Unknown target_os
    }
}"""

LIBC_UNIX_EDIT = """    } else if #[cfg(target_os = "lean_os")] {
        mod lean_os;
        pub use self::lean_os::*;
    } else {
        // Unknown target_os
    }
}"""

# src/new/ reexports a per-OS `unistd` for every unix. lean_os's _SC_ values
# come from its own generated module instead, because they are generated from
# this project's <unistd.h> and a second hand-written table could disagree
# with it.

LIBC_NEW_ANCHOR = """    if #[cfg(all(target_family = "unix", not(target_os = "qurt")))] {"""

LIBC_NEW_EDIT = """    if #[cfg(all(
        target_family = "unix",
        not(target_os = "qurt"),
        not(target_os = "lean_os")
    ))] {"""

# The standard library's own workspace resolves libc from crates.io. The fork
# has to be the one std compiles against, so the workspace patches it to the
# vendored copy this script edits.

STD_WORKSPACE_ANCHOR = """windows-sys = { path = 'windows-sys' }"""

STD_WORKSPACE_EDIT = """windows-sys = { path = 'windows-sys' }
libc = { path = 'vendor/libc-0.2.189' }"""


# The libc port is its own function because there are TWO checkouts of this
# crate on this machine at the same version, and they must not drift: rust-src
# vendors libc-0.2.189 for std to compile against, and Chromium vendors the
# same 0.2.189 under third_party/rust/chromium_crates_io for its own Rust
# targets - which is how //skia reaches it, through fontconfig's fontations
# font backend. M157 found the second one by building Skia. One port, applied
# twice, rather than 948 lines copied.


# Chromium's Rust rules refuse to compile a source the GN target does not
# list, so a crate whose build file enumerates every file needs telling about
# two new ones. That belongs here rather than in a Chromium patch: this is
# where the module's file names are already written down, and a patch
# carrying them would be a second place for them to be wrong. rust-src's
# copy has no BUILD.gn, so the argument is optional.

LIBC_SOURCES_ANCHOR = """    \"//third_party/rust/chromium_crates_io/vendor/libc-v0_2/src/unix/linux_like/android/b32/arm.rs\","""

LIBC_SOURCES_EDIT = """    \"//third_party/rust/chromium_crates_io/vendor/libc-v0_2/src/unix/lean_os/constants.rs\",
    \"//third_party/rust/chromium_crates_io/vendor/libc-v0_2/src/unix/lean_os/mod.rs\",
    \"//third_party/rust/chromium_crates_io/vendor/libc-v0_2/src/unix/linux_like/android/b32/arm.rs\","""


def apply_libc(libc, report, label="libc ", build_gn=None):
    report("%s src/unix/mod.rs" % label, edit(
        os.path.join(libc, "src", "unix", "mod.rs"),
        LIBC_UNIX_ANCHOR, LIBC_UNIX_EDIT,
        "lean_os is a unix with a libc of its own"))

    report("%s src/new/mod.rs" % label, edit(
        os.path.join(libc, "src", "new", "mod.rs"),
        LIBC_NEW_ANCHOR, LIBC_NEW_EDIT,
        "lean_os's unistd constants are generated, not reexported"))

    for name in ("mod.rs", "constants.rs"):
        report("%s src/unix/lean_os/%s" % (label, name), install(
            os.path.join(PORT, "libc", "lean_os", name),
            os.path.join(libc, "src", "unix", "lean_os", name),
            "the lean_os libc module"))

    if build_gn:
        report("%s BUILD.gn" % label, edit(
            build_gn, LIBC_SOURCES_ANCHOR, LIBC_SOURCES_EDIT,
            "a source Chromium's Rust rules have not been told about is a "
            "source they refuse to compile"))


def apply(tree, report):
    apply_libc(os.path.join(tree, "vendor", "libc-0.2.189"), report)

    report("std   Cargo.toml", edit(
        os.path.join(tree, "Cargo.toml"),
        STD_WORKSPACE_ANCHOR, STD_WORKSPACE_EDIT,
        "std compiles against the forked libc"))

    for name in ("mod.rs", "raw.rs", "fs.rs"):
        report("std   src/os/lean_os/%s" % name, install(
            os.path.join(PORT, "std", "os", "lean_os", name),
            os.path.join(tree, "std", "src", "os", "lean_os", name),
            "std::os::lean_os"))

    for source, anchor, replacement, why in STD_EDITS:
        report("std   src/%s" % source, edit(
            os.path.join(tree, "std", "src", source), anchor, replacement, why))


STD_EDITS = [
    ("../../unwind/src/lib.rs",
     """#[cfg(target_os = "redox")]
#[link(name = "gcc_eh", kind = "static", modifiers = "-bundle", cfg(target_feature = "crt-static"))]""",
     """#[cfg(target_os = "lean_os")]
#[link(name = "gcc_eh", kind = "static", modifiers = "-bundle")]
unsafe extern "C" {}

#[cfg(target_os = "redox")]
#[link(name = "gcc_eh", kind = "static", modifiers = "-bundle", cfg(target_feature = "crt-static"))]""",
     "M121 chose GCC's unwinder over LLVM's compiler-rt and libunwind, and "
     "this is that decision said again in Rust's vocabulary: std's backtrace "
     "calls _Unwind_Backtrace, and libgcc_eh.a is where it lives here"),

    ("sys/io/error/unix.rs",
     """            target_os = "hurd",
            target_os = "teeos",
        ),
        link_name = "__errno_location"
    )]""",
     """            target_os = "hurd",
            target_os = "teeos",
            target_os = "lean_os",
        ),
        link_name = "__errno_location"
    )]""",
     "errno is __errno_location here, the way it is on Linux"),

    ("sys/args/unix.rs",
     """    target_os = "rtems",
    target_os = "nuttx",
))]
mod imp {
    use crate::ffi::c_char;""",
     """    target_os = "rtems",
    target_os = "nuttx",
    target_os = "lean_os",
))]
mod imp {
    use crate::ffi::c_char;""",
     "argc and argv are what crt0 passes main, so std stores them at startup"),

    ("sys/fd/unix.rs",
     """        target_os = "wasi",
    )))]
    pub fn set_cloexec(&self) -> io::Result<()> {
        unsafe {
            cvt(libc::ioctl(self.as_raw_fd(), libc::FIOCLEX))?;""",
     """        target_os = "wasi",
        target_os = "lean_os",
    )))]
    pub fn set_cloexec(&self) -> io::Result<()> {
        unsafe {
            cvt(libc::ioctl(self.as_raw_fd(), libc::FIOCLEX))?;""",
     "this OS has no FIOCLEX - close-on-exec is fcntl(F_SETFD)"),

    ("sys/fd/unix.rs",
     """        target_os = "wasi",
    ))]
    pub fn set_cloexec(&self) -> io::Result<()> {
        unsafe {
            let previous = cvt(libc::fcntl(self.as_raw_fd(), libc::F_GETFD))?;""",
     """        target_os = "wasi",
        target_os = "lean_os",
    ))]
    pub fn set_cloexec(&self) -> io::Result<()> {
        unsafe {
            let previous = cvt(libc::fcntl(self.as_raw_fd(), libc::F_GETFD))?;""",
     "...and the same edit's other half, which is the arm that is taken"),

    ("sys/net/connection/socket/unix.rs",
     """    #[cfg(not(any(target_os = "solaris", target_os = "illumos", target_os = "vita")))]
    pub fn set_nonblocking(&self, nonblocking: bool) -> io::Result<()> {
        let mut nonblocking = nonblocking as libc::c_int;""",
     """    #[cfg(not(any(
        target_os = "solaris",
        target_os = "illumos",
        target_os = "vita",
        target_os = "lean_os"
    )))]
    pub fn set_nonblocking(&self, nonblocking: bool) -> io::Result<()> {
        let mut nonblocking = nonblocking as libc::c_int;""",
     "this OS has no FIONBIO either"),

    ("sys/net/connection/socket/unix.rs",
     """    #[cfg(any(target_os = "solaris", target_os = "illumos"))]
    pub fn set_nonblocking(&self, nonblocking: bool) -> io::Result<()> {""",
     """    #[cfg(any(target_os = "solaris", target_os = "illumos", target_os = "lean_os"))]
    pub fn set_nonblocking(&self, nonblocking: bool) -> io::Result<()> {""",
     "...so a socket goes the same fcntl(F_SETFL) way a file does"),

    ("sys/pal/unix/sync/condvar.rs",
     """    target_os = "redox",
    target_os = "teeos",
)))]
impl Condvar {
    pub const PRECISE_TIMEOUT: bool = true;""",
     """    target_os = "redox",
    target_os = "teeos",
    target_os = "lean_os",
)))]
impl Condvar {
    pub const PRECISE_TIMEOUT: bool = true;""",
     "this libc has no pthread_condattr_setclock"),

    ("sys/pal/unix/sync/condvar.rs",
     """    target_os = "redox",
    target_os = "teeos",
))]
impl Condvar {
    pub const PRECISE_TIMEOUT: bool = false;""",
     """    target_os = "redox",
    target_os = "teeos",
    target_os = "lean_os",
))]
impl Condvar {
    pub const PRECISE_TIMEOUT: bool = false;""",
     "this libc has no pthread_condattr_setclock, and its "
     "pthread_cond_timedwait reads the deadline against the realtime clock, "
     "which is what CLOCK_REALTIME in this arm already says"),

    ("sys/random/mod.rs",
     """    any(target_os = "horizon", target_os = "cygwin") => {""",
     """    any(target_os = "horizon", target_os = "cygwin", target_os = "lean_os") => {""",
     "this libc has getrandom"),

    ("sys/thread/mod.rs",
     """            target_os = "aix",
            target_os = "wasi",
        )))]
        pub use unix::set_name;""",
     """            target_os = "aix",
            target_os = "wasi",
            target_os = "lean_os",
        )))]
        pub use unix::set_name;""",
     "this OS does not name threads"),

    ("sys/thread/mod.rs",
     """            target_os = "aix",
            target_os = "wasi",
        ))]
        pub use unsupported::set_name;""",
     """            target_os = "aix",
            target_os = "wasi",
            target_os = "lean_os",
        ))]
        pub use unsupported::set_name;""",
     "...so set_name is the one that says so rather than a silent no-op"),

    ("os/mod.rs",
     """#[cfg(target_os = "nuttx")]
pub mod nuttx;""",
     """#[cfg(target_os = "lean_os")]
pub mod lean_os;
#[cfg(target_os = "nuttx")]
pub mod nuttx;""",
     "std::os::lean_os"),

    ("os/unix/mod.rs",
     """    #[cfg(target_os = "nuttx")]
    pub use crate::os::nuttx::*;""",
     """    #[cfg(target_os = "lean_os")]
    pub use crate::os::lean_os::*;
    #[cfg(target_os = "nuttx")]
    pub use crate::os::nuttx::*;""",
     "...reached through std::os::unix::fs::MetadataExt like every other unix"),

    ("sys/fs/unix.rs",
     """        target_os = "nuttx",
    )))]
    pub fn modified(&self) -> io::Result<SystemTime> {""",
     """        target_os = "nuttx",
        target_os = "lean_os",
    )))]
    pub fn modified(&self) -> io::Result<SystemTime> {""",
     "this struct stat carries a timespec, not a time_t and a nanoseconds"),

    ("sys/fs/unix.rs",
     """        target_os = "nuttx",
    )))]
    pub fn accessed(&self) -> io::Result<SystemTime> {""",
     """        target_os = "nuttx",
        target_os = "lean_os",
    )))]
    pub fn accessed(&self) -> io::Result<SystemTime> {""",
     "...and the same for st_atim"),

    ("sys/fs/unix.rs",
     """        target_os = "nuttx",
        all(target_os = "vxworks", not(vxworks_lt_25_09))
    ))]
    pub fn modified(&self) -> io::Result<SystemTime> {""",
     """        target_os = "nuttx",
        target_os = "lean_os",
        all(target_os = "vxworks", not(vxworks_lt_25_09))
    ))]
    pub fn modified(&self) -> io::Result<SystemTime> {""",
     "...so st_mtim.tv_sec is the arm that is taken"),

    ("sys/fs/unix.rs",
     """        target_os = "nuttx",
        all(target_os = "vxworks", not(vxworks_lt_25_09))
    ))]
    pub fn accessed(&self) -> io::Result<SystemTime> {""",
     """        target_os = "nuttx",
        target_os = "lean_os",
        all(target_os = "vxworks", not(vxworks_lt_25_09))
    ))]
    pub fn accessed(&self) -> io::Result<SystemTime> {""",
     "...and st_atim.tv_sec"),

    ("../build.rs",
     """        || target_os == "vexos"
""",
     """        || target_os == "vexos"
        || target_os == "lean_os"
""",
     "std is not restricted here - this is the list that says a target_os "
     "has a std backend, and after this milestone lean_os does"),

    ("sys/paths/unix.rs",
     """#[cfg(target_os = "aix")]
pub fn current_exe() -> io::Result<PathBuf> {""",
     """#[cfg(any(target_os = "aix", target_os = "lean_os"))]
pub fn current_exe() -> io::Result<PathBuf> {""",
     "this OS has no /proc/self/exe, so argv[0] is what there is - and this "
     "is the arm that resolves it against the cwd and PATH rather than "
     "returning it raw"),

]


def main():
    if len(sys.argv) < 2:
        raise SystemExit(
            "usage: apply.py <rust-src library directory>\n"
            "       apply.py --libc <libc crate directory> [BUILD.gn]")

    libc_only = sys.argv[1] == "--libc"
    if libc_only:
        if len(sys.argv) < 3:
            raise SystemExit("apply.py: --libc needs a directory")
        tree = sys.argv[2]
        if not os.path.isdir(os.path.join(tree, "src", "unix")):
            raise SystemExit(
                "apply.py: %s is not a libc crate directory" % tree)
    else:
        tree = sys.argv[1]
        if not os.path.isdir(os.path.join(tree, "std")):
            raise SystemExit(
                "apply.py: %s is not a rust-src library directory" % tree)

    width = 0
    lines = []

    def report(what, outcome):
        lines.append((what, outcome))

    if libc_only:
        apply_libc(tree, report,
                   build_gn=(sys.argv[3] if len(sys.argv) > 3 else None))
    else:
        apply(tree, report)
    width = max(len(what) for what, _ in lines)
    for what, outcome in lines:
        print("  %-*s  %s" % (width, what, outcome))


main()
