/* kernel/ipc/timerfd.h
 *
 * M119: a deadline you can wait on, as a descriptor.
 *
 * The second half of docs/browser.md's second condition. A message pump
 * does two things: it waits for descriptors, and it wakes up at a time.
 * `epoll_wait` has a millisecond timeout, so a pump *can* do the second
 * with the first - and every pump that has to handle more than one timer
 * stops doing that, because recomputing a single timeout from a heap of
 * deadlines on every pass is exactly the bookkeeping a timer descriptor
 * removes. Chromium's `MessagePumpEpoll` owns a `timerfd`; so does glib's
 * main loop.
 *
 * ---- what it can honestly promise -------------------------------------
 *
 * **A 10 ms granularity, and that is the machine rather than this file.**
 * PIT_HZ is 100 (kernel/drivers/pit.h), so the only clock here advances in
 * 10 ms steps, and a timer asked for 1 ms fires on the next tick. The ABI
 * carries nanoseconds because `struct itimerspec` does and because a
 * coarser ABI would have to be widened the day the clock gets finer; what
 * it does NOT do is pretend. A timer set for less than one tick is
 * rounded UP to one, never down to zero, because a timerfd that is
 * readable immediately is a pump that spins.
 *
 * **Expirations are counted, not dropped.** A read returns how many times
 * the timer fired since the last read, which is what makes a 10 ms
 * periodic timer honest on a machine that was busy for 100: the reader
 * learns it missed nine rather than being told about one. Linux reports
 * the same number for the same reason.
 *
 * ---- why every call takes `now_ms` ------------------------------------
 *
 * Because a timer whose clock is a global is a unit that cannot be tested.
 * The syscall layer reads the clock once per call and passes it in, so
 * every function here is a pure function of (state, time) and
 * tests/test_readyfds.c can ask what happens at 999 ms and at 1000 ms
 * without a machine, a tick or a sleep. tcp.c reads the clock itself and
 * its tests pay for that with a fake PIT whose every read advances time;
 * this file is the other choice, made deliberately after seeing that bill.
 */
#pragma once

#include <stdint.h>

#define TIMERFD_MAX 64

/* Which clock a timer's absolute deadlines are against. The relative case
 * is identical for both - "500 ms from now" does not care - and the
 * difference only appears under TFD_TIMER_ABSTIME. */
#define TIMERFD_CLOCK_REALTIME  0
#define TIMERFD_CLOCK_MONOTONIC 1

struct timerfd;

void timerfd_init(void);

/* `clockid` is one of the two above. One reference, the caller's fd slot
 * takes it. A fresh timer is disarmed: it never fires until settime. */
struct timerfd *timerfd_create(int clockid);

void timerfd_ref(struct timerfd *t);
void timerfd_unref(struct timerfd *t);

int timerfd_clock(const struct timerfd *t);

/* Arms, re-arms or disarms. `value_ns` of 0 disarms (and an interval
 * alone cannot arm anything, which is POSIX's rule and a trap worth
 * naming: `{0, interval}` is a disarm, not a periodic timer).
 * `interval_ns` of 0 is one-shot. `absolute` non-zero means `value_ns` is
 * a point on this timer's clock rather than a distance from now, and
 * `now_ns` is what that clock reads at the moment of the call.
 *
 * Writes what the timer would have reported to `old_value_ns` and
 * `old_interval_ns` if they are non-NULL - the remaining time, not the
 * original setting, which is the only form that lets a caller restore a
 * timer it borrowed.
 *
 * Arming CLEARS any expirations nobody has read. A re-armed timer that
 * still reported the old one's firings would be a timer reporting the
 * past. */
int timerfd_settime(struct timerfd *t, uint64_t now_ns, int absolute,
                    uint64_t value_ns, uint64_t interval_ns,
                    uint64_t *old_value_ns, uint64_t *old_interval_ns);

/* What is left. `value_ns` is 0 for a disarmed timer, which is how a
 * caller tells a disarmed timer from a one-shot that has not fired. */
void timerfd_gettime(const struct timerfd *t, uint64_t now_ns,
                     uint64_t *value_ns, uint64_t *interval_ns);

/* Takes the expiration count into `out` and resets it to zero. Returns 0,
 * or -1 when it is zero - "nothing yet". Never blocks. `now_ns` brings
 * the timer forward first: a timer nobody asked about has still fired. */
int timerfd_read(struct timerfd *t, uint64_t now_ns, uint64_t *out);

/* Has it fired? The readable condition epoll and SYS_waitfds ask about. */
int timerfd_readable(struct timerfd *t, uint64_t now_ns);

/* When this timer next needs someone to look, in milliseconds from now,
 * or -1 if it is disarmed. What makes a blocking wait on a timerfd a
 * sleep with a deadline rather than a poll: syscall.c asks this to decide
 * how long to park. */
long timerfd_next_ms(struct timerfd *t, uint64_t now_ns);

int timerfd_in_use(void);
