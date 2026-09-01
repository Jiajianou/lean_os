# Building and running your own program on lean_os

Every program this project ships itself (`hello`, `ls`, `gui_paint`, ...)
gets onto disk the same way: baked into `kernel.bin` as an incbin'd blob
(`kernel/proc/embed_programs.asm`) and seeded onto
[leanfs](../kernel/fs/leanfs.h) the first time the kernel boots. That's
fine for programs this repo owns, but it means writing one has always
meant editing three places at once — the Makefile's `USER_PROGRAMS`
list, `embed_programs.asm`, and `kernel.c`'s
`FOR_EACH_EMBEDDED_PROGRAM` — none of which a third-party author should
have to touch just to try their own program.

This is the other way: compile your program with the same toolchain,
then write it straight onto an already-built disk image without
rebuilding the kernel at all.

## The workflow

```sh
make all                                          # 1. build the OS as usual
make preseed                                      # 2. one time per disk image, see below
tools/build-user-program.sh path/to/myapp.c myapp # 3. compile against user_space/lib
build/leanfs-put build/os-image.bin build/myapp.elf /bin/myapp  # 4. write it onto the disk
                                                  #    (-r copies a whole tree; see step 4)
make run                                          # 5. boot - 'myapp' is just another file now
```

From here `myapp` is indistinguishable from any built-in program: `ls`
finds it, the shell/launcher spawns it the same way, `SYS_spawn` loads
it the same way. Nothing in the kernel or the Makefile's `USER_PROGRAMS`
list needs to know it exists.

## Step 3: `tools/build-user-program.sh`

Compiles a single `.c` file with the exact flags and against the exact
`user_space/lib` objects (`crt0`, `syscall_wrappers`, `str`, `malloc`,
`gfx`, `font8x16`, `wmclient`) every one of this project's own programs
builds with — pulled live from the Makefile (`make print-USER_CFLAGS`),
not a second hardcoded copy that could drift. Your program is ordinary
freestanding C against that API (see [`user_space/bin/hello.c`](../user_space/bin/hello.c)
for the smallest real example) — no libc, same as everything else here.

## Step 4: `tools/leanfs-put.c`

A small host program (ordinary hosted C, built with your system `cc` —
it never runs as part of the OS) that speaks leanfs's on-disk format
directly and writes into an existing disk image's filesystem region,
using the same allocate/free logic the kernel does — so a file it writes
reads back through `leanfs_read` exactly as if the kernel had written it.

It gets the on-disk format from
[`kernel/fs/leanfs_format.h`](../kernel/fs/leanfs_format.h), the header
the kernel itself includes. It used to carry its own hand-copied structs
under a comment saying they had to match byte for byte; they did not, and
had not since M93 — see Q3 in [milestones.md](../milestones.md) for what
that cost and why there is now one definition rather than two.

**A whole directory tree, not just a file** (M93):

```sh
build/leanfs-put -r build/os-image.bin ./my-source-tree /src
```

Copies the tree in, preserving symbolic links, hard links and
modification times, and writes `/.image-manifest` describing what it put
there. The machine checks that manifest against its own walk of the tree
on the next boot with self-tests enabled, and
[`tools/image-tree-test.sh`](../tools/image-tree-test.sh) is the harness
that does both halves — see M93's second-attempt notes. This is the only
way a source tree gets onto this disk: every other road runs through
`kernel/proc/embed_programs.asm`, and a tarball cannot be `incbin`'d into
a kernel.

## Why `make preseed` first

`kernel.c`'s own boot self-tests hardcode some assumptions about file
*order* - most notably, the M22 desktop-shell self-test checks actual
rendered pixels assuming "launcher slot 0 is `hello`, the first file
ever seeded." That assumption holds because `hello` is first in
`FOR_EACH_EMBEDDED_PROGRAM` and the kernel seeds in that order on a
truly fresh disk.

If you run `leanfs-put` on a disk image that has *never* booted, your
program claims the first free inode for itself - and if that's inode 0,
it takes the slot the self-test expects `hello` to be in, and the
kernel panics on its own boot self-test the next time you boot that
image (not a bug in your program; the self-test's hardcoded position
assumption is what breaks).

`make preseed` sidesteps the question entirely: it writes every
built-in program onto the image via `leanfs-put` itself, in the same
order the kernel would seed them in, *before* you add anything of your
own. Do that once per disk image and every third-party program you add
afterward lands safely past all of them. It's not part of `all` and
never runs automatically — default boot behavior (the kernel seeding
these itself on first boot) is completely untouched either way, since
that seeding loop already skips any program that's already present.

## Limits worth knowing

*These were 72 KiB, 32 files, 27-character names and "no subdirectories"
until M93, which is four separate numbers this page went on repeating
after each of them had moved. They are read off
`kernel/fs/leanfs.h` as of M93:*

- **4 GiB max file size** (`LEANFS_MAX_FILE_SIZE`), capped by the
  inode's 32-bit `size` field rather than by the block tree, which
  reaches a little further.
- **131072 files total** (`LEANFS_MAX_INODES`), shared with every
  built-in program — `make preseed` uses about fifty of them. The cap is
  the inode table being held in memory in full (16 MiB); past this, the
  answer is not a bigger array but reading inodes through the block
  cache, which is work with a trigger rather than a date.
- **2 GiB data region** (`LEANFS_DATA_BLOCKS` at 4 KiB blocks). The image
  file is sparse, so this costs what is actually stored.
- **255-byte filenames** (`LEANFS_MAX_NAME`) and **4096-byte paths**
  (`LEANFS_MAX_PATH`).
- Directories, since M53. Names are unique within a directory, not across
  the disk.

---

# M63: porting a program nobody here wrote

Everything above is about *your* program, written against this project's
own API. This section is about the other case, and it is the one M63
exists for: source that was written years before this OS and knows
nothing about it.

## What is in the tree

```
third_party/whetstone/whetstone.c   the port
user_space/libc/include/            <stdio.h>, <stdlib.h>, <string.h>,
user_space/libc/src/                <math.h>, <time.h>, <ctype.h>, <assert.h>
```

`third_party/` is the boundary. Nothing in it is edited to suit this OS
beyond what is listed below, nothing from it moves into `kernel/` or
`user_space/lib/`, and it is built with its own flags
(`THIRD_PARTY_CFLAGS` in the Makefile) — the same as everything else
except that `-Werror` is off, because treating a 1998 program's warnings
as errors would mean editing somebody else's program to make it build.

## What was ported, and what was changed

**`third_party/whetstone/whetstone.c`** — the C translation (Rich
Painter, Painter Engineering, 1998) of the Whetstone benchmark (Curnow
and Wichmann, 1972). Its licence permits use and redistribution provided
the original comment block travels with it, which it does, unmodified.

**Changes made to it: none.** It compiles and runs exactly as
downloaded. That is worth stating explicitly, because it is the actual
claim being made: the program was not adapted to lean_os, lean_os was
made able to run the program.

What it needed, which is what `user_space/libc` now provides:

| Header      | What Whetstone calls                               |
|-------------|----------------------------------------------------|
| `<math.h>`  | `sin`, `cos`, `atan`, `log`, `exp`, `sqrt`         |
| `<stdio.h>` | `printf` with `%ld`, `%.1f`, `%12.4e`; `fprintf`   |
| `<stdlib.h>`| `atol`                                             |
| `<string.h>`| `strncmp`                                          |
| `<time.h>`  | `time(0)`                                          |

Its link errors were the specification, exactly as M63 planned. Nothing
in `user_space/libc` was written speculatively for a caller that does not
exist — the parts beyond this table (`realloc`, `strstr`, `snprintf`
truncation semantics, `pow`, `atan2`) are there because the *next*
program will want them and because leaving out half of a header is worse
than having one.

## The libc subset is ours

The ground rules say no external library is linked into anything this OS
ships, and that is still true. `user_space/libc` is written here; it is
the opposite of linking newlib. Two properties of it are worth knowing
before adding to it:

- **`memcpy`, `memset`, `strlen` and `strcmp` are not in it.** They come
  from `user_space/lib/str.c`, where they have always been. Two copies of
  `memcpy` in one binary is exactly the kind of thing that quietly
  diverges.
- **The accuracy of `math.h` is a claim this project can fail**, not a
  hope: roughly 1e-12 relative, and `user_space/bin/libctest.c` checks it
  against known values on every boot. Trigonometry reduces to quadrants
  with a Cody–Waite split — reducing only to [-pi, pi] and running a
  Taylor series is accurate where nobody calls it and wrong at
  `cos(-40)`, which is how the first version failed its own test.

## What a ported program can and cannot have

- **Floating point: yes.** `-mgeneral-regs-only` is gone from
  `USER_CFLAGS` and every task carries its own FXSAVE area
  (`kernel/arch/x86_64/fpu.h`). The kernel keeps the flag, which is what
  makes this cheap: only a task switch has to save SSE state, never an
  interrupt.
- **A real `argv`: yes** (M60), including argument counts and quoting
  from the terminal.
- **Files: yes** (M59) — `fopen`/`fread`/`fseek` over real descriptors,
  and files up to 8 MiB.
- **Memory: `SYS_sbrk`, and since M78 an `mmap`/`munmap` arena too.** A
  process's address-space layout is still fixed in `kernel/proc/proc.h`,
  but a mapping can now be *given back*, and `malloc` routes anything
  over 64 KiB through one so that a large allocation freed is a page
  freed. Every mapping is backed by a real frame at the moment of the
  call - there is no page-fault handler that could fill one in later -
  so a program that needs tens of megabytes is still a program to check
  the budget for *before* starting the port, not half-way through.
- **A window: through `wmclient.h`, like everything else.** Whetstone is
  a console program and runs in the terminal, which is a window on this
  desktop - the claim being made is "this desktop runs somebody else's
  program alongside its own", not "this program took over the screen".
- **Threads: yes** (M79) — `<pthread.h>` with create/join/detach, a
  mutex, a condition variable, `pthread_once` and thread-specific
  storage, over two of the scheduler's tasks sharing one page table.
  Threads share memory and share the descriptors that were open when
  they were created; one opened afterwards is not visible to them.
- **Signals: yes** (M76) — `<signal.h>`'s `signal`, `sigaction`, `kill`,
  `raise` and a block/unblock mask, with handlers invoked in ring 3
  through a frame the kernel writes onto the process's own stack. A
  caught signal is delivered on the way back from a syscall, so a
  program in a pure compute loop that never syscalls does not run its
  handler until it does.
- **`struct tm`, `localtime`, `strftime`: yes** (M80 groundwork), with
  one honest caveat: `localtime` and `gmtime` are the same function,
  because this machine keeps UTC and knows of no other zone.
- **Sockets: yes** (M64/M66) — as ordinary file descriptors, gated on
  `CAP_NETWORK`. There is no `<sys/socket.h>`; the interface is
  `system_api/include/os_net.h`, which is a different spelling of the
  same idea and would be a thin header away from the POSIX one.
- **`scanf`: no.** Still absent because nothing has asked. A program
  that wants one says so at link time, which is the specification - and
  that rule is what produced the whole M80 list: thirteen headers and
  several hundred lines of libc, every one of them named by CPython's
  own source refusing to compile rather than guessed at in advance.
- **`fork`, `exec`, `dlopen`: no, and not coming soon.** `SYS_spawn` is
  a combined fork+exec by design (M13) and there is no dynamic loader
  (see "Deliberately not next" in milestones.md). A program built around
  `fork` needs a decision, not a header.
