# A browser on lean_os, and the one that will not come

M100's fifth and sixth bullets, together: *a real but small engine*, and
*the measurement, which is the deliverable*. This file is both — what
runs here, and what Chromium's build actually asks of this machine, in
numbers produced by trying rather than by estimating.

---

## Part 1: what runs

**NetSurf 3.11**, built for `x86_64-lean_os` by this project's own
compiler, installed as `/bin/netsurf` and on the desktop as **Browser**.

```
NetSurf 3.11            layout, painting, the browser itself
  libhubbub  0.3.8      HTML5 parsing
  libdom     0.4.2      the DOM
  libcss     0.9.2      CSS: cascade, selectors, computed style
  Duktape               JavaScript, with the bindings generated from WebIDL
  libnsfb    0.2.2      the framebuffer surface layer
  libcurl    8.11.1     http and https
    mbedtls  3.6.2      TLS 1.3                          (M100, 8th increment)
  freetype   2.13.3     glyph rasterisation              (M100, 4th increment)
  libpng / libjpeg / libnsgif / libnsbmp / libsvgtiny    images
  zlib       1.3.1                                       (M100, 1st increment)
  ---------------------------------------------------------------------
  this OS               sockets, TCP, the rtl8139 driver, leanfs, the
                        compositor, the capability model, and the libc
                        every line above is linked against
```

Fifteen of those are somebody else's source, and **not one of them was
edited**. The two edits the port needed are in
`tools/netsurf-port/apply.py`, they are both in NetSurf's *build system*
rather than its code, and neither is about lean_os:

- `/bin/which`, which does not exist on macOS
- `echo -n`, which this desk's `/bin/sh` prints instead of honouring

Both would be needed to cross-compile NetSurf to *any* target from this
machine.

### The seam: a surface, not a front end

The obvious way to put a framebuffer browser on a new OS is to write a
front end for it. That is a permanent fork of somebody else's program,
and this project's rule (CLAUDE.md) is that third-party source is ported
*against* rather than merged into.

`user_space/bin/nsfb_leanos.c` is the whole display port — one file,
compiled here and added to `libnsfb.a`. It works because of three
decisions upstream made for its own reasons:

1. libnsfb's surfaces register **at runtime**, through the non-static
   `_nsfb_register_surface`.
2. NetSurf's framebuffer front end picks its surface **by name at
   runtime** (`nsfb_type_from_name`).
3. That front end links libnsfb with `-Wl,--whole-archive`, so an object
   added to the archive keeps its constructor.

So a surface called `leanos` registers itself at startup and NetSurf
finds it exactly as it would find SDL.

The pixels are **zero copy**. The compositor hands a client a shared
segment of tightly packed `0x00RRGGBB` words (`user_space/lib/gfx.h`),
and libnsfb's `NSFB_FMT_XRGB8888` plotters produce precisely that —
`colour_to_pixel` in `32bpp-xrgb8888.c` swaps R and B out of NetSurf's
own ABGR colour and writes `0x00RRGGBB`. `nsfb->ptr` points straight at
the segment. There is no blit in the port and there does not need to be
one.

### What it holds

`CAP_FS_WRITE | CAP_NETWORK`. That is the entire grant
(`system_api/include/caps.h`), and the list of what it does **not** hold
is the more interesting half:

- **not `CAP_FRAMEBUFFER`** — it paints a page by drawing into its own
  window's shared segment, with no more authority over the screen than
  the clock has
- **not `CAP_CLIPBOARD`** — NetSurf's framebuffer front end has its own
  internal clipboard and never asks the system for one
- not `CAP_PROCESS_LIST`, `CAP_POWER`, `CAP_AUDIO`, `CAP_DISPLAY_MODE`

Twenty megabytes of somebody else's C and C++, running a JavaScript
engine on bytes fetched from a machine nobody here controls, holding one
bit more than a text editor. That is the model doing its job on the
hardest case it has had.

### https, and why no certificate authorities ship with it

TLS works: NetSurf → libcurl → mbedtls → this project's TCP. It is
graded against a server whose CA is on the machine's own disk.

What is **not** here is a bundle of public certificate authorities.
`Choices` points `ca_bundle` at `/etc/ssl/certs/ca-bundle.pem` and
`tools/install-netsurf.sh` does not create that file, so `https://` to
the public web fails certificate verification until somebody puts one
there.

That is a decision, on M65's rule — *don't build a thing that pretends
to enforce something*. A machine that trusts a hundred and fifty
authorities it has never looked at, shipped by a project whose whole
claim is that you can read everything on the disk, would be decoration.

**The condition for reopening it**: a way to update the bundle without
rebuilding the image. M111 built that — `os install` — and a
`ca-certificates` package is the shape this takes when somebody wants
it. Signing (also M111's open box) is what makes that worth having.

**M114 met that condition and built the package.** Until then `https://`
failed before it got near a certificate, because this machine could not
resolve a hostname at all — see M114 in `milestones.md` for the packet
capture. With the resolver fixed, `https://` failed with exactly one
message, `Problem with the SSL CA cert (path? access rights?)`, which is
mbedtls refusing to verify against a bundle that is not there.

So now:

```
os install ca-certificates
```

installs 128 authorities into `/pkg/ca-certificates/1.0/`, which is the
path `Choices` points `ca_bundle` at, and `https://example.com/` loads
and renders. **Nothing changed about what a fresh image trusts, which is
nobody** — the bundle is not in the image, it is a package somebody
installs by name, and `os` reports what it may do on the way in:

```
os: installed ca-certificates-1.0 (1 files) in /pkg/ca-certificates/1.0
os: it may: nothing but read files and use the descriptors it is given
```

That last line is not decoration. This package contains one file of text
and no executable at all, so "nothing" is the whole truth about it — the
smallest possible demonstration that a package here is data plus a
manifest rather than a program that runs.

The bundle is the **build host's** trust store, copied rather than
written here, and that is the honest description: installing it means
trusting whoever curates the machine this was built on. It is data, not
code, and it lives under `/pkg` rather than in the OS, which is exactly
where CLAUDE.md's first non-negotiable puts it.

**M116: and it now arrives with the browser, still as a package.** M115
left `os install ca-certificates` as a step somebody had to know to type
into a terminal, on an image that did not even have `/pkg/repo` on it —
`tools/run-qemu.sh` never put one there. So the machine a person booted
failed every `https://` certificate check, and google.com worked for
exactly one page. Now the browser's install (`tools/install-netsurf.sh`,
which `make browser` and `browser-if-built` run) puts the repository on
the image and names `ca-certificates` in `/pkg/repo/preinstall`, and on
first boot init runs `os preinstall`, which installs it through the same
three hashes and the same registry `os install` uses:

```
os: installed ca-certificates-1.0 (1 files) in /pkg/ca-certificates/1.0
os: it may: nothing but read files and use the descriptors it is given
```

Every property that made it a package is kept. It is removable, and
`os remove ca-certificates` **sticks** — `/pkg/db/preinstalled` records
what was preinstalled, so the next boot does not quietly put it back.
It is verifiable (`os verify`), and replaceable without rebuilding the
image. What changed is the default for an image that has a browser on
it: that image trusts the build host's authorities, the way every
browser anyone uses ships with a root store. **An image without the
browser still trusts nobody** — `make all` alone writes no repository
and no preinstall list.

### Fetching, measured — M116

The first time anybody typed google.com into this browser it did not
load. Three bugs in this OS, all below the browser, each found with a
packet capture rather than by reading code:

| what | symptom on the wire | cost per event |
|---|---|---|
| the RTL8139 driver reassembled a frame that straddled the end of its receive ring from the ring's *start* — the layout with `RCR_WRAP` clear, which it sets | a segment arrives, is ACKed as if it had not | 1.5 s retransmission, one full-sized frame in five |
| TCP judged a FIN on the same segment as data against the data's first byte | the last segment of every reply ACKed without its FIN | 1.4 s per server-closed connection |
| `gettimeofday` was RTC seconds plus uptime modulo 1000, and went backwards by up to a second | a reply sitting in the socket for seconds with the CPU half idle | up to 1 s per 10 ms timer in NetSurf's scheduler |

| | before | after |
|---|---|---|
| 60 KB from a host server, `fetch` | never finished (gave up at 8 s with 30 KB) | 35 ms |
| `http://www.google.com/` | never finished | 4.2 s, rendered |
| `https://www.google.com/` | certificate failure | 5.9 s, verified and rendered |

What still does not work is **Google search**: since 2025 a results page
is served only to a browser that runs modern JavaScript, and to anything
else it is a `<noscript>` meta-refresh to an "enable JavaScript" notice.
Duktape is ES5, so NetSurf runs the script, the script fails, and the page
stays blank. That is the right-hand column of the table below, not the
network.

### What it can and cannot render, measured

`http://example.com/` loads and renders with its stylesheet — verified
on the machine, M114. What NetSurf 3.11 *cannot* do is not a porting
problem and will not be fixed by anything in this tree:

| | here | the modern web assumes |
|---|---|---|
| CSS | libcss: CSS 2.1 plus parts of CSS 3 | flexbox, grid, custom properties |
| JavaScript | Duktape, ES5 | ES2020+, modules |
| layout | NetSurf's own | the same, plus a compositor |

A site built on any of the right-hand column will render, and will
render wrongly. That is the honest description of what is on this
machine, and it is why the next two things this browser needs are
certificate authorities and ICU rather than a different engine — see
*the browser that will not come*, below, for why there is no
smaller-effort modern one.

### The first crash, and what was under it - M117

`https://www.apple.com/` killed the browser on NetSurf's own assertion
(`layout.c:5333`, `box->height != AUTO`), and so did Wikipedia's
`/wiki/Unix` (M116's open box). The same NetSurf 3.11 built for the
host rendered both, which by M116's condition made it this port's
failure. What it was: **this libc's `malloc(0)` returned NULL.** C
allows that; NetSurf's flex layout sizes its item list with
`calloc(number_of_children, ...)`, an empty `display: flex` container
has none, and every libc NetSurf was ever built against - glibc, macOS,
musl - answers a zero-byte request with a unique pointer, so NetSurf
reads NULL as out of memory, fails the layout silently, and asserts
some frames later. apple.com's nav has one empty flex container per
flyout; google.com has none. `malloc(0)` returns the minimum block now
(`user_space/lib/malloc.c`), graded by `tests/test_malloc.c` and, in
pixels, by the input suite's `browser_survives_an_empty_flex_container`
(`tools/netsurf-port/flex.html`, installed at
`/usr/share/netsurf/flex.html`).

Two instruments found it, and both stay:

- **A backtrace on assert.** `tools/build-netsurf.sh` builds the port
  with `-fno-omit-frame-pointer`, and this libc's `__assert_fail`
  prints the chain of return addresses after its one line. Symbolise
  with `nm -n build/netsurf/netsurf` (the unstripped binary):

  ```
  assertion failed: box->height != AUTO at content/handlers/html/layout.c:5333
    #0 0x80000f89dc   layout_calculate_descendant_bboxes+0x36c
    ...
    #10 0x80001022b3  layout_document+0x1a3
    #11 0x80000ef936  html_reformat+0x136
  ```

- **The twin.** `tools/build-netsurf-host.sh` builds the same NetSurf
  for this Mac - the framebuffer front end on libnsfb's display-less
  `ram` surface, freetype with the guest's DejaVu files, the JPEG
  decoder from the same tarball, Duktape, the guest's `Choices` - and
  `tools/build-netsurf-host.sh run <url>` says rendered or crashed. It
  refuses to call the result a twin unless `FT_Init_FreeType`,
  `jpeg_read_header` and `duk_create_heap` are in the binary, because
  the first three twins built by hand were not twins (a script wrote
  its own `Makefile.config` over the one being edited) and rendered the
  page for reasons that had nothing to do with the guest. If the twin
  crashes on a page the guest crashes on, the bug is upstream's; if it
  does not, look under the browser.

The bisection that went with them - a proxy on the test harness's
guestfwd serving apple.com's bytes to the guest with one path
replaced, the page cut down to three empty divs, the 145 stylesheet
rules those divs could see halved twice - is written up in M117 in
`milestones.md`. It is the method, not a tool: the fixture it ended
at is the tool.

**And the desktop under the browser** (M117, the "everything should be
snappy" half): every wmclient program's main loop yielded instead of
blocking, so an idle desktop cost 22% of a host core and 44% with eight
windows open; the compositor learned a client had drawn only on a
100 ms poll, so a page NetSurf had finished laying out waited up to a
tenth of a second to appear. `wm_wait_ms` and `WM_ACTION_PRESENT`
(`user_space/lib/wmclient.h`) fixed both, and the NetSurf surface
presents once per turn of NetSurf's event loop and waits in
`wm_wait_ms` for the bounded time libnsfb asks for.

### What porting it found

Five gaps in this system, each named by a build rather than by a
checklist. This is M63's rule at M100's scale, and the reason the
milestone is worth more than the browser:

| gap | what named it |
|---|---|
| `pread`/`pwrite` did not exist | libnsutils wraps them for NetSurf's disc cache |
| the `lround` family did not exist | libsvgtiny calls `lroundf` |
| `scandir`/`alphasort` did not exist | NetSurf's `file:` fetcher, building a directory index |
| `STDIN_FILENO` did not exist | curl's `terminal.c`, asking whether it is a tty |
| **`<iconv.h>` did not exist at all** | NetSurf includes it unconditionally |

And a sixth that is not a missing function but a fact about this
project's own sysroot, and the sharpest of them:

> `system_api/include/signal.h` and `user_space/libc/include/signal.h`
> **have the same name**. The libc one is found first and reaches the
> other with `#include_next`, which works only because
> `usr/local/include` precedes `usr/include` in the default search
> order. curl's `configure` adds `-isystem <sysroot>/usr/include` when
> told where mbedtls is — putting the kernel ABI's `signal.h` in front —
> and then `sigset_t` vanished and `<setjmp.h>` stopped compiling.
>
> **Any third-party build that names the sysroot's include directory
> hits this.** It is worked around in `tools/build-netsurf.sh` by
> pinning `usr/local/include` ahead of it on the compiler line. It is
> not fixed. The fix is to stop having two headers with one name.

Each of the five is now built and graded: `iconv` against the host's own
over 2.58 million conversions (`tools/iconv-test.sh`), the `lround`
family against the host's libm (`tools/math-test.sh`), and all four of
the new interfaces on the machine itself by the `[m100h]` boot
self-test. The browser is graded where a browser has to be — in
framebuffer pixels, by `tools/qemu-input-test.sh`'s
`browser_renders_a_page`.

### Installing it, and the reason that is its own milestone (M113)

```sh
make browser          # cross-build NetSurf if it has never been built,
                      # then install it into the image
```

That one command replaces `tools/build-netsurf.sh` followed by
`tools/install-netsurf.sh`, and it is not sugar. **M100 left the browser
installed on exactly one image: whichever one happened to be sitting in
`build/` at the time.** `$(IMAGE)`'s recipe recreates the disk from
scratch — `cat mbr kernel > image`, then `truncate` — every time the
kernel changes, which wipes leanfs entirely. So the browser M100 built
was gone again after the next kernel edit, and nothing put it back.

What made that more than an inconvenience is what it did to the
instrument. `browser_renders_a_page` is the only thing in this project
that can grade a layout engine at all, and nothing between `make all`
and that test ever put a browser on the image it runs against. It passed
because a browser had been installed by hand and the kernel had not been
touched since. The first person to edit the kernel and run the suite
would have got a browser that painted nothing — and that failure reads
exactly like a rendering bug in `nsfb_leanos.c`.

Three things close it, and the shape is the one `make toybox` and
`make packages` already had:

- **`make browser`** builds and installs; **`make browser-if-built`**
  installs only, and says so and succeeds when the port has never been
  cross-built. Both depend on `preseed`, which is what keeps a
  third-party file out of the inode M22's launcher self-test has
  opinions about. Neither is part of `all`, for that same reason.
- **`tools/run-qemu.sh` and `tools/run-tests.sh` both go through
  `browser-if-built`** after `make all`. That is what makes the browser
  survive a kernel rebuild: the reinstall costs about a tenth of a
  second, against the twenty minutes a rebuild of the port would.
- **The `[m113]` boot self-test** asks the machine itself whether it has
  a browser: `/bin/netsurf` at a plausible size, `default.css` where the
  compiled-in `NETSURF_FB_RESPATH` looks, `DejaVuSans.ttf` where
  `NETSURF_FB_FONTPATH` looks, and the capability grant still
  `CAP_FS_WRITE | CAP_NETWORK` with `CAP_FRAMEBUFFER` absent. The last
  one is there because a browser that had quietly acquired authority
  over the screen would render perfectly and the pixel test would still
  pass.

  It **skips** rather than fails when `/bin/netsurf` is absent, naming
  `make browser` in the skip line — an image without a browser is a
  valid image, and a battery that panicked on one would make the default
  build depend on an optional twenty-minute cross-compile. Both branches
  were run rather than reasoned about: the skip on a fresh image, and
  the panic on an image where `/bin/netsurf` had been replaced with a
  21 KiB program.

---

## Part 2: the measurement — Chromium, in numbers

M100's sixth bullet asks for *a list produced by trying and reading the
errors*, not an estimate. Here is what was actually run, and what it
said. Where a number is quoted from Chromium's own source or docs rather
than measured here, it says so.

### The machine it asks for, in its own words

From `docs/linux/build_instructions.md`, fetched from
`chromium.googlesource.com` on 2026-09-10:

| Chromium requires | lean_os has |
|---|---|
| ≥ 8 GB RAM, "more than 16GB is highly recommended" | boots and passes its battery on **128 MiB** |
| ≥ 32 GB of **swap** on an 8 GB machine | **no swap at all** — refused on a number (M102) |
| ≥ 100 GB free disk | a **2 GiB** image, 361 MB of it used |
| a 64-bit host, or the link runs out of memory | yes |

The largest single translation unit this machine has ever compiled peaks
at **217 MiB** (`build_cxx_tu_peak_rss_kib`, M98). Chromium's *minimum*
is thirty-seven times the whole disk image.

### The checkout

`DEPS`, fetched the same day: **5,197 lines**, naming **535
sub-repositories**. `gclient sync` fetches all of them. For scale, the
entire NetSurf stack that runs on this machine is **fifteen**
repositories and 731,107 lines; lean_os itself is 147,389.

### The compiler

Also from its own docs, and this one is decisive rather than merely
large:

> `libc++` is currently the only supported STL. `clang` is the only
> supported compiler.

This project's toolchain is **GCC 14.2 with libstdc++** (M97, M98),
built by `tools/build-toolchain.sh` from a nine-edit port that taught
binutils and GCC the `x86_64-lean_os` triple. Chromium would need that
work done again for clang and libc++, before anything else on this list.

Its build system is `gn` (C++) plus `ninja` (C++) plus `depot_tools`
(Python). Two more ports, both plausible — this machine already builds
C++ and runs CPython 3.12.

### The syscalls — the number that actually decides it

Chromium's own seccomp-bpf sandbox enumerates the syscalls it is
prepared to permit. Counting the distinct `__NR_*` names in
`sandbox/linux/seccomp-bpf-helpers/syscall_sets.cc` and
`baseline_policy.cc`:

```
syscalls Chromium's sandbox names          427
this kernel has                            113
  of which Chromium also names              61   by the same name
  plus                                      18   under a different spelling
                                           ---
  overlap                                   79
  absent                                   348
```

**M118 and M119 move eleven of those 348 and the table is left as it was
measured**, because a number with a date on it should not be quietly
edited later. For the record: this kernel has 126 syscalls now. M118
moved `socketpair`, `sendmsg`, `recvmsg` and `shutdown` - the whole of
Mojo's transport - and M119 moved `epoll_create1`, `epoll_ctl`,
`epoll_wait`, `eventfd2`, `timerfd_create`, `timerfd_settime` and
`timerfd_gettime`, which is the whole of `base`'s message pump. That is
an overlap of **90** and a gap of **337**.

What has *not* moved, and these are the ones that matter: `signalfd4`,
`memfd_create`, `seccomp`, `prctl(PR_SET_SECCOMP)`, and `clone` with
`CLONE_NEWUSER`/`NEWPID`/`NEWNET`. The first two are features; the last
three are the sandbox, which is condition 5 and a design question rather
than a port.

The 18 are real: this kernel spells `wait4` as `SYS_waitpid`,
`rt_sigaction` as `SYS_sigaction`, `getrusage` as `SYS_rusage`,
`getdents64` as `SYS_getdents`, `exit_group` as `SYS_exit`, and so on.
A name-only comparison would have overstated the gap by that much, which
is why it is corrected here.

348 is a large number and it is not the point. **These are:**

| absent | what depends on it |
|---|---|
| `clone` with `CLONE_NEWUSER`/`NEWPID`/`NEWNET` | the sandbox. This kernel has `fork` and **no namespaces of any kind** |
| `seccomp`, `prctl(PR_SET_SECCOMP)` | the sandbox, again — it *is* seccomp-bpf |
| `socketpair`, `sendmsg`/`recvmsg` with `SCM_RIGHTS` | **Mojo**, Chromium's entire IPC layer, which passes file descriptors between processes over an `AF_UNIX` socket. This kernel has **no `AF_UNIX` at all** |
| `epoll_create1`, `epoll_ctl`, `epoll_wait` | `base`'s message pump. This kernel has `poll` and `select` and not this |
| `eventfd2`, `timerfd_create`, `signalfd4` | the same message pump — how a Chromium thread is woken |
| `memfd_create` | shared memory between renderer and GPU process |

By family, the 348 break down as 22 `clock_*`/`timer*`, 14 `sched_*`,
7 `epoll*`, 6 signal-related, 5 namespace-related, 5 socket message
calls, 4 capability/seccomp, 3 `memfd`/`userfaultfd`/`process_vm`, and
3 `mount`/`chroot`/`pivot_root`.

### What its GPU and sandbox layers assume

- **A sandbox built on Linux kernel features this kernel does not have**
  — user namespaces for the layer-1 sandbox, seccomp-bpf for layer 2.
  Neither has an analogue here. lean_os's capability model
  (`docs/capabilities.md`) is a *different* mechanism aimed at the same
  thing, and it is the one NetSurf runs under; it is not a drop-in for
  what Chromium's code calls.
- **GPU compositing.** Chromium's software path exists but its
  architecture assumes a GPU process talking to a driver.
  `docs/capabilities.md`'s deferred list has refused a GPU driver on its
  own terms — *"a driver per vendor per generation"* — and M100 does not
  change that. Software rasterisation into this compositor's framebuffer
  is the answer here, and it is a slow answer rather than a missing one.

### The conclusion, stated as a condition rather than a mood

**A browser is not next, and this measurement is why.** The deferred
list in `milestones.md` said *"HTML, CSS, layout, a JS runtime, TLS, GPU
compositing, codecs, a sandbox, and a Linux-scale syscall surface"*.
M100 has now delivered the first five of those nine, and it delivered
them by porting somebody else's engine rather than by writing one.

What would have to become true for Chromium specifically:

1. **`AF_UNIX` with `SCM_RIGHTS`.** Without descriptor passing there is
   no Mojo, and without Mojo there is no Chromium — not a slow one, not
   a limited one, none. This is the single smallest change with the
   largest effect on the list, it is a few hundred lines, and M100's own
   entry already had `AF_UNIX` as an open box for a different reason
   (CPython's `test_stat`). **It should be the next thing.**

   **Done - M118, 2026-09-11.** `socketpair`, `bind`/`connect` by name
   and by abstract name, `sendmsg`/`recvmsg` with `SCM_RIGHTS`,
   `MSG_TRUNC`/`MSG_CTRUNC`, and a `shutdown` that is real on this
   family. It needs **no capability**, which is not an oversight: a
   renderer must hold no `CAP_NETWORK` and cannot work without this
   call. See [unix-sockets.md](unix-sockets.md).

   And the measurement above understated the case for it, in the
   direction that matters. This is **not Chromium's condition.**
   WebKit's `IPC::Connection`, Gecko's IPDL and Ladybird's LibIPC all
   pass descriptors over a Unix-domain socket too, and none of those
   four engines has a supported single-process mode any more. One
   kernel feature stands in front of every multi-process engine that
   exists, which is why it was built before the engine was chosen
   rather than after.

2. **An epoll-shaped readiness interface**, plus `eventfd`/`timerfd`.
   The message pump is not optional and `poll` is not what it calls.
   **This is now the next thing**, and M118 is why: condition 1 is
   closed, and of the five this is the only other one that is ordinary
   work with a condition attached rather than an arc. `base`'s
   `MessagePumpEpoll` calls `epoll_wait`; this kernel has `poll`,
   `select` and `SYS_waitfds`, and a thread here is woken by none of
   `eventfd`, `timerfd_create` or `signalfd4`.

   **Done - M119, 2026-09-12.** `epoll_create1`, `epoll_ctl`,
   `epoll_wait` with level, edge and one-shot modes; `eventfd` with
   `EFD_SEMAPHORE`; `timerfd` on both clocks at the 10 ms granularity
   this machine's PIT actually has. It also gave this kernel
   **write-readiness**, which it had never had: `EPOLLOUT` means a pipe
   with room rather than `<poll.h>`'s "anything open". `signalfd` is not
   built and `base` does not call it. See
   [readiness.md](readiness.md). **The instrument worth naming**: the
   self-test measures `SYS_idle_ticks` across a 200 ms `epoll_wait`, so
   "it sleeps rather than spinning" is a number - which is the failure
   this work was most likely to ship and the one no unit test can see.

3. **clang and libc++ for `x86_64-lean_os`.** A second toolchain port,
   with M94's nine edits as the template for how much that costs.
   **With conditions 1 and 2 closed, this is where the arc stops being
   ordinary work.** The two that are done were a few hundred lines each
   of kernel; this is a compiler.
4. **A machine with 16 GB of RAM and 100 GB of disk**, which is a
   statement about M110's hardware rather than about this code.
5. Then a sandbox story, which is a design question rather than a port:
   Chromium's code calls seccomp and namespaces, and this OS's answer to
   "what may this process do" is a capability set assigned at spawn.
   Those are not the same shape, and pretending otherwise would produce
   exactly the kind of thing M65 refused.

Items 1 and 2 are ordinary work with conditions attached, which is what
this project means by a deferral. Items 3 to 5 are an arc.

### And Google Chrome, which is a different question

Chrome is not Chromium. It is **proprietary**: there is no source to
hand to `x86_64-lean_os-gcc`, and the only artifacts Google ships are
ELF binaries dynamically linked against glibc, GTK, X11, dbus and
PulseAudio. Running one would mean shipping somebody else's binary in
the image, which the first non-negotiable in CLAUDE.md forbids outright,
and it would need a glibc ABI this project has deliberately not built.

Chromium is a hundred-gigabyte checkout and five conditions. Chrome is
not a porting problem at all.
