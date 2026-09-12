# Capabilities

What a process on this machine is allowed to do, why the model is this
shape, and what it does not claim.

Added in **M65**, after four earlier milestones declined to add one. Those
refusals were correct at the time and are worth reading in
`milestones.md`: a check with nothing behind it is worse than an honest
admission that there is no check.

## The model

There are **no users** here. No login, no uid, no owner on a file. So
this is a *capability* model rather than a permission one, and it rests
on the one relationship this OS genuinely has: **a process has a parent,
and the parent chose to start it.**

```
kernel (task 0)              CAP_ALL
  └── init                   CAP_ALL           (manifest)
        ├── compositor       CAP_ALL           (manifest)
        │     ├── settings   default + display-mode + clipboard
        │     ├── text_editor default + clipboard
        │     ├── file_manager default
        │     └── gui_paint  default
        └── shell            CAP_ALL           (manifest)
              ├── nettime    default + network + set-time
              └── hello      default
```

Four rules, and there is no fifth:

1. Task 0 holds every capability. Everything else descends from it.
2. A child starts with **its parent's set**, intersected with what the
   manifest grants its program.
3. A process can **drop** its own capabilities (`sys_dropcaps`).
4. **Nothing grants a capability to a running process.** There is no such
   call. `sys_dropcaps` is an `AND`; the spawn path is an `AND`.

Rule 4 is what makes this checkable rather than merely present: no code
path anywhere has to be *trusted* to hand a capability back, because none
can.

## The manifest

`system_api/include/caps.h` names every shipped program that needs more
than `CAP_APP_DEFAULT`. Everything not listed gets the default, which is
`CAP_FS_WRITE` and nothing else.

The table lives in the **system API** and is applied by the **kernel**,
in `process_spawnv`. Both of those are deliberate:

- In the system API rather than in the compositor, because the shell
  launches programs too, and a table only one launcher consulted would be
  a rule with a way around it.
- Applied in the kernel rather than passed in by a launcher, because a
  launcher that forgot to ask would be that same way around. The way to
  not have that problem is to not give launchers the choice.

Adding a program grants it nothing. A program that needs more has to be
written down, in a list one screen long that a person can read.

## The capabilities

| Capability | Gates | Held by |
|---|---|---|
| `framebuffer` | `SYS_fb_map` | compositor |
| `kill-any` | `SYS_kill` on a non-descendant | task_manager, wm_crash |
| `power` | `SYS_shutdown` | shutdown, reboot |
| `clipboard` | `SYS_clipboard_get`/`_set` | text_editor, settings, gui_terminal |
| `network` | `SYS_socket` with `OS_AF_INET` | nettest, nettime |
| `audio` | `SYS_audio_claim` | audiograb |
| `display-mode` | `SYS_display_set_mode` | settings |
| `process-list` | `SYS_taskinfo` | task_manager, wm_crash |
| `fs-write` | `SYS_writefile`, `unlink`, `rename`, `mkdir`, `rmdir`, `open` with a write flag | everything |
| `set-time` | `SYS_settime` | nettime |

(`init`, `compositor`, `shell` and `gui_terminal` hold `CAP_ALL` - they
are launchers, and a launcher can only ever hand out what it holds.)

## What is deliberately not gated

- **Reading a file.** This OS has no secrets on disk, and claiming a read
  boundary nothing enforces would be exactly the fake check M65 exists to
  stop making.
- **Talking to another process on this machine** (M118). A pipe needs no
  capability, and an `AF_UNIX` socket reaches exactly what a pipe reaches:
  another process here that is already listening for it. `SYS_socket`
  checks `network` for `OS_AF_INET` and nothing for `OS_AF_UNIX`, and that
  asymmetry is the point rather than a gap — the program this family
  exists for is a browser's renderer, which must hold **no** network
  capability and cannot work without the call. What the model still
  decides is what a passed descriptor carries: exactly the authority it
  already had, because what crosses is a reference to the same kernel
  object. A process cannot manufacture authority by sending it, and it
  *can* hand over authority it holds, which is what `SCM_RIGHTS` is. A
  capability set still only ever shrinks. See
  [unix-sockets.md](unix-sockets.md).
- **`SYS_fb_info`** - how big is the screen.
- **`SYS_netconf`** - what is this machine's address.

Those last two are *facts*, not authority. Gating a fact is the kind of
check that looks like security and is not.

## Signals have a relationship in them

`SYS_kill` is the one gate that is not a plain bit test. **A parent may
always end what it started**, capability or not - the relationship that
gave you the pid is what entitles you to use it, and a launcher that
cannot stop its own children is not a launcher. The check walks the whole
parent chain, not one level: a shell that spawned a program that spawned
a program is still the reason all three are running.

Anything else needs `CAP_KILL_ANY`.

## Denials are logged

Once per process per capability:

```
[caps] captest was refused 'framebuffer' - see system_api/include/caps.h
```

Silent refusal is how a permission model turns into an unexplained bug.
The program sees `-1` from a call that has ten other reasons to return
`-1`, and whoever is debugging it has nothing to go on.

## Seeing it

```sh
caps        # what this process holds
caps -a     # every capability that exists, held or not
```

## What this is not

It is **not** a defence against a kernel exploit. Nothing here stops a
process writing to a page it should not have - that is `vmm`'s job, and
M52's. What it stops is a program doing something its *launcher* never
intended, which is the failure a downloaded program actually presents,
and the only one a model with no users can honestly claim to address.
