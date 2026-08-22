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
build/leanfs-put build/os-image.bin build/myapp.elf myapp   # 4. write it onto the disk
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
directly and writes a file into an existing disk image's filesystem
region. It duplicates the two on-disk structs from
[`kernel/fs/leanfs.c`](../kernel/fs/leanfs.c) byte-for-byte (see that
file if the format ever changes) and implements the same allocate/free
logic, so a file it writes reads back through the kernel's own
`leanfs_read` exactly as if the kernel had written it itself.

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

- **72 KiB max file size** (`LEANFS_MAX_FILE_SIZE` - 16 direct blocks +
  one 128-pointer indirect block, all 512-byte blocks). Plenty for a
  coreutils-sized program; not for anything with large embedded assets.
- **32 files total** (`LEANFS_MAX_INODES`), shared with every built-in
  program - `make preseed` alone uses 14 of them.
- **27-character filenames** (`LEANFS_MAX_NAME`).
- leanfs is flat (no subdirectories) - your program's name has to be
  unique across the whole disk.
