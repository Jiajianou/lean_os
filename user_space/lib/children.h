/* user_space/lib/children.h
 *
 * M54: "reap what you spawn", for the two long-lived processes in this
 * project that launch things and then forget about them.
 *
 * A task slot comes back when its exit status is consumed - see
 * kernel/sched/sched.h's sched_reap_slot for why that, and not exiting,
 * is the trigger. The boot self-tests always waited on what they spawned,
 * so recycling made the task table stop being a lifetime budget for
 * *them* immediately. The desktop did not: an app launched from an icon
 * or the launcher had no parent that ever called wait, so closing it left
 * a slot claimed for the machine's uptime - which is precisely the ceiling
 * M54 exists to remove.
 *
 * Deliberately only the *parent* reaps. It would have been less code for
 * the compositor to reap every client whose window it reclaims, since it
 * already notices those deaths - but it is not their parent, and a
 * non-parent consuming an exit status races with whoever is: the boot
 * self-tests kill a client and then wait for it, and a slot recycled in
 * between could have been handed to somebody else by the time that wait
 * (or the SIGKILL before it) named a pid. A parent reaping its own
 * children has no such race.
 */
#pragma once

/* Records a pid returned by sys_spawn. Silently ignores a negative pid
 * (a spawn that failed has nothing to reap) and a full table - the worst
 * case of the latter is the old behavior, one slot not coming back. */
void child_track(long pid);

/* Reaps whichever tracked children have exited, without blocking on the
 * ones that haven't. Cheap enough to call once per main-loop iteration:
 * at most CHILD_MAX non-blocking syscalls, and usually zero live entries
 * at all. */
void child_reap(void);
