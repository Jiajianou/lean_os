/* system_api/include/caps.h
 *
 * M65: what a process is allowed to do.
 *
 * Every prior milestone that came near this question declined to answer
 * it, in writing, and was right to: `SYS_shutdown`'s comment calls a
 * check with nothing behind it "the same fake check the task manager's
 * own desktop-process guard is careful not to make". For a single-user
 * desktop running only programs from this repo, "any process can do
 * anything" is a defensible position, and pretending otherwise would
 * have been worse than admitting it.
 *
 * Two milestones ended that. M63 made it possible to run a program
 * nobody here wrote. M64 made it possible for that program to open a
 * socket and talk to anything on the network. The stretch-goal entry
 * said this should be revisited "the moment a ported program is
 * something a person downloads rather than something this repo builds",
 * and both halves of the sentence it was waiting on have now happened.
 *
 * ## The model, and why it is this one
 *
 * There are no users here. There is no login, no uid, no owner on a
 * file, and inventing one would be a much larger lie than the one being
 * fixed - a uid nothing sets and nothing checks is decoration. So this
 * is a **capability** model rather than a permission one, and it rests
 * on the one relationship this OS genuinely has: **a process has a
 * parent, and the parent chose to start it.**
 *
 *   - A capability set is a bitmask, carried per process.
 *   - `init` (PID 1) starts with all of them.
 *   - A child gets its parent's set, minus whatever the parent chose to
 *     withhold at spawn time.
 *   - **A process can drop its own capabilities and can never regain
 *     them.** Monotonic, with no call that grants anything - which is
 *     the single property that makes the whole thing checkable, because
 *     it means no code path anywhere has to be trusted to hand a
 *     capability back.
 *
 * That is the entire mechanism. It is not a security boundary against a
 * hostile kernel exploit and does not claim to be; it is a boundary
 * against a program doing something its launcher never intended, which
 * is the failure a downloaded program actually presents.
 *
 * ## What it is worth
 *
 * The compositor owns the screen. Before this, so did every program the
 * compositor launched: `SYS_fb_map` was available to all of them, and a
 * text editor could paint over the taskbar. Now the compositor keeps
 * CAP_FRAMEBUFFER and drops it from everything it starts, so a client
 * cannot draw outside its own window even by mistake - and that is a
 * property you can photograph, which is why M65's self-test does.
 */
#pragma once

#include <stdint.h>

/* Draw straight to the screen (SYS_fb_info, SYS_fb_map). The compositor
 * has it; nothing the compositor launches does. */
#define CAP_FRAMEBUFFER   (1u << 0)

/* Signal a process that is not one of this process's own descendants.
 * A parent can always kill its own children with or without this - the
 * relationship that gives you the pid is the one that entitles you. */
#define CAP_KILL_ANY      (1u << 1)

/* Switch the machine off or reboot it (SYS_shutdown). */
#define CAP_POWER         (1u << 2)

/* Read and write the shared clipboard. Read is the interesting half:
 * a clipboard a program can read at will is a keylogger with a delay. */
#define CAP_CLIPBOARD     (1u << 3)

/* Open a socket (SYS_socket) and therefore talk to anything. */
#define CAP_NETWORK       (1u << 4)

/* Claim the machine's sound devices (SYS_audio_claim). */
#define CAP_AUDIO         (1u << 5)

/* Change the display mode (SYS_display_set_mode). Separate from
 * CAP_FRAMEBUFFER because they are different kinds of authority: one
 * paints the screen, the other reconfigures the hardware under everyone
 * else's feet. */
#define CAP_DISPLAY_MODE  (1u << 6)

/* Enumerate every process on the machine (SYS_taskinfo). The task
 * manager has it. */
#define CAP_PROCESS_LIST  (1u << 7)

/* Modify the filesystem: create, write, truncate, unlink, rename,
 * mkdir, rmdir. Reading is not a capability - this OS has no secrets on
 * disk and claiming a read boundary it does not enforce would be exactly
 * the fake check this file exists to avoid. */
#define CAP_FS_WRITE      (1u << 8)

/* Set the system clock (SYS_settime). */
#define CAP_SET_TIME      (1u << 9)

/* M70: read the kernel log. Gated, and the reasoning is M65's own: the
 * log carries driver addresses, the arguments of other processes' failed
 * syscalls, and every capability denial on the machine - which is a
 * description of what everybody else is doing. Reading it is authority,
 * not a fact about the machine, so unlike SYS_fb_info and SYS_netconf it
 * gets a gate. */
#define CAP_SYSLOG        (1u << 10)
#define CAP_ALL           0x7FFu

/* What an ordinary desktop application gets: it can read and write
 * files, and that is nearly all. No screen, no other processes' lives,
 * no clipboard it did not ask a person for, no network, no sound, no
 * clock, no power switch. Everything a windowed program actually needs
 * it gets by *asking the compositor* over the WM protocol, which is a
 * request another process can refuse rather than a thing it can do. */
#define CAP_APP_DEFAULT   (CAP_FS_WRITE)

/* Every capability name, for anything that has to show them to a
 * person - the task manager's column, and `caps` on the command line.
 * Kept next to the bits so a new one cannot be added without a name. */
typedef struct {
    uint32_t bit;
    const char *name;
} cap_name_t;

static const cap_name_t CAP_NAMES[] = {
    {CAP_FRAMEBUFFER,  "framebuffer"},
    {CAP_KILL_ANY,     "kill-any"},
    {CAP_POWER,        "power"},
    {CAP_CLIPBOARD,    "clipboard"},
    {CAP_NETWORK,      "network"},
    {CAP_AUDIO,        "audio"},
    {CAP_DISPLAY_MODE, "display-mode"},
    {CAP_PROCESS_LIST, "process-list"},
    {CAP_FS_WRITE,     "fs-write"},
    {CAP_SET_TIME,     "set-time"},
    {CAP_SYSLOG,       "syslog"},
};

#define CAP_NAME_COUNT ((int)(sizeof(CAP_NAMES) / sizeof(CAP_NAMES[0])))

/* ---- What each shipped program is granted ----------------------------
 *
 * This OS's equivalent of an application manifest, and it lives here
 * rather than inside the compositor because the compositor is not the
 * only thing that launches programs - the shell does too, and a table
 * only one launcher consulted would be a rule with a way around it.
 *
 * A program not listed gets CAP_APP_DEFAULT. That is the important
 * direction of the default: adding a program grants it nothing, and a
 * program that needs more has to be written down here, where the list
 * of everything unusual on the machine is one screen long and can be
 * read by a person.
 *
 * Note what is *not* in this table: `text_editor`, `file_manager`,
 * `gui_paint`, `gui_clock`, every coreutil. They are the majority, and
 * they need nothing beyond writing files - because everything a windowed
 * program does to the screen it does by asking the compositor over the
 * WM protocol, which is a request another process can refuse rather
 * than a thing it can do.
 */
typedef struct {
    const char *name; /* the program's basename, as spawned */
    uint32_t caps;
} cap_grant_t;

static const cap_grant_t CAP_GRANTS[] = {
    /* PID 1, which starts the compositor and the shell. It has to be
     * able to hand out everything they need, because a spawn can only
     * ever narrow - an `init` with less than CAP_ALL would be a machine
     * whose compositor could not draw. */
    {"init",         CAP_ALL},
    /* Owns the screen, the sound devices and the machine's shutdown
     * path, and starts everything else - so it needs everything it might
     * ever hand out. This is the one entry that is broad, and it is
     * broad because the compositor *is* the trusted launcher. */
    {"compositor",   CAP_ALL},
    /* Notably *not* here: `desktop_shell`. The taskbar has a Start
     * menu with "Shut down" on it and holds no power capability at all,
     * because it does not switch the machine off - it asks the
     * compositor to, over the WM protocol. That is the pattern almost
     * every program on this desktop follows, and the reason the grant
     * table below is as short as it is. */
    {"shutdown",      CAP_APP_DEFAULT | CAP_POWER},
    {"reboot",        CAP_APP_DEFAULT | CAP_POWER},
    /* Shows every process and ends the one you pick. Both halves are
     * capabilities and it is the only shipped program that has either. */
    {"task_manager",  CAP_APP_DEFAULT | CAP_PROCESS_LIST | CAP_KILL_ANY},
    /* Changes the resolution, which reconfigures the display hardware
     * under every other program's feet. */
    {"settings",      CAP_APP_DEFAULT | CAP_DISPLAY_MODE | CAP_CLIPBOARD},
    /* A shell can start anything, so it needs to be able to *hand out*
     * anything - it re-applies this same table to each program it
     * launches, so what it holds is a ceiling rather than a grant to the
     * things it starts. */
    /* M72: renamed from "shell". The program is /bin/sh now, because
     * that is what a `#!` line says and a shell whose name does not match
     * the convention every script in the world uses is a shell scripts
     * cannot name. */
    {"sh",            CAP_ALL},
    {"gui_terminal",  CAP_ALL},
    /* ---- the desktop's icon grid, and the bug its absence caused -------
     *
     * Same argument as the two above, missed for five milestones with a
     * real consequence. `desktop_icons` launches programs the same way a
     * shell does - a plain SYS_spawn - and a child's set is its parent's
     * set intersected with this table. It was absent here, so it held
     * CAP_APP_DEFAULT, so **every program launched by double-clicking a
     * desktop icon was capped at CAP_FS_WRITE no matter what this table
     * said about it.** The manifest was dead letter for the whole
     * desktop: the editor could not read the clipboard, the task manager
     * could not list processes, Settings could not change the resolution.
     *
     * The boot self-tests never saw it because they spawn from
     * kernel_main, which holds everything, and the Start menu never saw it
     * because that goes through the compositor's launcher. Only the icon
     * grid was affected, and it surfaced as "Ctrl+V does nothing in the
     * editor" - four programs away from the cause.
     *
     * What it holds is a CEILING, not a grant to what it starts: the
     * kernel still applies this table to every child, so this lets it
     * hand out what the table already allows and nothing more. The honest
     * cost is that this process can itself call privileged syscalls -
     * a real widening, and the same one already accepted for `sh` and
     * `gui_terminal`.
     *
     * The cleaner shape is for a launch to be a *request to the
     * compositor*, the way desktop_shell's Start menu already works and
     * the way it asks for a shutdown - the compositor is the trusted
     * launcher and this program would then need nothing at all. That is a
     * WM-protocol change rather than a table entry, and it is the right
     * follow-up. */
    {"desktop_icons", CAP_ALL},
    /* The network programs, and the reason CAP_NETWORK is worth having:
     * these two are the entire list of things on this machine that may
     * talk to anything. `netconf` is deliberately *not* one of them -
     * SYS_netconf reports the machine's own address and is not gated, the
     * same call SYS_fb_info makes about the screen's size. Knowing how
     * big the screen is and knowing what address this machine has are
     * facts, not authority, and gating a fact is the kind of check that
     * looks like security and is not. */
    {"nettest",       CAP_APP_DEFAULT | CAP_NETWORK},
    {"tcptest",       CAP_APP_DEFAULT | CAP_NETWORK},
    /* M67: needs the socket table to hammer, and nothing else beyond the
     * default - the other three subsystems it races (fs, shm, pipes) are
     * ungated, which is itself the manifest saying something true. */
    {"racetest",      CAP_APP_DEFAULT | CAP_NETWORK},
    {"nettime",       CAP_APP_DEFAULT | CAP_NETWORK | CAP_SET_TIME},
    /* Claims the sound devices directly - the one program that does,
     * and the one that demonstrates the claim being refused. */
    {"audiograb",     CAP_APP_DEFAULT | CAP_AUDIO},
    /* Paste. Two programs on this machine can read the clipboard and
     * this table is where you find out which - which is most of the
     * value of writing it down, because "a clipboard any program can
     * read whenever it likes" is a keylogger with a delay. */
    {"text_editor",   CAP_APP_DEFAULT | CAP_CLIPBOARD},
    /* The self-test programs for earlier milestones, which assert things
     * about syscalls rather than about capabilities. badptr walks the
     * entire syscall surface with deliberately bad pointers and needs
     * every call to get far enough to reject one; wm_crash reads the
     * process list to find the compositor. Granted explicitly and
     * narrowly rather than by making the default permissive. */
    {"badptr",        CAP_ALL},
    {"wm_crash",      CAP_APP_DEFAULT | CAP_PROCESS_LIST | CAP_KILL_ANY},
    /* The self-test program for this milestone. It is granted nothing
     * beyond the default *on purpose*: what it asserts is that the
     * things it is not allowed to do fail. */
    /* M70: the log viewer. The one program on this machine that reads the
     * kernel's own account of itself - which is why it is the only entry
     * here with CAP_SYSLOG, and why the capability exists at all. */
    {"console",       CAP_APP_DEFAULT | CAP_SYSLOG},
    {"captest",       CAP_APP_DEFAULT},
};

#define CAP_GRANT_COUNT ((int)(sizeof(CAP_GRANTS) / sizeof(CAP_GRANTS[0])))

/* The capabilities `path` should be launched with. Takes a full path or
 * a bare name; only the part after the last '/' is matched, because
 * "/bin/settings" and "settings" are the same program and a table keyed
 * on the spelling would be a table with a way around it. */
static inline uint32_t caps_for_program(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    for (int i = 0; i < CAP_GRANT_COUNT; i++) {
        const char *a = CAP_GRANTS[i].name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            return CAP_GRANTS[i].caps;
        }
    }
    return CAP_APP_DEFAULT;
}
