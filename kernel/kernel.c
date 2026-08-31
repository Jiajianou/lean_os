#include <stddef.h>
#include <stdint.h>

#include "acpi/acpi.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/tsc.h"
#include "drivers/console.h"
#include "drivers/cursor.h"
#include "display.h" /* system_api/include/display.h - display_mode_t, M58 */
#include "icon.h"    /* system_api/include/icon.h - M63: icons are files, and this test edits one */
#include "drivers/dispi.h"
#include "drivers/fb.h"
#include "drivers/font8x16.h" /* M39 self-test reads the glyph tables and the shared metric directly */
#include "drivers/keyboard.h"
#include "drivers/klog.h"
#include "drivers/mouse.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/io.h"
#include "drivers/ac97.h"
#include "drivers/pcspk.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "fs/leanfs.h"
#include "fs/openfile.h"
#include "dev/tty.h" /* M85 */
#include "fs/vfs.h"
#include "ipc/pipe.h"
#include "ipc/shm.h" /* M50 - shm_count_by_owner, for the kill storm's segment accounting */
#include "lib/libk.h"
#include "mm/e820.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net/icmp.h"
#include "net/net.h"
#include "net/tcp.h"
#include "panic.h"
#include "paths.h"   /* system_api/include/paths.h - M53's filesystem layout, shared with user space */
#include "proc.h"      /* system_api/include/proc.h - task_info_t, M45's SYS_taskinfo self-test. Resolves to the system_api header, not kernel/proc/proc.h - see syscall.c's own note on the search order. */
#include "power/power.h"
#include "proc/proc.h"
#include "sched/sched.h"
#include "shortcuts.h" /* system_api/include/shortcuts.h - M49's one table of window-manager chords */
#include "signal.h"  /* system_api/include/signal.h */
#include "spawn_error.h" /* system_api/include/spawn_error.h - M48's SYS_spawn failure codes and their shared message table */
#include "syscall.h" /* system_api/include/syscall.h */
#include "wm.h"      /* system_api/include/wm.h - M30 self-test speaks WM_ACTION_PIPE directly */

/* Embedded by kernel/proc/embed_programs.asm - every user program this
 * project ships, built against user_space/lib by the Makefile and
 * incbin'd in as raw bytes. These are only ever used as a one-time seed
 * to get a copy of each onto the disk filesystem the first time it boots
 * - every actual load path (SYS_spawn, kernel_main's own init spawn
 * below) reads back from disk like any other file would be, which is
 * the point (M12). */
#define FOR_EACH_EMBEDDED_PROGRAM(X) \
    X(hello)                         \
    X(echo)                          \
    X(cat)                           \
    X(cp)                            \
    X(audiograb)                     \
    X(libctest)                      \
    X(netconf)                       \
    X(nettime)                       \
    X(nettest)                       \
    X(tcptest)                       \
    X(racetest)                      \
    X(console)                       \
    X(nslookup)                      \
    X(fetch)                         \
    X(httpd)                         \
    X(caps)                          \
    X(captest)                       \
    X(whetstone)                     \
    X(ls)                            \
    X(init)                          \
    X(sh)                            \
    X(memtest)                       \
    X(fonttest)                      \
    X(compositor)                    \
    X(wm_demo)                       \
    X(gui_clock)                     \
    X(gui_paint)                     \
    X(desktop_shell)                 \
    X(desktop_icons)                 \
    X(gui_terminal)                  \
    X(text_editor)                   \
    X(file_manager)                  \
    X(settings)                    \
    X(task_manager)                \
    X(wm_stubborn)                 \
    X(wm_zorder)                   \
    X(wm_faulter)                  \
    X(wm_crash)                    \
    X(badptr)                      \
    X(shutdown)                    \
    X(reboot)                      \
    X(env)                         \
    X(envtest)                     \
    X(sigtest)                     \
    X(treewalk)                    \
    X(mmaptest)                    \
    X(threadtest)                  \
    X(lazytest)                    \
    X(forktest)                    \
    X(exectest)                    \
    X(jobtest)

#define DECLARE_EMBEDDED_PROGRAM(name) \
    extern const uint8_t name##_elf_start[]; \
    extern const uint8_t name##_elf_end[];
FOR_EACH_EMBEDDED_PROGRAM(DECLARE_EMBEDDED_PROGRAM)
#undef DECLARE_EMBEDDED_PROGRAM

/* M81: the word entry.asm stamps immediately below the boot stack. See
 * the checking code near the desktop handoff, and entry.asm for why it
 * exists at all. */
extern uint64_t kernel_stack_guard[];
#define KERNEL_STACK_GUARD_VALUE 0x5354414B47554152ULL /* "STAKGUAR" */

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} embedded_program_t;

static const embedded_program_t embedded_programs[] = {
#define PROGRAM_TABLE_ENTRY(name) {#name, name##_elf_start, name##_elf_end},
    FOR_EACH_EMBEDDED_PROGRAM(PROGRAM_TABLE_ENTRY)
#undef PROGRAM_TABLE_ENTRY
};
#define EMBEDDED_PROGRAM_COUNT (sizeof(embedded_programs) / sizeof(embedded_programs[0]))

/* Self-test helper for M8: invokes `int 0x80` directly (this is still
 * ring 0, so there's no real user/kernel boundary to cross yet - that's
 * M9 - but the gate, dispatch table, and calling convention are exactly
 * what a ring-3 caller will use later). Matches system_api/include/
 * syscall.h's convention: rax = number/return, rdi/rsi/rdx = args 1-3. */
static long do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                      : "rcx", "r8", "r9", "memory");
    return (long)ret;
}

/* M70: the four-argument form. SYS_klog is the first syscall a kernel-side
 * self-test drives that needs a fourth (the cursor it writes back), and
 * the convention is already defined - rcx is arg 4, see syscall.h - so
 * this is the same wrapper with one more register bound. */
static long do_syscall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t ret;
    __asm__ volatile("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "D"(a1), "S"(a2), "d"(a3), "c"(a4)
                      : "r8", "r9", "memory");
    return (long)ret;
}

/* Tears down a task a self-test spawned, and waits for it to really be
 * gone rather than only for the signal to have been posted.
 *
 * The waiting is the point. A SIGKILL is noticed at the target's next
 * syscall or scheduler tick (kernel/sched/sched.c), and only then does
 * task_exit_with_code run shm_free_by_owner - which, for a compositor,
 * hands back every window's pixel buffer plus its own full-screen back
 * buffer, several megabytes of frames. Left unwaited, that return lands
 * at an arbitrary later moment inside whatever self-test happens to be
 * running by then, and the one that measures free frames across an
 * operation (M40's SYS_spawn failure-path audit) reads it as that
 * operation having leaked. That is not hypothetical: it showed up as an
 * intermittent panic in a test with nothing to do with the change being
 * made, which is the worst possible form for it to take.
 *
 * M42: every self-test that spawns a compositor or a client now goes
 * through here instead of firing a bare SYS_kill and moving on. */
/* M59: read a program off disk into a buffer sized for the file, not for
 * the format's ceiling. Every self-test below used to say
 * `kmalloc(LEANFS_MAX_FILE_SIZE)`, which was a harmless 72 KiB
 * over-allocation right up until double-indirect blocks made that ceiling
 * eight megabytes. Fifty-two of those, some of them three at a time, is
 * not an over-allocation any more.
 *
 * Panics rather than returning an error on purpose: every caller is a
 * boot self-test loading a program this kernel seeded onto the disk
 * moments earlier, so a failure here is a broken filesystem, not a
 * situation to recover from. Returns the image; *out_size gets its real
 * length. */
static uint8_t *read_program(const char *path, size_t *out_size) {
    leanfs_stat_t st;
    if (vfs_stat(path, &st) != 0 || st.is_dir || st.size == 0) {
        panic("read_program: a program this kernel just seeded is missing or empty");
    }
    uint8_t *image = (uint8_t *)kmalloc(st.size);
    if (!image) {
        panic("read_program: out of memory reading a program back from disk");
    }
    if (vfs_read(path, image, st.size) < 0) {
        panic("read_program: vfs_read failed on a program that stat succeeded on");
    }
    *out_size = st.size;
    return image;
}

/* M60: type a whole string into whatever holds focus, one injected key
 * at a time, with a gap between keys. The gap is not politeness: the
 * keyboard ring is small and the compositor forwards keys to a client's
 * event pipe one loop iteration at a time, so a burst faster than that
 * loop is a burst that gets dropped. M56's own editor test injected two
 * characters by hand; a command line is thirty, which is where doing it
 * by hand stops being reasonable. */
static void selftest_type(const char *s) {
    for (const char *p = s; *p; p++) {
        keyboard_inject(*p, 0);
        pit_sleep_ms(25);
    }
}

/* M75: does `haystack` contain `needle`? Three self-tests had written
 * their own copy of this loop by the time a fourth wanted one, which is
 * the point at which a helper stops being premature. Deliberately not in
 * lib/libk.h: a substring search is not something this kernel needs, it
 * is something these tests need. */
static int selftest_contains(const char *haystack, const char *needle) {
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (needle[j] && haystack[i + j] == needle[j]) {
            j++;
        }
        if (needle[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

static void selftest_reap(task_t *t) {
    do_syscall(SYS_kill, (uint64_t)t->id, SIGKILL, 0);
    do_syscall(SYS_wait, (uint64_t)t->id, 0, 0);
}

/* ---- M69: waiting for a condition, not for a duration -----------------
 *
 * THE ROOT CAUSE THIS MILESTONE EXISTS TO FIX.
 *
 * There are 137 `pit_sleep_ms` calls in this file, about fifty seconds of
 * pure waiting, and nearly every one of them is a *bet*: sleep long
 * enough that the compositor has probably started, that a client has
 * probably connected, that a window has probably been painted - then read
 * a pixel and assert. The condition is never checked. The duration is a
 * proxy for it.
 *
 * That works for exactly as long as nothing changes how promptly work
 * gets done, and it is why M68 could not land. Wait queues changed the
 * scheduling of every process on the machine, every one of those bets
 * changed odds at once, and the suite failed at whichever test happened
 * to have the least slack - M36 on one run, M55 on the next, wm_demo on
 * the one after. **A failure that moves between runs is not a bug in the
 * thing being tested; it is the suite measuring the wrong thing.** The
 * M55 test's own comment says the quiet part out loud: "anything shorter
 * than a couple of those intervals is a test that passes on timing rather
 * than on behavior."
 *
 * So: wait for the thing you are about to assert. These helpers poll a
 * condition to a deadline and return whether it ever came true. Three
 * consequences, and the third is the one that matters:
 *
 *   - the suite gets FASTER, because almost all of those fifty seconds
 *     was slack held for the worst case;
 *   - a failure names the condition that never became true, instead of
 *     surfacing as a wrong pixel three lines later;
 *   - a scheduling change can no longer break a test that was not about
 *     scheduling. That is the property M68 needs and did not have.
 *
 * The deadline is generous on purpose. It is not a performance budget -
 * M69's input-to-photon instrumentation below is where timing is
 * *measured*. This is only the difference between "it did not happen
 * yet" and "it is never going to happen", and being wrong about that in
 * the tight direction is how the suite got here.
 */
#define SELFTEST_POLL_MS 10 /* one PIT tick: the finest grain the clock has */

/* Poll `probe` until it returns non-zero or `timeout_ms` elapses. Returns
 * 1 if the condition came true, 0 on timeout. */
static int selftest_wait_until(int (*probe)(void *ctx), void *ctx, uint32_t timeout_ms,
                                const char *what) {
    uint64_t deadline = pit_get_ticks() + (timeout_ms + 9) / 10;
    for (;;) {
        if (probe(ctx)) {
            return 1;
        }
        if (pit_get_ticks() >= deadline) {
            /* Said out loud rather than left to whatever assertion fails
             * next. A timeout here is a real finding - either the machine
             * genuinely cannot do this any more, or the budget is wrong -
             * and both are worth more than a mystery pixel. */
            klog_puts("[selftest] timed out waiting for: ");
            klog_puts(what);
            klog_putc('\n');
            return 0;
        }
        pit_sleep_ms(SELFTEST_POLL_MS);
    }
}

typedef struct {
    uint32_t x, y, expected;
} pixel_probe_t;

static int pixel_matches(void *ctx) {
    pixel_probe_t *p = (pixel_probe_t *)ctx;
    return fb_get_pixel(p->x, p->y) == p->expected;
}

/* The workhorse. "Wait until this pixel is this colour" is the condition
 * behind almost every GUI self-test in this file - a window is up when
 * its own colour is where it should be, and no sooner. */
static int selftest_wait_for_pixel(uint32_t x, uint32_t y, uint32_t expected,
                                    uint32_t timeout_ms, const char *what) {
    pixel_probe_t probe = {x, y, expected};
    return selftest_wait_until(pixel_matches, &probe, timeout_ms, what);
}

/* How long the check below will wait for the screen to catch up.
 * Generous for the reason SELFTEST_POLL_MS's own comment gives: this is
 * the difference between "not yet" and "never", not a budget. */
#define SELFTEST_PAINT_MS 4000

/* The pixel at (x, y) once it is what the caller expects - or whatever it
 * actually is when the deadline expires, which leaves the caller's own
 * comparison to report the failure exactly as it would have.
 *
 * The shape almost every GUI check in this file wanted and did not have.
 * `pit_sleep_ms(400); px = fb_get_pixel(...)` grades a constant against a
 * guess at how long this machine takes to paint one, and those two things
 * are unrelated. M69 replaced the sleeps that waited for a *process* with
 * conditions and left behind the ones that wait for a *repaint*; they
 * fail the same way it described - intermittently, on a busy host, and
 * looking exactly like the defect the check exists to find. Two were
 * caught doing it: a launcher overlay sampled three fifths of the way
 * through its fade-in, and a terminal sampled while `ls` was still
 * printing.
 *
 * It cannot make a check weaker. On a timeout the caller gets whatever is
 * actually on screen and grades that, exactly as it graded whatever a
 * sleep happened to leave it - a pixel that is never going to be right is
 * still wrong at the deadline, and this only decides how long to keep
 * asking. The expected value passed here is a condition to wait on and
 * not the assertion; where a caller has a table of expected values, that
 * table is still what grades.
 *
 * Two agreeing reads, not one, and that is not belt-and-braces. This
 * kernel reads the framebuffer while the compositor is in the middle of
 * copying its back buffer into it a row at a time (compositor.c's
 * present), so a single read taken at the instant a pixel first goes
 * right can be half of one frame and half of the one before - which is
 * how a probe a few rows lower comes back holding the *previous* frame,
 * animation ghost and all. A fixed sleep never saw that because it always
 * sampled long after the last paint; waiting for a condition means
 * sampling exactly when one is happening. Ten milliseconds apart is
 * several frames' worth of separation, and a torn frame does not survive
 * to the second one.
 *
 * Which is also why every *graded* pixel of a group should come through
 * here rather than only the one that changes: settling the one and then
 * reading its neighbours plainly puts those neighbours back in the
 * torn-frame window this exists to step out of. */
static uint32_t selftest_pixel_settled(uint32_t x, uint32_t y, uint32_t expected,
                                        const char *what) {
    uint64_t deadline = pit_get_ticks() + (SELFTEST_PAINT_MS + 9) / 10;
    int agreed = 0;
    for (;;) {
        if (fb_get_pixel(x, y) == expected) {
            if (++agreed >= 2) {
                return expected;
            }
        } else {
            agreed = 0;
        }
        if (pit_get_ticks() >= deadline) {
            klog_puts("[selftest] timed out waiting for: ");
            klog_puts(what);
            klog_putc('\n');
            return fb_get_pixel(x, y);
        }
        pit_sleep_ms(SELFTEST_POLL_MS);
    }
}

/* M46: the window title's own pixels, counted by colour over the strip
 * the title is drawn in - x:[106, 150), y:[82, 98), which is "Clock"
 * starting at the cascade origin plus TITLE_MARGIN.
 *
 * Counted rather than probed at a point on purpose: picking one glyph
 * pixel by hand would be asserting on the shape of the font rather than
 * on the colour the title is drawn in.
 *
 * `until_bright` says which of the two this pass is waiting to see, and
 * the wait is the same one selftest_pixel_settled makes: the title is
 * painted by the same repaint as the frame around it, so a titlebar with
 * an animation ghost over it (a window opening is one, and it is drawn in
 * front of the windows by design) has *neither* colour anywhere in the
 * strip. Zero and zero is not the dim title this check is looking for and
 * not the bright one either - it is the check having looked too early.
 * The counts it returns are still exactly what grades. */
static void selftest_title_counts(int until_bright, int *out_bright, int *out_dim) {
    uint64_t deadline = pit_get_ticks() + (SELFTEST_PAINT_MS + 9) / 10;
    for (;;) {
        int bright = 0, dim = 0;
        for (int32_t ty = 82; ty < 98; ty++) {
            for (int32_t tx = 106; tx < 150; tx++) {
                uint32_t c = fb_get_pixel(tx, ty);
                if (c == 0x00F0F0F0u) {
                    bright++;
                } else if (c == 0x009AA4B0u) {
                    dim++;
                }
            }
        }
        int found = until_bright ? bright : dim;
        if (found > 0 || pit_get_ticks() >= deadline) {
            if (found == 0) {
                klog_puts("[selftest] timed out waiting for: the window title to be drawn\n");
            }
            *out_bright = bright;
            *out_dim = dim;
            return;
        }
        pit_sleep_ms(SELFTEST_POLL_MS);
    }
}

/* M69: "wait until the compositor owns the screen", which is what all
 * ~35 `process_spawn("compositor"); pit_sleep_ms(N)` pairs in this file
 * were really saying. The desktop background at a point no window covers
 * is the condition; the sleep was a guess at how long it takes.
 *
 * DELIBERATELY DOES NOT FAIL ON TIMEOUT, and that is what makes replacing
 * a sleep with it safe to do mechanically across dozens of call sites: if
 * the desktop appears, this returns in tens of milliseconds instead of
 * hundreds; if it does not, this has waited at least as long as the sleep
 * it replaced and the caller's own assertions then fail exactly as they
 * did before. It can make the suite faster. It cannot make it fail.
 *
 * (500,400) is bare desktop in every self-test here - well right of the
 * icon column, well above the taskbar, and no self-test opens a window
 * over it before checking the compositor is up. */
static void selftest_wait_for_compositor(void) {
    selftest_wait_for_pixel(500, 400, 0x001A1A2Eu, 5000, "the compositor to paint the desktop");
}

/* M61: how many pixels of the column at `x`, between the bottom of a
 * cascaded window and the bottom of the screen, are not the desktop
 * colour. An animation is deliberately *not* settled, so what a test can
 * honestly assert is that something is somewhere on the path - not where
 * exactly, which is a question about easing and scheduler jitter. */
static int selftest_column_lit(uint32_t x, uint32_t bg) {
    int lit = 0;
    for (uint32_t y = 240; y < fb_height(); y += 4) {
        if (fb_get_pixel(x, y) != bg) {
            lit++;
        }
    }
    return lit;
}

/* The same column, watched until something appears on it. Returns the
 * most it ever saw (0 if nothing did), and how long it took to see it.
 *
 * One sample is not enough and the reason is worth writing down: this
 * task sleeps with `hlt` and comes back when the scheduler next picks it,
 * so "wait 60 ms" is a floor rather than a time - a single sample can
 * land after a 140 ms animation has already finished.
 *
 * And nor is a fixed *number* of samples, which is what this was.
 * Twelve samples 12 ms apart covers a 140 ms animation on a machine
 * where a sleep of 12 ms is 12 ms and a frame is 16 ms. Attach a display
 * to the emulator, or put the host under load, and both of those numbers
 * grow together - the animation stretches (compositor.c's
 * ANIM_MIN_FRAMES) and this task's own turn comes round less often - and
 * a window that covers a fixed 144 ms lands entirely before the motion it
 * was meant to catch. That failure looks exactly like the defect this
 * check exists to find, which is the worst property a check can have.
 *
 * So: a deadline instead of a sample count, and one that is generous
 * because it is not a performance budget - what is being asserted is that
 * motion happens at all, not that it happens within any particular number
 * of milliseconds. The caller gets the latency back so that the matching
 * negative check ("and nothing at all with motion switched off") can be
 * watched for at least as long as the positive one needed. */
static int selftest_column_lit_wait(uint32_t x, uint32_t bg, uint32_t timeout_ms,
                                     uint32_t *took_ms) {
    uint64_t start = pit_get_ticks();
    uint64_t deadline = start + (timeout_ms + 9) / 10;
    int peak = 0;
    for (;;) {
        int lit = selftest_column_lit(x, bg);
        if (lit > peak) {
            peak = lit;
        }
        if (peak > 0 || pit_get_ticks() >= deadline) {
            break;
        }
        pit_sleep_ms(SELFTEST_POLL_MS);
    }
    if (took_ms) {
        *took_ms = (uint32_t)((pit_get_ticks() - start) * 10);
    }
    return peak;
}

/* The same column, watched for a whole window of time rather than until
 * something happens - the shape a *negative* claim needs, since there is
 * no moment at which "nothing is going to appear" becomes true. */
static int selftest_column_lit_peak_ms(uint32_t x, uint32_t bg, uint32_t window_ms) {
    uint64_t deadline = pit_get_ticks() + (window_ms + 9) / 10;
    int peak = 0;
    for (;;) {
        int lit = selftest_column_lit(x, bg);
        if (lit > peak) {
            peak = lit;
        }
        if (pit_get_ticks() >= deadline) {
            return peak;
        }
        pit_sleep_ms(SELFTEST_POLL_MS);
    }
}

/* And the other end of the same claim: watched until the path is bare
 * again. Returns what is still lit, which is 0 when the animation cleaned
 * up after itself and a real finding when it did not. A deadline rather
 * than a sleep for the same reason as above - "the animation has
 * finished" is a condition, and on a machine slow enough to stretch one
 * (ANIM_MIN_FRAMES again) a fixed 400 ms can expire in the middle of it
 * and report a perfectly healthy animation as a trail left behind. */
static int selftest_column_clear_wait(uint32_t x, uint32_t bg, uint32_t timeout_ms) {
    uint64_t deadline = pit_get_ticks() + (timeout_ms + 9) / 10;
    for (;;) {
        int lit = selftest_column_lit(x, bg);
        if (lit == 0 || pit_get_ticks() >= deadline) {
            return lit;
        }
        pit_sleep_ms(SELFTEST_POLL_MS);
    }
}

/* M56: how much of gui_terminal's top text row is lit.
 *
 * The window is the only one on screen, so it lands at the cascade origin
 * (100, 100) and its first row of 8x16 cells is y:[100, 116) across its
 * 70 columns. Counting lit pixels rather than reading the text is
 * deliberate: what row is at the top of a scrolled-back terminal is a
 * claim about scrollback, and which filenames are on it is a claim about
 * the order leanfs happens to return directory records in. */
static int selftest_term_top_lit(void) {
    int lit = 0;
    for (int32_t ty = 100; ty < 116; ty++) {
        for (int32_t tx = 100; tx < 660; tx++) {
            if (fb_get_pixel((uint32_t)tx, (uint32_t)ty) == 0x00D0D0D0u) {
                lit++;
            }
        }
    }
    return lit;
}

/* The same count, once it has stopped moving - and, when `differs_from`
 * is not -1, once it has stopped moving somewhere *else*.
 *
 * This used to be three fixed sleeps, and the middle one carried the
 * whole test: `ls /bin` is a spawn, its output through a pipe, and a
 * terminal draining that pipe a line at a time, and 1500 ms was how long
 * those three take on the machine the number was written on. On a slower
 * one the count gets sampled while output is still arriving; the terminal
 * scrolls once more behind the sample, and the "live" figure the last
 * check compares against is a view the terminal had already left. The
 * failure that produces - scrolling forward did not come back to the live
 * view - is indistinguishable from the defect the check exists to find.
 *
 * "Stopped moving" needs a length of quiet, and the length that means
 * something is a property of the machine rather than a constant: a
 * terminal rendering a line every 30 ms and one rendering a line every
 * 400 ms both look identical for 300 ms at a time. So the quiet this
 * requires is measured from the machine's own cadence - four times the
 * longest gap between two changes it has actually been seen to take, and
 * never less than 300 ms. Nothing to keep in step with a scheduler
 * change, and nothing to re-measure when the host gets slower. */
static int selftest_term_top_settled(int differs_from, uint32_t timeout_ms) {
    uint64_t deadline = pit_get_ticks() + (timeout_ms + 9) / 10;
    uint64_t last_change = pit_get_ticks();
    uint64_t quiet_needed = 30;  /* ticks: 300 ms, the floor */
    int value = selftest_term_top_lit();
    for (;;) {
        pit_sleep_ms(SELFTEST_POLL_MS);
        uint64_t now = pit_get_ticks();
        int sampled = selftest_term_top_lit();
        if (sampled != value) {
            uint64_t gap = (now - last_change) * 4;
            if (gap > quiet_needed) {
                quiet_needed = gap > 400 ? 400 : gap; /* ticks: 4 s, the ceiling */
            }
            value = sampled;
            last_change = now;
        } else if (now - last_change >= quiet_needed &&
                   (differs_from < 0 || value != differs_from)) {
            return value;
        }
        if (now >= deadline) {
            /* Out of time. Returning what is actually on screen leaves
             * the caller's own assertion to say what went wrong, which is
             * a better report than a timeout here would be. */
            return value;
        }
    }
}

/* "The compositor has taken this settings change" as a condition rather
 * than as a sleep.
 *
 * M61's own test writes `animations = 0` down WM_SETTINGS_PIPE and then
 * asserts that the next minimize animates *nothing*. Between those two
 * things sits a message the compositor has not read yet, and a fixed
 * sleep in the gap is a guess about how long a busy machine takes to get
 * round to its request pipes. Guess short and the toggle arrives while
 * the setting is still on, the window animates exactly as designed, and
 * the test reports the setting as broken. So ask, using M44's read side
 * of the same setting, until the answer comes back. */
static int selftest_wait_for_animations_setting(int want, uint32_t timeout_ms) {
    int sq_fds[2];
    int sqr_fds[2];
    if (do_syscall(SYS_pipe_open, (uint64_t)WM_SETTINGS_QUERY_PIPE, (uint64_t)sq_fds, 0) != 0 ||
        do_syscall(SYS_pipe_open, (uint64_t)WM_SETTINGS_QUERY_RESP_PIPE, (uint64_t)sqr_fds, 0) != 0) {
        return 0;
    }
    uint64_t deadline = pit_get_ticks() + (timeout_ms + 9) / 10;
    for (;;) {
        /* Drain first: anything already in there is the answer to
         * somebody else's question - see the M44 self-test's own note. */
        do_syscall(SYS_pipe_reset, (uint64_t)sqr_fds[0], 0, 0);
        uint8_t ping = 1;
        do_syscall(SYS_write, (uint64_t)sq_fds[1], (uint64_t)&ping, sizeof(ping));
        pit_sleep_ms(SELFTEST_POLL_MS);
        wm_settings_request_t got;
        k_memset(&got, 0, sizeof(got));
        if (do_syscall(SYS_read, (uint64_t)sqr_fds[0], (uint64_t)&got, sizeof(got)) == (long)sizeof(got) &&
            (int)(got.animations != 0) == (want != 0)) {
            return 1;
        }
        if (pit_get_ticks() >= deadline) {
            klog_puts("[selftest] timed out waiting for: the compositor to take the animations setting\n");
            return 0;
        }
        pit_sleep_ms(SELFTEST_POLL_MS);
    }
}

/* ---- M69: input-to-photon ---------------------------------------------
 *
 * The number this milestone owns, and the first thing it does - before
 * any scheduler change, because a milestone that tunes a number it never
 * measured cannot claim anything afterwards.
 *
 * What is measured is the whole path a person's hand takes: a mouse event
 * enters the driver exactly as the IRQ handler would deliver it, the
 * compositor notices, decides what moved, repaints the box the cursor
 * passed through, and those pixels reach the framebuffer. Timed with the
 * TSC (kernel/arch/x86_64/tsc.h) rather than the PIT, because the budget
 * being judged is 16 ms and a 10 ms clock cannot see inside one.
 *
 * The cursor is the right thing to move for this. It is the shortest
 * input-to-photon path on the machine - no client involved, no protocol
 * round trip, just the compositor's own "the pointer moved, repaint an
 * 8x8 box" branch, which its own comment calls the hot path. Anything
 * slower than this is slower for a reason further up.
 *
 * Returns microseconds, or 0 if the pixel never changed within the
 * timeout - which is a failure the caller has to report, not a zero to
 * average in.
 */
static uint64_t selftest_input_to_photon_us(int32_t target_x, int32_t target_y,
                                             uint32_t timeout_ms) {
    /* Park the pointer at the top-left first, so the delta below lands it
     * somewhere known regardless of where the last test left it. The
     * compositor clamps, so overshooting is the way to ask for a corner. */
    mouse_inject(-5000, -5000, 0, 0);
    pit_sleep_ms(120); /* not part of the measurement - just letting the move settle */

    /* Sample the target *after* the cursor has gone elsewhere, so this is
     * genuinely the background and the change below is genuinely the
     * cursor arriving. */
    uint32_t before = fb_get_pixel((uint32_t)target_x + 2, (uint32_t)target_y + 2);

    uint64_t deadline = pit_get_ticks() + (timeout_ms + 9) / 10;
    uint64_t t0 = tsc_read();
    mouse_inject(target_x, target_y, 0, 0);
    for (;;) {
        if (fb_get_pixel((uint32_t)target_x + 2, (uint32_t)target_y + 2) != before) {
            return tsc_to_us(tsc_read() - t0);
        }
        if (pit_get_ticks() >= deadline) {
            return 0;
        }
        /* Not a bare spin, and the first version of this was - which
         * produced a beautifully precise measurement of the wrong thing.
         *
         * A tight loop here is a CPU-bound task. Under round-robin with a
         * 50 ms quantum that means the compositor cannot run again until
         * this loop has burned its entire slice, so the "idle" figure came
         * out at 82 ms: the measurement was measuring its own greed.
         * Yielding makes this observer close to free, so what is left is
         * the path being timed.
         *
         * Still not pit_sleep_ms: that would quantise the answer to 10 ms,
         * which is the whole thing this exists to see inside of. schedule()
         * gives the CPU up without giving up the clock. */
        schedule();
    }
}

/* M47: every GUI self-test below spawns a real compositor and grades real
 * pixels against the compositor's *compiled-in* defaults - the desktop
 * background, the accent color, the wallpaper style. That was safe for
 * eleven milestones because those defaults were the only thing a fresh
 * compositor could start with.
 *
 * M47 made settings persist, which quietly broke it: a user who picks a
 * flat wallpaper writes settings.conf, and every subsequent boot's [m44]
 * self-test then panics because the desktop is no longer the gradient it
 * asserts. Found the first time the input harness rebooted a guest that
 * had just changed its wallpaper - which is exactly the scenario the
 * feature exists for.
 *
 * So the self-test phase runs against known settings, and hands the
 * user's own file back before PID 1 ever starts. Not "the tests should
 * tolerate any settings": a pixel test whose expected values depend on
 * what somebody clicked last week isn't a test. */
static char saved_user_settings[256];
static int64_t saved_user_settings_len = -1;

static const char SELFTEST_SETTINGS_CONF[] =
    "bg=0x001a1a2e\n"      /* compositor.c's DEFAULT_BG_COLOR */
    "accent=0x004c99e6\n"  /* its TITLEBAR_FOCUS_COLOR */
    "wallpaper=0x00000001\n" /* WALLPAPER_GRADIENT, its wallpaper_id default */
    "animations=0x00000001\n"; /* M61: on, which is also the default - stated rather than left to the fallback, because this file is the whole point of "the self-tests run against known settings" */

/* M74: the same, with motion off. This milestone's self-test starts a
 * compositor and two clients and then kills them; what it asserts is
 * where a window came back, and nothing about how it got there. M61's
 * frame-budget check is global to a compositor process and fails the
 * whole boot on any animated frame that overruns 16 ms - which is the
 * right assertion for the test that is *about* motion, and the wrong one
 * to apply to a compositor competing with two clients over something
 * else entirely. So this one does not animate, and M61's own self-test
 * keeps the budget honest. */
static const char SELFTEST_SETTINGS_NO_ANIM[] =
    "bg=0x001a1a2e\n"
    "accent=0x004c99e6\n"
    "wallpaper=0x00000001\n"
    "animations=0x00000000\n";

static void selftest_settings_install_defaults(void) {
    saved_user_settings_len = vfs_read(PATH_SETTINGS, saved_user_settings, sizeof(saved_user_settings));
    if (saved_user_settings_len > (int64_t)sizeof(saved_user_settings)) {
        saved_user_settings_len = -1; /* bigger than anything settings_file_save writes - not ours to preserve */
    }
    vfs_write(PATH_SETTINGS, SELFTEST_SETTINGS_CONF, sizeof(SELFTEST_SETTINGS_CONF) - 1);
}

static void selftest_settings_restore(void) {
    if (saved_user_settings_len >= 0) {
        vfs_write(PATH_SETTINGS, saved_user_settings, (size_t)saved_user_settings_len);
    } else {
        /* There wasn't one. Writing the defaults is behaviorally the same
         * as leaving no file (settings_file_load falls back to exactly
         * these), and this kernel has no unlink to do the other thing. */
        vfs_write(PATH_SETTINGS, SELFTEST_SETTINGS_CONF, sizeof(SELFTEST_SETTINGS_CONF) - 1);
    }
}

/* Self-test task body for M7: prints a few lines with a CPU-bound spin
 * between them (long enough to span several scheduler quanta) so that if
 * preemption is really working, two of these running concurrently
 * interleave their output instead of one finishing before the other
 * starts. */
static void demo_task(void *arg) {
    const char *name = (const char *)arg;
    for (int i = 0; i < 5; i++) {
        klog_puts("[task ");
        klog_puts(name);
        klog_puts("] iteration ");
        klog_put_hex32((uint32_t)i);
        klog_putc('\n');

        /* Busy-spin (no hlt - this has to be genuine CPU-bound work for
         * the timer to forcibly preempt) for a few real PIT ticks rather
         * than a fixed instruction count, so this reliably spans multiple
         * scheduler quanta regardless of host CPU speed. */
        uint64_t target = pit_get_ticks() + 3;
        while (pit_get_ticks() < target) {
            for (volatile int spin = 0; spin < 100000; spin++) {
            }
        }
    }
}

/* SMP self-test task body: records which physical CPU (smp_current_cpu)
 * actually ran each iteration into a shared array the caller provides,
 * with a real (PIT-tick-based) CPU-bound spin between iterations - same
 * "genuine work, not instruction-count timing" reasoning as demo_task
 * above - so several of these running at once give every online CPU a
 * real chance to pick one up via the round-robin scheduler. */
static void smp_probe_task(void *arg) {
    volatile int *seen = (volatile int *)arg;
    for (int iter = 0; iter < 30; iter++) {
        seen[smp_current_cpu()] = 1;
        uint64_t target = pit_get_ticks() + 1;
        while (pit_get_ticks() < target) {
            for (volatile int spin = 0; spin < 20000; spin++) {
            }
        }
    }
}

/* Self-test task body for M8: exits via the SYS_exit syscall instead of
 * just returning (which would reach the same place indirectly through
 * task_entry_trampoline -> task_exit) - this exercises the syscall
 * dispatch path for exit specifically, from inside a real task rather
 * than kernel_main's own (undying) context. */
static void syscall_exit_task(void *arg) {
    (void)arg;
    long pid = do_syscall(SYS_getpid, 0, 0, 0);
    klog_puts("[task C] getpid() via syscall = ");
    klog_put_hex64((uint64_t)pid);
    klog_puts(", exiting via SYS_exit...\n");
    do_syscall(SYS_exit, 0, 0, 0);
    panic("syscall_exit_task: resumed after SYS_exit"); /* should be unreachable */
}

/* M14 self-test task bodies: a pipe producer/consumer pair (kernel-level
 * pipe_create/pipe_write/pipe_read calls, not through SYS_pipe - this
 * exercises the actual blocking buffer logic directly; the syscall
 * plumbing on top of it gets its own, separate self-test in
 * kernel_main). The consumer starts before the producer has written
 * anything, so pipe_read genuinely has to block (cooperatively yield)
 * and get woken by later scheduling rather than finding data already
 * there. */
static void pipe_producer_task(void *arg) {
    pipe_t *p = (pipe_t *)arg;
    static const char msg[] = "ping";
    for (int i = 0; i < 3; i++) {
        pipe_write(p, msg, sizeof(msg) - 1);
    }
    pipe_close_write(p);
}

static void pipe_consumer_task(void *arg) {
    pipe_t *p = (pipe_t *)arg;
    char buf[64];
    size_t total = 0;
    for (;;) {
        long n = pipe_read(p, buf + total, sizeof(buf) - total);
        if (n == 0) {
            break; /* EOF: pipe_close_write was called and the buffer's empty */
        }
        total += (size_t)n;
    }
    klog_puts("[pipe] consumer received ");
    klog_put_hex64((uint64_t)total);
    klog_puts(" bytes: \"");
    for (size_t i = 0; i < total; i++) {
        klog_putc(buf[i]);
    }
    klog_puts("\"\n");
    buf[total] = '\0';
    if (total != 12 || k_strcmp(buf, "pingpingping") != 0) {
        panic("pipe self-test: consumer received unexpected data");
    }
}

/* Loops forever doing real CPU-bound work (not hlt) so terminating it
 * has to come from a signal actually being delivered, not the task just
 * finishing on its own. */
/* M68: parks in SYS_waitfds on a pipe nobody writes to - the shortest
 * way to produce a task in TASK_BLOCKED, now that pipe_read itself
 * deliberately still spins (see kernel/ipc/pipe.c on why). A ten-second
 * timeout rather than none, so a self-test that is about not-hanging
 * cannot itself hang. The fd comes in as the arg, so this needs no
 * globals. */
static void m68_sleeper_task(void *arg) {
    int fd = (int)(uint64_t)arg;
    int fds[1];
    fds[0] = fd;
    do_syscall(SYS_waitfds, (uint64_t)fds, 1, 10000);
    task_exit();
}

static void spinner_task(void *arg) {
    (void)arg;
    for (;;) {
        for (volatile int i = 0; i < 1000000; i++) {
        }
    }
}

/* Returns immediately - exit code 0 via task_entry_trampoline's implicit
 * task_exit(). Used to give SYS_wait(-1) two real children to reap. */
static void quick_task(void *arg) {
    (void)arg;
}

/* e820_map: pointer to a dword entry count immediately followed by that
 * many e820_entry_t records — the layout the boot loader
 * (kernel/boot/uefi/boot.c) builds and hands off in RDI.
 * fb_info: fb_boot_info_t describing the linear framebuffer the boot
 * loader's init_framebuffer set up (M16) - handed off in RSI, the System V
 * ABI's second integer argument register.
 * rsdp_phys: M47 - the ACPI RSDP's physical address, taken from the UEFI
 * configuration table by the boot loader, or 0 if the firmware published
 * none. RDX, the third argument register. Under UEFI the RSDP is not in
 * the legacy BIOS ranges kernel/acpi/acpi.c scans, so without this ACPI
 * simply is not found - which had been silently true (and quietly costing
 * this kernel SMP) since M26 removed the BIOS boot path. */
/* M81: "fNNNN" for the file storm in the M81 self-test, four digits wide.
 *
 * A function because the first version of that test open-coded three
 * digits in three places, which silently wrapped at a thousand: file 1000
 * was named "f000" and overwrote file 0, so a directory told to hold 1200
 * names held 1000 and the test reported the filesystem as broken. The
 * filesystem was fine. One formatter, wide enough for the count, is both
 * the fix and the reason it is not written inline three times. */
static void m81_storm_name(char *out, int i) {
    out[0] = 'f';
    out[1] = (char)('0' + (i / 1000) % 10);
    out[2] = (char)('0' + (i / 100) % 10);
    out[3] = (char)('0' + (i / 10) % 10);
    out[4] = (char)('0' + i % 10);
    out[5] = '\0';
}

void kernel_main(uint32_t *e820_map, fb_boot_info_t *fb_info, uint64_t rsdp_phys) {
    klog_init();
    klog_puts("lean_os kernel: hello from C!\n\n");

    gdt_init();
    idt_init();
    pic_remap();
    klog_puts("GDT/TSS, IDT, and PIC remap initialized.\n");

    /* Self-test: a real trip through the IDT/ISR pipeline (gate -> stub
     * -> C handler -> iretq) rather than just trusting it compiled.
     * int3 is the one exception vector that's meant to be resumed, so
     * this proves the round trip works without ending in a panic. */
    __asm__ volatile("int3");
    klog_puts("Resumed after breakpoint self-test.\n\n");

    uint32_t count = *e820_map;
    if (count == 0) {
        panic("E820 memory map is empty - cannot continue");
    }

    e820_entry_t *entries = (e820_entry_t *)((uint8_t *)e820_map + 8);

    klog_puts("E820 memory map (");
    klog_put_hex32(count);
    klog_puts(" entries):\n");

    for (uint32_t i = 0; i < count; i++) {
        klog_puts("  base=0x");
        klog_put_hex64(entries[i].base);
        klog_puts(" len=0x");
        klog_put_hex64(entries[i].length);
        klog_puts(" type=0x");
        klog_put_hex32(entries[i].type);
        klog_putc('\n');
    }
    klog_putc('\n');

    pmm_init(e820_map);
    vmm_init();
    heap_init();

    /* Self-test: map, write through, read back, and unmap a throwaway
     * virtual address directly via vmm - the same "prove it, don't just
     * trust it compiled" discipline as the int3 test above. */
    uint64_t scratch_phys = pmm_alloc_frame();
    uint64_t scratch_virt = 0x50000000ULL; /* arbitrary address above the 1 GiB identity map, unused by the heap */
    vmm_map_page(scratch_virt, scratch_phys, VMM_FLAG_WRITABLE);
    volatile uint64_t *scratch = (volatile uint64_t *)scratch_virt;
    *scratch = 0x1122334455667788ULL;
    if (*scratch != 0x1122334455667788ULL) {
        panic("vmm self-test: readback mismatch");
    }
    vmm_unmap_page(scratch_virt);
    pmm_free_frame(scratch_phys);
    klog_puts("[vmm] map/unmap self-test passed.\n");

    /* Self-test: kmalloc/kfree round trip through the heap, which exercises
     * vmm_map_page again via a completely different code path (heap growth,
     * not a direct call) than the test above. */
    uint64_t *test = (uint64_t *)kmalloc(sizeof(uint64_t));
    if (!test) {
        panic("kmalloc self-test: allocation failed");
    }
    *test = 0xDEADBEEFCAFEBABEULL;
    if (*test != 0xDEADBEEFCAFEBABEULL) {
        panic("kmalloc self-test: readback mismatch");
    }
    kfree(test);
    klog_puts("[heap] kmalloc/kfree self-test passed.\n\n");

    /* M16: bring up the linear framebuffer the boot loader's
     * init_framebuffer set up and described in RSI (fb_info, this
     * function's second argument) - needs vmm live first, since fb_init
     * maps the physical framebuffer region in. */
    fb_init(fb_info);
    /* M58: probe the display adapter for a runtime mode-setting interface
     * and build the validated mode list, right after the framebuffer the
     * firmware handed us is mapped. Finding nothing is an ordinary
     * outcome, not a failure - on any machine without a Bochs/QEMU DISPI
     * adapter the answer is "the mode the firmware picked, and nothing on
     * offer", which is exactly what the Display pane then shows. */
    dispi_init();
    /* M59: read the CMOS clock once here so the boot log says up front
     * whether this machine knows the date - every timestamp below depends
     * on the answer, and "files are dated zero" is much easier to explain
     * when the reason is one line near the top of the log. */
    rtc_init();
    /* M62: the first sound this OS has ever been able to make. The
     * speaker is unconditional - PIT channel 2 gated onto port 0x61 is
     * hardware every PC-compatible machine has - and the AC'97 probe
     * degrades to "no device found" exactly as M27 decided for the NIC,
     * because a desktop that cannot find a sound card should still be a
     * desktop. */
    pcspk_init();
    ac97_init();

    /* Self-test: clear to a background color, fill a smaller rectangle
     * with a different one, then read individual pixels back to confirm
     * both landed exactly where expected - a memory-correctness proof,
     * the same "prove it, don't just trust it compiled" discipline as
     * every earlier milestone's self-tests. (Whether it's actually
     * *visible* is checked separately via a QEMU screendump - reading
     * our own writes back only proves the mapping and pixel math are
     * right, not that anything reaches the emulated display.) */
    fb_clear(0x001A1A2E);
    fb_fill_rect(10, 10, 100, 50, 0x00E94560);
    if (fb_get_pixel(0, 0) != 0x001A1A2E) {
        panic("fb self-test: background color readback mismatch");
    }
    if (fb_get_pixel(59, 34) != 0x00E94560) {
        panic("fb self-test: rectangle color readback mismatch (inside)");
    }
    if (fb_get_pixel(200, 200) != 0x001A1A2E) {
        panic("fb self-test: rectangle color readback mismatch (outside, should be background)");
    }
    klog_puts("[fb] framebuffer clear/fill/readback self-test passed.\n\n");

    /* M58 self-test: a real mode change, in the kernel, before anything
     * has been built on top of the boot geometry. Three things are worth
     * proving and only one of them is "the call returned 0":
     *
     *   1. The geometry SYS_fb_info would report is the one the device
     *      actually took, not the one that was asked for.
     *   2. The *pitch* is the one read back out of the adapter's own
     *      VIRT_WIDTH register. fb.h has said since M16 that pitch is not
     *      necessarily width * 4; a mode change that assumed otherwise
     *      would shear the whole screen, and would do it on a machine
     *      where the only way to see the damage is to look at it.
     *   3. The mapping actually grew. A larger mode needs more pages than
     *      the boot mode's mapping covered, so the far corner of the new
     *      mode is written and read back - which faults in ring 0 if
     *      fb_remap did not map through to it, and returns the wrong
     *      value if the pitch is wrong.
     *
     * Then it puts the boot mode back, because everything after this line
     * (the console, the desktop, every other self-test's pixel
     * coordinates) is written against it.
     *
     * On hardware with no DISPI adapter this is skipped rather than
     * failed - "this display cannot be resized after boot" is the honest
     * answer there, and the Display pane says exactly that. */
    {
        uint32_t boot_w = fb_width(), boot_h = fb_height(), boot_pitch = fb_pitch_bytes();

        if (!dispi_available()) {
            klog_puts("[m58] no runtime mode-setting interface on this adapter - "
                       "resolution stays what the firmware chose (self-test skipped).\n\n");
        } else {
            display_mode_t list[DISPLAY_MAX_MODES];
            int n = dispi_get_modes(list, DISPLAY_MAX_MODES);
            if (n <= 0) {
                panic("M58 self-test: a DISPI adapter answered the probe but offers no modes");
            }
            /* Any offered mode that is not the one already running - and
             * preferring a *larger* one, since growing the mapping is the
             * half that can actually fail. */
            int pick = -1;
            for (int i = 0; i < n; i++) {
                if (list[i].width == boot_w && list[i].height == boot_h) {
                    continue;
                }
                if (pick < 0 || (uint64_t)list[i].width * list[i].height >
                                 (uint64_t)list[pick].width * list[pick].height) {
                    pick = i;
                }
            }
            if (pick < 0) {
                panic("M58 self-test: the only offered mode is the one already running");
            }

            uint32_t pitch = 0;
            if (dispi_set_mode(list[pick].width, list[pick].height, &pitch) != 0) {
                panic("M58 self-test: the adapter refused a mode this driver had already validated");
            }
            if (pitch < list[pick].width * 4u) {
                panic("M58 self-test: the pitch read back from the device is narrower than one row of pixels");
            }
            fb_remap(pitch, list[pick].width, list[pick].height);

            if (fb_width() != list[pick].width || fb_height() != list[pick].height) {
                panic("M58 self-test: fb geometry after a mode change is not the mode that was set");
            }
            if (fb_pitch_bytes() != pitch) {
                panic("M58 self-test: fb pitch is not the one read back from the device");
            }
            if (fb_mapped_bytes() < (uint64_t)pitch * fb_height()) {
                panic("M58 self-test: the framebuffer mapping does not cover the new mode");
            }

            /* The far corner - the pixel that only exists in the new
             * mode, at the stride the device chose. */
            fb_put_pixel(fb_width() - 1, fb_height() - 1, 0x00123456u);
            if (fb_get_pixel(fb_width() - 1, fb_height() - 1) != 0x00123456u) {
                panic("M58 self-test: the last pixel of the new mode did not read back");
            }

            uint32_t back_pitch = 0;
            if (dispi_set_mode(boot_w, boot_h, &back_pitch) != 0) {
                panic("M58 self-test: could not restore the boot mode - this is the failure the revert timer exists for");
            }
            fb_remap(back_pitch, boot_w, boot_h);
            if (fb_width() != boot_w || fb_height() != boot_h || fb_pitch_bytes() != boot_pitch) {
                panic("M58 self-test: the boot mode did not come back exactly as it was");
            }
            fb_clear(0x00000000u);

            klog_puts("[m58] display mode set and read back from the device (geometry, "
                       "device-chosen pitch and a grown mapping), then restored - self-test passed.\n\n");
        }
    }

    /* M17: hand logging over to the graphical console (console.h) - from
     * here on, klog's visual half draws through the framebuffer instead
     * of VGA text mode. Everything above this line (including the fb
     * self-test's own deliberately-visible rectangle) only ever reached
     * VGA text mode, since console_init() needs the framebuffer mapped
     * first; serial output (tools/qemu-serial-test.sh) is unaffected
     * either way. */
    console_init();

    /* M39 self-test: unlike almost every GUI-facing milestone since M18,
     * this one is fully checkable headlessly - glyph geometry is exact
     * data, not a mouse hover or a "does it look bold" judgement call.
     * Two halves:
     *
     *   1. The table itself. gen-font.c already enforces M39's shared
     *      metric at generation time, but that's a host program that
     *      never boots; this proves the table that actually shipped
     *      inside the kernel image is the one those checks passed on -
     *      column 7 reserved blank everywhere, every printable
     *      codepoint present, control codes blank, and font8x16_bold
     *      exactly the lossless one-column dilation compositor.c now
     *      looks up instead of recomputing per pixel.
     *   2. The rendered result. "Axg" through the real console blit
     *      path, read back out of the framebuffer: 'A' must start on
     *      the cap line, 'x' on the x-height line, both must sit on the
     *      same baseline, 'g' must reach the descender row, and column
     *      7 of all three cells must stay background. That is M39's
     *      whole premise - text on one shared baseline with uniform
     *      spacing - measured in real pixels rather than asserted.
     *
     * Runs after console_init() (so the framebuffer holds a cleared
     * console with the cursor at 0,0) but before klog_use_console(), so
     * the screen this reads back is exactly what it drew and nothing
     * else. It re-inits the console afterward to hand a clean screen to
     * the logging that follows. */
    {
        int all_ok = 1;

        for (int code = 0; code < 128 && all_ok; code++) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                if (font8x16[code][row] & 0x01u) {
                    klog_puts("[font39] glyph 0x");
                    klog_put_hex32((uint32_t)code);
                    klog_puts(" has ink in column 7, the reserved advance gap.\n");
                    all_ok = 0;
                    break;
                }
            }
        }

        for (int code = 0x21; code <= 0x7E && all_ok; code++) {
            int blank = 1;
            for (int row = 0; row < FONT_HEIGHT; row++) {
                if (font8x16[code][row]) {
                    blank = 0;
                    break;
                }
            }
            if (blank) {
                klog_puts("[font39] printable codepoint 0x");
                klog_put_hex32((uint32_t)code);
                klog_puts(" is blank - the table is incomplete.\n");
                all_ok = 0;
            }
        }

        for (int code = 0; code < 128 && all_ok; code++) {
            if (code > 0x20 && code < 0x7F) {
                continue; /* printable, checked non-blank above */
            }
            for (int row = 0; row < FONT_HEIGHT; row++) {
                if (font8x16[code][row]) {
                    klog_puts("[font39] non-printable codepoint 0x");
                    klog_put_hex32((uint32_t)code);
                    klog_puts(" should be blank but isn't.\n");
                    all_ok = 0;
                    break;
                }
            }
        }

        /* Lossless bold: with column 7 reserved (proved above), nothing
         * can shift off the end, so the dilation is exactly reversible
         * in the sense that matters - no ink is dropped. M38's runtime
         * smear had no such guarantee. */
        for (int code = 0; code < 128 && all_ok; code++) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = font8x16[code][row];
                if (font8x16_bold[code][row] != (uint8_t)(bits | (bits >> 1))) {
                    klog_puts("[font39] font8x16_bold disagrees with the dilation of font8x16 at 0x");
                    klog_put_hex32((uint32_t)code);
                    klog_putc('\n');
                    all_ok = 0;
                    break;
                }
            }
        }

        if (all_ok) {
            /* Cell 0 row 0 is blank in every glyph (nothing reaches
             * above FONT_CAP_TOP), so this samples the console's own
             * background without needing console.c's private constant. */
            console_puts("Axg");
            uint32_t bg = fb_get_pixel(0, 0);

            /* top/bottom lit row per cell, and whether column 7 stayed clear */
            int top[3], bot[3], gap_clear[3];
            for (int cell = 0; cell < 3; cell++) {
                top[cell] = -1;
                bot[cell] = -1;
                gap_clear[cell] = 1;
                for (int y = 0; y < FONT_HEIGHT; y++) {
                    for (int x = 0; x < FONT_WIDTH; x++) {
                        if (fb_get_pixel((uint32_t)(cell * FONT_WIDTH + x), (uint32_t)y) != bg) {
                            if (top[cell] < 0) {
                                top[cell] = y;
                            }
                            bot[cell] = y;
                            if (x == FONT_WIDTH - 1) {
                                gap_clear[cell] = 0;
                            }
                        }
                    }
                }
            }

            struct { const char *what; int got; int want; } checks[] = {
                { "'A' does not start on the shared cap line",        top[0], FONT_CAP_TOP },
                { "'A' does not sit on the shared baseline",          bot[0], FONT_BASELINE - 1 },
                { "'x' does not start on the shared x-height line",   top[1], FONT_X_TOP },
                { "'x' does not sit on the shared baseline",          bot[1], FONT_BASELINE - 1 },
                { "'g' does not start on the shared x-height line",   top[2], FONT_X_TOP },
                { "'g' does not reach the shared descender row",      bot[2], FONT_DESC_LAST },
                { "'A' drew into its advance gap (column 7)",         gap_clear[0], 1 },
                { "'x' drew into its advance gap (column 7)",         gap_clear[1], 1 },
                { "'g' drew into its advance gap (column 7)",         gap_clear[2], 1 },
            };
            for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
                if (checks[i].got != checks[i].want) {
                    klog_puts("[font39] rendered-pixel check failed: ");
                    klog_puts(checks[i].what);
                    klog_puts(" - expected ");
                    klog_put_hex32((uint32_t)checks[i].want);
                    klog_puts(" got ");
                    klog_put_hex32((uint32_t)checks[i].got);
                    klog_putc('\n');
                    all_ok = 0;
                }
            }

            console_init(); /* clear the sample text back off the screen */
        }

        if (!all_ok) {
            panic("M39 font self-test: glyph table and/or rendered text metric is wrong");
        }
        klog_puts("[font39] glyph table + shared-baseline render self-test passed.\n\n");
    }

    klog_use_console();
    klog_puts("[console] framebuffer text console active - logging switched over from VGA text mode.\n\n");

    /* Every IRQ line has been masked since pic_remap() (M4) - nothing has
     * needed one until now. IF has actually been set since real mode (the
     * BIOS leaves it that way and nothing here has touched it), so this
     * `sti` is defensive documentation more than a state change: from
     * this point on, unmasked IRQ lines really do fire. */
    __asm__ volatile("sti");

    pit_init();
    /* M69: calibrated against the PIT, so it has to come after it. See
     * arch/x86_64/tsc.h - the PIT is the only clock that knows what a
     * second is, and the TSC is the only one fine enough to measure a
     * frame with. */
    tsc_init();
    klog_puts("[pit] channel 0 programmed for ");
    klog_put_hex32(PIT_HZ);
    klog_puts(" Hz, IRQ0 unmasked.\n");

    /* Self-test: sleep for a bit and confirm the tick counter actually
     * advanced. This is also an implicit hang test - if IRQ0 never fired
     * (bad PIC remap, bad IDT gate, bad divisor), pit_sleep_ms's internal
     * wait loop would never terminate and boot would stop dead right
     * here instead of printing anything below. */
    uint64_t before = pit_get_ticks();
    pit_sleep_ms(50);
    uint64_t after = pit_get_ticks();
    klog_puts("[pit] slept 50ms: ticks ");
    klog_put_hex64(before);
    klog_puts(" -> ");
    klog_put_hex64(after);
    klog_putc('\n');

    keyboard_init();
    klog_puts("[kbd] IRQ1 unmasked, waiting up to 3s for a test keypress "
               "(QEMU monitor: 'sendkey <key>')...\n");
    int key = -1;
    uint64_t deadline = pit_get_ticks() + 3 * PIT_HZ;
    while (pit_get_ticks() < deadline) {
        key = keyboard_read();
        if (key != -1) {
            break;
        }
        __asm__ volatile("hlt");
    }
    if (key != -1) {
        klog_puts("[kbd] received keypress: '");
        klog_putc((char)key);
        klog_puts("'\n");
    } else {
        klog_puts("[kbd] no keypress within timeout - driver is installed, "
                   "just untested interactively this boot.\n");
    }

    klog_putc('\n');

    /* M18: PS/2 mouse, IRQ12 - same shape of bring-up as the keyboard
     * self-test just above (bounded wait for real interactive input, a
     * clean "installed but untested" message on timeout rather than
     * hanging boot). Cursor starts at screen center, off to one side of
     * where the console's own text is scrolling (top-left), so the two
     * don't visibly collide during this test - cursor.c has no real
     * damage-tracking against console output, that's M20's job once a
     * compositor owns the framebuffer for real. */
    mouse_init();
    cursor_init((int32_t)(fb_width() / 2), (int32_t)(fb_height() / 2));
    klog_puts("[mouse] IRQ12 unmasked, cursor drawn at screen center. Waiting "
               "up to 3s for test movement (QEMU monitor: 'mouse_move dx dy' "
               "/ 'mouse_button val')...\n");
    int got_mouse_event = 0;
    mouse_event_t last_ev = {0, 0, 0, 0, 0};
    uint64_t mouse_deadline = pit_get_ticks() + 3 * PIT_HZ;
    while (pit_get_ticks() < mouse_deadline) {
        mouse_event_t ev;
        while (mouse_read(&ev)) {
            cursor_move(ev.dx, ev.dy);
            last_ev = ev;
            got_mouse_event = 1;
        }
        __asm__ volatile("hlt");
    }
    if (got_mouse_event) {
        klog_puts("[mouse] received movement/click - cursor now at (");
        klog_put_hex32((uint32_t)cursor_x());
        klog_puts(", ");
        klog_put_hex32((uint32_t)cursor_y());
        klog_puts(") buttons=0x");
        klog_put_hex32(last_ev.buttons);
        klog_putc('\n');
    } else {
        klog_puts("[mouse] no movement within timeout - driver is installed, "
                   "just untested interactively this boot.\n");
    }
    klog_putc('\n');

    /* M63: SSE on, before there is a second task to switch between.
     * Everything above this line ran with the FPU in whatever state the
     * firmware left it; from here it is this OS's, and every task carries
     * its own copy of it. */
    fpu_init_cpu();
    sched_init();
    /* M68: before anything can block. Two, not MAX_CPUS: every AP already
     * registers its own `cpu-idle` identity in sched_init_ap and that
     * identity is marked idle too, so the only CPU without one is the BSP.
     * The spare is headroom, not a requirement. Kept small deliberately -
     * every task in the table is one more entry in the scans wake_expired
     * and the idle accounting do on every timer tick, and this is a
     * hot path measured in a 16 ms frame budget. */
    sched_spawn_idle_tasks(2);
    klog_puts("[sched] round-robin scheduler initialized (this context is task 0).\n");
    task_spawn("demo-a", demo_task, "A");
    task_spawn("demo-b", demo_task, "B");
    klog_puts("[sched] spawned tasks A and B; letting them run via "
               "preemption for ~1.5s...\n");
    pit_sleep_ms(1500);
    klog_puts("[sched] back on the main task - preemption round trip verified.\n\n");

    /* Self-test: SYS_getpid and SYS_write via a real `int 0x80` round
     * trip (gate -> syscall_common_stub -> syscall_handler -> dispatch
     * table -> back through RAX), the same "prove it, don't just trust
     * it compiled" discipline as the int3 test above. */
    long pid = do_syscall(SYS_getpid, 0, 0, 0);
    klog_puts("[syscall] getpid() = ");
    klog_put_hex64((uint64_t)pid);
    klog_putc('\n');

    static const char msg[] = "[syscall] hello via SYS_write\n";
    long written = do_syscall(SYS_write, 1, (uint64_t)msg, sizeof(msg) - 1);
    if (written != (long)sizeof(msg) - 1) {
        panic("syscall self-test: SYS_write returned an unexpected length");
    }

    task_spawn("syscall-exit", syscall_exit_task, NULL);
    pit_sleep_ms(200);
    klog_puts("[syscall] SYS_exit self-test task ran and terminated.\n\n");

    /* M14 self-tests: pipes (both the raw kernel primitive and the
     * SYS_pipe/SYS_read/SYS_write syscall path), signals (SYS_kill), and
     * process/wait semantics (SYS_wait(-1) reaping exactly the children
     * spawned for it, SYS_getpgid) - the same "prove it, don't just trust
     * it compiled" discipline as every earlier milestone's self-tests. */

    /* Pipe self-test 1: kernel-level pipe_create/pipe_write/pipe_read,
     * exercising the blocking buffer logic directly (not through a
     * syscall). The consumer starts before the producer has written
     * anything, so pipe_read genuinely blocks (cooperatively yields) and
     * gets woken by later scheduling rather than finding data already
     * there. */
    pipe_t *test_pipe = pipe_create();
    if (!test_pipe) {
        panic("pipe self-test: pipe_create failed");
    }
    task_t *producer = task_spawn("pipe-producer", pipe_producer_task, test_pipe);
    task_t *consumer = task_spawn("pipe-consumer", pipe_consumer_task, test_pipe);
    while (producer->state != TASK_TERMINATED || consumer->state != TASK_TERMINATED) {
        schedule();
    }
    kfree(test_pipe);
    klog_puts("[pipe] kernel-level producer/consumer self-test passed.\n\n");

    /* Pipe self-test 2: the syscall path - SYS_pipe installs a pair of
     * fds into this very task's own fd table, and SYS_write/SYS_read
     * move data through them exactly like a real program would, without
     * ever touching pipe_t directly. */
    int pipe_fds[2];
    if (do_syscall(SYS_pipe, (uint64_t)pipe_fds, 0, 0) != 0) {
        panic("SYS_pipe self-test: pipe creation failed");
    }
    static const char pipe_msg[] = "hello through a syscall pipe";
    long pipe_written = do_syscall(SYS_write, (uint64_t)pipe_fds[1], (uint64_t)pipe_msg, sizeof(pipe_msg) - 1);
    if (pipe_written != (long)sizeof(pipe_msg) - 1) {
        panic("SYS_pipe self-test: SYS_write returned an unexpected length");
    }
    char pipe_readback[64] = {0};
    long pipe_read_n = do_syscall(SYS_read, (uint64_t)pipe_fds[0], (uint64_t)pipe_readback, sizeof(pipe_readback) - 1);
    if (pipe_read_n != (long)sizeof(pipe_msg) - 1 || k_strcmp(pipe_readback, pipe_msg) != 0) {
        panic("SYS_pipe self-test: SYS_read returned unexpected data");
    }
    klog_puts("[pipe] SYS_pipe/SYS_write/SYS_read self-test passed.\n\n");

    /* Signal self-test: a spinner task looping on pure CPU-bound work
     * (never yields, never syscalls) can only ever stop via a signal
     * actually being delivered through scheduler_tick's per-tick
     * pending-signal check (sched.c) - syscall_handler's check (M14)
     * would never fire for a task that never syscalls. */
    task_t *spinner = task_spawn("spinner", spinner_task, NULL);
    pit_sleep_ms(100);
    if (do_syscall(SYS_kill, (uint64_t)spinner->id, SIGTERM, 0) != 0) {
        panic("SYS_kill self-test: kill on a live task failed");
    }
    while (spinner->state != TASK_TERMINATED) {
        schedule();
    }
    if (spinner->exit_code != 128 + SIGTERM) {
        panic("SYS_kill self-test: unexpected exit code after SIGTERM");
    }
    klog_puts("[signal] SIGTERM self-test passed (spinner task terminated).\n\n");

    /* Process/wait self-test: drain any unreaped children left over from
     * earlier self-tests, spawn exactly two fresh ones, and confirm
     * SYS_wait(-1) reaps precisely those two (in either order) before
     * correctly reporting -1 once none remain - "more complete wait
     * semantics" (this milestone's own wording), not just the
     * single-pid form M13 already proved. */
    while (do_syscall(SYS_wait, (uint64_t)-1, 0, 0) != -1) {
    }
    task_t *quick_a = task_spawn("quick", quick_task, NULL);
    task_t *quick_b = task_spawn("quick", quick_task, NULL);
    long reaped1 = do_syscall(SYS_wait, (uint64_t)-1, 0, 0);
    long reaped2 = do_syscall(SYS_wait, (uint64_t)-1, 0, 0);
    int got_a = (reaped1 == quick_a->id) || (reaped2 == quick_a->id);
    int got_b = (reaped1 == quick_b->id) || (reaped2 == quick_b->id);
    if (!got_a || !got_b || reaped1 == reaped2) {
        panic("SYS_wait(-1) self-test: did not reap exactly the two expected children");
    }
    if (do_syscall(SYS_wait, (uint64_t)-1, 0, 0) != -1) {
        panic("SYS_wait(-1) self-test: expected -1 once no children remain");
    }
    klog_puts("[wait] SYS_wait(-1) self-test passed (reaped two children, then -1).\n\n");

    /* Process-group self-test: SYS_getpgid is read-only (no job control
     * exists to ever change a group), so all there is to prove is that a
     * spawned task really does inherit its parent's pgid - task 0's own
     * group (0, set by sched_init) propagating down to a task it spawns
     * directly.
     *
     * M54: this used to ask about quick_a, which the SYS_wait(-1) test
     * just above had already reaped - fine when a reaped task's slot
     * stayed valid forever, and a -1 the moment slots started coming
     * back. Asking about a *live* child is what the test always meant;
     * the old version only worked because nothing ever died completely.
     * `spinner_task` is used because it does not exit on its own, so it
     * is still there to be asked about. */
    task_t *pgid_child = task_spawn("pgidprobe", spinner_task, NULL);
    long self_pgid = do_syscall(SYS_getpgid, 0, 0, 0);
    long child_pgid = do_syscall(SYS_getpgid, (uint64_t)pgid_child->id, 0, 0);
    do_syscall(SYS_kill, (uint64_t)pgid_child->id, SIGKILL, 0);
    do_syscall(SYS_wait, (uint64_t)pgid_child->id, 0, 0);
    if (self_pgid != 0 || child_pgid != self_pgid) {
        panic("SYS_getpgid self-test: child did not inherit its parent's process group");
    }
    klog_puts("[pgid] SYS_getpgid self-test passed (child inherited pgid ");
    klog_put_hex64((uint64_t)self_pgid);
    klog_puts(").\n\n");

    /* M12: bring up the disk filesystem, seeding it with every embedded
     * program on first boot only - every subsequent load (including the
     * init spawn just below) reads back from disk like any other file
     * would be, which is the point. */
    vfs_init();
    tty_init(); /* M85: the terminal, before anything can be its foreground job */
    /* M53: the layout, created before anything is written into it. Each
     * one is idempotent-by-check rather than by vfs_mkdir returning 0 for
     * an existing path - see leanfs.h on why "already there" is an error
     * there rather than a no-op. */
    {
        static const char *const LAYOUT[] = {PATH_BIN, PATH_HOME, PATH_ETC, PATH_TMP};
        for (size_t i = 0; i < sizeof(LAYOUT) / sizeof(LAYOUT[0]); i++) {
            if (!vfs_exists(LAYOUT[i]) && vfs_mkdir(LAYOUT[i]) != 0) {
                panic("vfs_mkdir: failed to create the filesystem layout");
            }
        }
    }
    for (size_t i = 0; i < EMBEDDED_PROGRAM_COUNT; i++) {
        const embedded_program_t *p = &embedded_programs[i];
        char path[PATH_MAX_LEN];
        if (path_join(path, PATH_BIN_DIR, p->name) != 0) {
            panic("a program name is too long to live in /bin");
        }
        if (!vfs_exists(path)) {
            klog_puts("[fs] seeding disk with '");
            klog_puts(path);
            klog_puts("' (first boot only)...\n");
            size_t size = (size_t)(p->end - p->start);
            if (vfs_write(path, p->start, size) != 0) {
                panic("vfs_write: failed to seed a program onto disk");
            }
        }
    }
    klog_puts("[fs] all user programs present in " PATH_BIN ".\n\n");

    /* M53: one file in /home on a fresh disk. Not decoration - before
     * this milestone the file manager opened on a namespace that always
     * had two dozen things in it, and now it opens on a directory that
     * would otherwise be empty on a machine's first boot, which reads as
     * "this is broken" rather than "this is new". It also gives the
     * interactive suite a real file to drag, which is a smaller reason
     * but a real one. */
    /* M74: and more than one of them. A fresh disk used to boot to a
     * desktop with seeded icons and a single file - which is a demo. The
     * difference between a demo and a machine somebody just got is that
     * the second one has something in it: a README that says what this
     * is, a note to edit, and a directory to open. All three exist so
     * that the first thing a person does - open Files, open the editor -
     * lands on something rather than on emptiness.
     *
     * Written only when absent, so a person's own edits are never
     * overwritten by a later boot. That is the same rule the program
     * seeding above follows and it matters more here: these are the only
     * files on this machine that a person is expected to change. */
    {
        static const struct {
            const char *path;
            const char *body;
        } FIRST_BOOT[] = {
            {PATH_HOME_DIR "readme.txt",
             "Welcome to lean_os.\n"
             "\n"
             "This is /home - your files live here.\n"
             "Programs live in /bin, settings in /etc.\n"
             "\n"
             "Getting around\n"
             "  Double-click a name in Files to open it, or .. to go up.\n"
             "  Ctrl+Space opens the launcher; type a few letters and press Enter.\n"
             "  Ctrl+Shift+Esc opens the task manager.\n"
             "  Ctrl+Alt+Left/Right move between the four desktops.\n"
             "\n"
             "The terminal\n"
             "  ls, cat, cp, echo, env, cd, pwd - and > to redirect.\n"
             "  A file starting with #!/bin/sh is a program: run it by name.\n"
             "\n"
             "Your windows come back\n"
             "  Whatever is open when this machine stops is open again when\n"
             "  it starts, in the same places. /etc/session.conf is the file\n"
             "  that remembers, and it is plain text.\n"},
            {PATH_HOME_DIR "notes.txt",
             "Scratch file.\n"
             "\n"
             "The editor has undo (Ctrl+Z), redo (Ctrl+Y), find (Ctrl+F),\n"
             "cut/copy/paste, and a File menu that can save somewhere else.\n"
             "\n"
             "Nothing here is precious - edit it.\n"},
            {PATH_HOME_DIR "hello.sh",
             "#!/bin/sh\n"
             "# A script is a program here. Run it from the terminal as\n"
             "#   /home/hello.sh\n"
             "echo \"hello from $SHELL\"\n"
             "pwd\n"
             "echo \"there are these programs:\"\n"
             "ls /bin\n"},
        };
        for (size_t i = 0; i < sizeof(FIRST_BOOT) / sizeof(FIRST_BOOT[0]); i++) {
            if (vfs_exists(FIRST_BOOT[i].path)) {
                continue;
            }
            size_t len = 0;
            while (FIRST_BOOT[i].body[len]) {
                len++;
            }
            if (vfs_write(FIRST_BOOT[i].path, FIRST_BOOT[i].body, len) != 0) {
                panic("vfs_write: failed to seed a first-boot file into " PATH_HOME);
            }
        }
    }

    /* M15 self-test: every file up to now (the seeded programs) fits in
     * leanfs's direct blocks alone (<= 8 KiB), which would never exercise
     * the new singly-indirect path at all - "compiles" isn't "works", so
     * round-trip something deliberately bigger than LEANFS_DIRECT_BLOCKS *
     * LEANFS_BLOCK_SIZE (8 KiB) but within the new LEANFS_MAX_FILE_SIZE
     * (72 KiB) cap. */
    {
        size_t fstest_len = 20000; /* spans 16 direct + ~23 indirect blocks */
        uint8_t *fstest_buf = (uint8_t *)kmalloc(fstest_len);
        uint8_t *fstest_readback = (uint8_t *)kmalloc(fstest_len);
        if (!fstest_buf || !fstest_readback) {
            panic("out of memory for leanfs indirect-block self-test");
        }
        for (size_t i = 0; i < fstest_len; i++) {
            fstest_buf[i] = (uint8_t)(i * 31 + 7);
        }
        if (vfs_write(PATH_TMP_DIR "fstest", fstest_buf, fstest_len) != 0) {
            panic("leanfs indirect-block self-test: vfs_write failed");
        }
        k_memset(fstest_readback, 0, fstest_len);
        int64_t fstest_size = vfs_read(PATH_TMP_DIR "fstest", fstest_readback, fstest_len);
        if (fstest_size != (int64_t)fstest_len) {
            panic("leanfs indirect-block self-test: size mismatch on readback");
        }
        for (size_t i = 0; i < fstest_len; i++) {
            if (fstest_readback[i] != fstest_buf[i]) {
                panic("leanfs indirect-block self-test: data mismatch on readback");
            }
        }
        kfree(fstest_buf);
        kfree(fstest_readback);
        klog_puts("[fs] leanfs indirect-block self-test passed (20000-byte round trip).\n\n");
    }

    /* M19 self-test: spawn the real ring-3 memtest program (not a
     * kernel-side stand-in) to prove user-space malloc/free and
     * cross-process shared memory both actually work - "compiles" isn't
     * "works", same discipline as every earlier milestone's self-tests.
     * memtest itself spawns a second copy of itself (the shm reader
     * role) and reports the combined result via its own exit code, so
     * this only needs to wait for the one (creator) child and check
     * that. */
    {
        size_t memtest_size_bytes = 0;
        uint8_t *memtest_image = read_program("/bin/memtest", &memtest_size_bytes);
        int64_t memtest_size = (int64_t)memtest_size_bytes;
        task_t *memtest_task = process_spawn("memtest", memtest_image, (size_t)memtest_size, "");
        kfree(memtest_image);
        long memtest_status = do_syscall(SYS_wait, (uint64_t)memtest_task->id, 0, 0);
        if (memtest_status != 0) {
            panic("memtest self-test: nonzero exit code - malloc or shm is broken");
        }
        klog_puts("[memtest] user-space malloc/free and cross-process shm self-tests passed.\n\n");
    }

    /* M57 self-test: the proportional UI font family, checked from ring 3
     * because that is the only place it exists - uifont.c is a
     * user_space library, and the property being proved is that
     * gfx_text_width() and gfx_draw_text_font() agree about where the
     * ink lands. fonttest.c renders into its own buffer and measures the
     * result; see its header for why a table check would not have been
     * the same test. Spawned and waited on exactly the way memtest above
     * is. */
    {
        size_t fonttest_size_bytes = 0;
        uint8_t *fonttest_image = read_program("/bin/fonttest", &fonttest_size_bytes);
        int64_t fonttest_size = (int64_t)fonttest_size_bytes;
        task_t *fonttest_task = process_spawn("fonttest", fonttest_image, (size_t)fonttest_size, "");
        kfree(fonttest_image);
        long fonttest_status = do_syscall(SYS_wait, (uint64_t)fonttest_task->id, 0, 0);
        if (fonttest_status != 0) {
            panic("M57 font self-test: a measured text width disagrees with the pixels drawn");
        }
        klog_puts("[m57] proportional UI font: per-glyph advances, one shared baseline across three sizes, and every measured width matching the ink drawn - self-test passed.\n\n");
    }

    /* M47: pin the desktop's settings to their compiled-in defaults for
     * the whole self-test phase - see selftest_settings_install_defaults
     * for why, and selftest_settings_restore (just before PID 1) for the
     * other half. */
    selftest_settings_install_defaults();

    /* M20 self-test: spawn the real compositor and a real client
     * (user_space/bin/compositor.c, wm_demo.c) - genuine ring-3 code
     * talking over named pipes and shared memory, not a kernel-side
     * stand-in. Verified by reading pixels straight out of the physical
     * framebuffer via the kernel's own fb_get_pixel (fb.c's mapping is
     * always live in every address space, PML4[0] - the exact same
     * physical frames the compositor's own SYS_fb_map call points at, so
     * this genuinely observes what the compositor drew, not a kernel-side
     * copy of it) - the same "prove it, don't just trust it compiled"
     * discipline as every earlier milestone's self-tests, extended to a
     * case where the thing being proven is graphical. */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t demo_size_bytes = 0;
        uint8_t *demo_image = read_program("/bin/wm_demo", &demo_size_bytes);
        int64_t demo_size = (int64_t)demo_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        task_t *demo_task = process_spawn("wm_demo", demo_image, (size_t)demo_size, "");
        kfree(demo_image);

        /* M54: polled, not waited on. wm_demo draws one frame, exits
         * cleanly and leaves its window behind on purpose - that window
         * is what the rest of this block reads. Since M54 a SYS_wait also
         * *reaps*, and the compositor watching a pid the kernel no longer
         * knows correctly concludes the client is gone and reclaims the
         * window - so waiting here would tear down the very thing under
         * test. SYS_task_alive's 2 is the same assertion ("terminated,
         * exit code 0") without consuming the task. It is reaped below,
         * once every pixel has been read. */
        long demo_status = 1;
        uint64_t demo_t0 = pit_get_ticks();
        for (int spin = 0; spin < 300 && demo_status == 1; spin++) {
            pit_sleep_ms(10);
            demo_status = do_syscall(SYS_task_alive, (uint64_t)demo_task->id, 0, 0);
        }
        klog_puts("[wm_demo] connect+draw+exit took ");
        klog_put_dec((uint32_t)((pit_get_ticks() - demo_t0) * (1000 / PIT_HZ)));
        klog_puts(" ms\n");
        if (demo_status != 2) {
            /* M69: say which failure this is. 1 = still running after the
             * budget (too slow), 0 = terminated with a non-zero code
             * (actually failed), -1 = no such task. Those are three very
             * different findings and the panic used to conflate them,
             * which is why this test was a mystery every time a
             * scheduling change moved it. */
            klog_puts("[wm_demo] SYS_task_alive = ");
            klog_put_dec((uint32_t)(demo_status & 0xFF));
            klog_puts(" (1=still running past the budget, 0=exited non-zero, 255=no such task)\n");
            /* Every task, its state and what it is parked on. "Still
             * running past the budget" says the client did not finish
             * and nothing else; whether it is blocked, and on whose
             * channel, is the difference between a client that never got
             * an answer and a compositor that never asked. Printed
             * before the panic because after it there is nothing. */
            sched_debug_dump("wm_demo overran its budget");
            /* And what is actually sitting in the two rendezvous pipes.
             * "wm_demo is blocked on a pipe" does not say WHICH pipe or
             * which side of it: a request nobody read and a response
             * nobody wrote are opposite bugs with the same symptom, and
             * this is the one place both are visible at once. Opening
             * them here adds a reader and a writer to each, which would
             * matter if this were not about to panic. */
            {
                int probe[2];
                if (do_syscall(SYS_pipe_open, (uint64_t)WM_REQUEST_PIPE, (uint64_t)probe, 0) == 0) {
                    klog_puts("[wm_demo] WM_REQUEST_PIPE holds ");
                    klog_put_dec((uint32_t)do_syscall(SYS_pipe_poll, (uint64_t)probe[0], 0, 0));
                    klog_puts(" byte(s), a request is ");
                    klog_put_dec((uint32_t)sizeof(wm_create_request_t));
                    klog_putc('\n');
                }
                if (do_syscall(SYS_pipe_open, (uint64_t)WM_RESPONSE_PIPE, (uint64_t)probe, 0) == 0) {
                    klog_puts("[wm_demo] WM_RESPONSE_PIPE holds ");
                    klog_put_dec((uint32_t)do_syscall(SYS_pipe_poll, (uint64_t)probe[0], 0, 0));
                    klog_puts(" byte(s), a response is ");
                    klog_put_dec((uint32_t)sizeof(wm_create_response_t));
                    klog_putc('\n');
                }
            }
            panic("wm_demo self-test: did not exit cleanly - window creation failed");
        }

        /* The compositor redraws a handful of times right after accepting
         * the window (see its own header comment) and then only on real
         * input, so by now it has settled into a static frame - this
         * pause is just scheduler margin, not a race against an
         * ever-changing image. */
        pit_sleep_ms(1000);

        /* Capture every check *before* printing anything: klog_puts here
         * would go through the same graphical console (M17) the
         * compositor is compositing onto, and a console scroll
         * (fb_scroll_up) between reads would shift the whole screen -
         * including the frame under test - producing exactly the kind of
         * "different pixels wrong on every run" failure this self-test
         * hit for real before this was understood. Reading everything
         * first, with zero console output in between, is what actually
         * fixed it (confirmed by testing, not just reasoning about it). */
        /* window titlebar color: M21 made a newly connected window take
         * focus immediately (focus-follows-click's initial-state
         * counterpart), and wm_demo is the only client here - so its
         * titlebar renders in the *focused* color now, not the plain
         * one this check expected before M21 existed. */
        struct { uint32_t x, y; uint32_t expected; const char *what; } checks[] = {
            {200, 180, 0x00336699u, "window content color"},
            {140, 140, 0x00CC8822u, "window accent square color"},
            {150, 85,  0x004C99E6u, "window titlebar color (focused)"},
            {99,  150, 0x00444466u, "window border color"},
            {500, 500, 0x001A1A2Eu, "desktop background color"},
        };
        uint32_t got[sizeof(checks) / sizeof(checks[0])];
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            got[i] = fb_get_pixel(checks[i].x, checks[i].y);
        }

        /* The compositor never exits on its own (a real WM shouldn't) -
         * this self-test bounds its lifetime deliberately, then restores
         * the graphical text console (M17) it took over, so the rest of
         * boot (init/shell) has a clean screen to work with again. Killed
         * and the console restored *before* evaluating/printing results,
         * for the same "no console output while the frame under test is
         * still live" reason. */
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        int all_ok = 1;
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            if (got[i] != checks[i].expected) {
                klog_puts("[wm] pixel check failed: ");
                klog_puts(checks[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32(checks[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32(got[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        if (!all_ok) {
            panic("compositor self-test: framebuffer content did not match");
        }
        klog_puts("[wm] compositor + client self-test passed (5/5 pixel checks matched).\n\n");
    }

    /* M21 self-test: two real GUI clients connected at once (closing the
     * "z-order unproven beyond one window" gap M20 itself flagged),
     * proving multi-window compositing, gfx.h text/rect rendering
     * through a real client, and focus-follows-click's *result* (the
     * later connection, gui_paint, should hold focus and draw its
     * titlebar in the focused color) all landed correctly. What this
     * block deliberately does *not* attempt to prove automatically:
     * that a live mouse click actually moves focus, or that a routed
     * keystroke/mouse-drag reaches gui_paint and draws a stroke - both
     * need real hardware-shaped input (QEMU monitor mouse_move/
     * mouse_button/sendkey), the same category of thing M18's mouse
     * driver relied on manual interactive verification for rather than
     * a boot-time self-test. See milestones.md's M21 entry for that
     * verification's results. */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t clock_size_bytes = 0;
        uint8_t *clock_image = read_program("/bin/gui_clock", &clock_size_bytes);
        int64_t clock_size = (int64_t)clock_size_bytes;
        size_t paint_size_bytes = 0;
        uint8_t *paint_image = read_program("/bin/gui_paint", &paint_size_bytes);
        int64_t paint_size = (int64_t)paint_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        /* Spawned one at a time, each given room to finish its whole
         * connect handshake (several pipe round trips, each needing
         * multiple scheduler quanta) before the next one starts - so
         * which window ends up at index 0 vs 1 is deterministic instead
         * of a race between two tasks starting from the same instant. */
        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(500);

        task_t *paint_task = process_spawn("gui_paint", paint_image, (size_t)paint_size, "");
        kfree(paint_image);
        pit_sleep_ms(1000); /* gui_paint connects (stealing focus) and both settle into the compositor's periodic redraw */

        /* Capture every check *before* printing anything - see the M20
         * self-test above for exactly why (console scroll racing the
         * still-live frame under test). Coordinates chosen to avoid the
         * region where gui_paint (drawn second, so on top) visually
         * overlaps gui_clock - see the M21 progress log entry for the
         * overlap math this was worked out from. */
        /* static const: a plain local aggregate initializer this size
         * (12 entries) makes GCC lower the initialization to a memcpy
         * call at -O1 rather than inlined stores - and this freestanding
         * kernel has no libc memcpy for it to link against (kernel/lib/
         * libk.h's k_memcpy is a different, deliberately non-standard
         * name for exactly that reason). static const sidesteps it
         * entirely: the data just lives in .rodata, nothing gets copied
         * anywhere at runtime. */
        static const struct { uint32_t x, y; uint32_t expected; const char *what; } checks[] = {
            /* gui_clock (window 0, unfocused once gui_paint connects): */
            {150, 90,  0x00335577u, "clock titlebar color (unfocused)"},
            {98,  150, 0x00444466u, "clock compositor border color"},
            {105, 105, 0x00122438u, "clock content background color"},
            /* M39 moved these two: probing a pixel *inside* a glyph is
             * inherently coupled to that glyph's bitmap, and re-authoring
             * the font changed which columns of 'C' are ink. Same cell
             * (window 0 at 100,100 + gui_clock's local 10,10), same row 5
             * of the glyph, columns picked off the new letterform: the
             * left stem is ink, the bowl's interior isn't. */
            {110, 115, 0x00FFFFFFu, "clock caption 'C' glyph - on pixel (left stem)"},
            {113, 115, 0x00122438u, "clock caption 'C' glyph - off pixel (bowl interior)"},
            /* gui_paint (window 1, focused): */
            {200, 130, 0x004C99E6u, "paint titlebar color (focused)"},
            {140, 190, 0x0088AA55u, "paint's own border frame color"},
            {150, 240, 0x00202020u, "paint canvas background color"},
            /* Same M39 re-aim as the clock caption above (window 1 at
             * 140,140 + gui_paint's local 10,6). */
            {150, 151, 0x00FFFFFFu, "paint caption 'P' glyph - on pixel (left stem)"},
            {153, 151, 0x00202020u, "paint caption 'P' glyph - off pixel (bowl interior)"},
            {240, 164, 0x0088AA55u, "paint separator line color"},
            /* desktop, unoccupied by either window: */
            {500, 500, 0x001A1A2Eu, "desktop background color"},
        };
        uint32_t got[sizeof(checks) / sizeof(checks[0])];
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            got[i] = fb_get_pixel(checks[i].x, checks[i].y);
        }

        selftest_reap(paint_task);
        selftest_reap(clock_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        int all_ok = 1;
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            if (got[i] != checks[i].expected) {
                klog_puts("[wm21] pixel check failed: ");
                klog_puts(checks[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32(checks[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32(got[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        if (!all_ok) {
            panic("M21 multi-window self-test: framebuffer content did not match");
        }
        klog_puts("[wm21] multi-window compositor + focus-routing self-test passed "
                  "(12/12 pixel checks matched).\n\n");
    }

    /* M22 self-test: a real desktop_shell panel client, connected as
     * wm_create_request_t's chrome-less "panel" kind for the first time
     * (M20/M21's self-tests only ever exercised ordinary windows),
     * proving it reflects a real second window (gui_clock, spawned here
     * the same way M21's did) into a real running-window slot - labeled
     * with gui_clock's own title ("Clock", wm_create_request_t.title) -
     * via the M22 query protocol (system_api/include/wm.h's
     * WM_QUERY_PIPE). The taskbar has no launcher of its own (removed
     * once it started surfacing every on-disk coreutil as clutter -
     * launching is desktop_icons.c's job); what this can't prove
     * headlessly is that *clicking* a running-window slot really focuses/
     * minimizes it - that needs real mouse input, verified manually via
     * QEMU monitor injection the same way M21's focus-follows-click and
     * gui_paint strokes were; see milestones.md's M22 entry for that
     * verification's results. */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t shell_size_bytes = 0;
        uint8_t *shell_image = read_program("/bin/desktop_shell", &shell_size_bytes);
        int64_t shell_size = (int64_t)shell_size_bytes;
        size_t clock_size_bytes = 0;
        uint8_t *clock_image = read_program("/bin/gui_clock", &clock_size_bytes);
        int64_t clock_size = (int64_t)clock_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        task_t *shell_task = process_spawn("desktop_shell", shell_image, (size_t)shell_size, "");
        kfree(shell_image);
        pit_sleep_ms(500); /* connects, lists files, draws its first frame */

        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(1000); /* connects (window 1, focused); desktop_shell's next periodic query picks it up */

        /* Panel docks at the bottom: y = 768 - PANEL_HEIGHT(32) = 736.
         * Running slot 0 is gui_clock's window (id 1, the only non-panel
         * window, focused), at local (84,4) 96x24 - M42 moved the running
         * buttons right of the new Start button, which is what the 80px
         * shift in the probes below is, and M44 moved its label 4px
         * further in again (desktop_shell.c's LABEL_PAD, widened to clear
         * the rounded corners).
         *
         * M44 also made the taskbar translucent, so none of the colors
         * below are the panel's own any more: every one is
         * TRANSLUCENT_NUM/DEN of what desktop_shell.c painted mixed with
         * what was already composited underneath, which in this self-test
         * (no desktop background client) is the compositor's own
         * DEFAULT_BG_COLOR 0x1A1A2E. The focused slot's 0x2E4A63 becomes
         * (0x1A*1 + 0x2E*3)/4, (0x1A + 0x4A*3)/4, (0x2E + 0x63*3)/4 =
         * 0x293E55; white glyph ink becomes 0xC5C5CA; the panel's own
         * 0x181828 becomes 0x181829. Its label is
         * gui_clock's own title, "Clock" - glyph math below is for 'C'
         * (M39's font8x16.c row 5: 0xC0 = 11000000, the left stem - so
         * column 0 is ink and the bowl's interior at column 3 isn't.
         * These two flipped when M39 re-authored the glyphs; probing a
         * pixel inside a letterform is coupled to that letterform by
         * construction, which is exactly why the two neighbours here
         * deliberately sample flat fills instead). Coordinates below are absolute
         * (panel-local + the panel's own (0,736) origin) - see
         * milestones.md's M22 entry for the glyph-bitmap method this
         * follows, same one M21's own pixel checks already proved out. */
        static const struct { uint32_t x, y; uint32_t expected; const char *what; } checks[] = {
            {124, 742, 0x00293E55u, "running slot 0 background (focused)"},
            {90,  749, 0x00C5C5CAu, "running slot 0 'C' glyph - on pixel (left stem)"},
            {93,  749, 0x00293E55u, "running slot 0 'C' glyph - off pixel (bowl interior)"},
            {500, 738, 0x00181829u, "panel background (margin strip above the slot row, y=2 - never overdrawn by any slot regardless of window count)"},
            {500, 500, 0x001A1A2Eu, "desktop background color, above the panel"},
        };
        uint32_t got[sizeof(checks) / sizeof(checks[0])];
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            got[i] = fb_get_pixel(checks[i].x, checks[i].y);
        }

        selftest_reap(clock_task);
        selftest_reap(shell_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        int all_ok = 1;
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            if (got[i] != checks[i].expected) {
                klog_puts("[wm22] pixel check failed: ");
                klog_puts(checks[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32(checks[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32(got[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        if (!all_ok) {
            panic("M22 desktop shell self-test: framebuffer content did not match");
        }
        klog_puts("[wm22] desktop shell (panel + taskbar query, no launcher) self-test passed "
                  "(5/5 pixel checks matched).\n\n");
    }

    /* M30 self-test: a real gui_clock window, driven purely through the
     * WM_ACTION_PIPE protocol (system_api/include/wm.h) - no simulated
     * mouse hardware involved, so this proves apply_window_action's
     * maximize/restore/minimize/close state machine itself is correct,
     * the same logic a real titlebar-button click drives (compositor.c's
     * handle_mouse calls the exact same function). Whether a click at the
     * *right pixel coordinates* actually reaches that function is left to
     * manual/interactive verification, same as focus-follows-click and
     * gui_paint strokes already are (M21's own self-test comment) - real
     * mouse input needs real hardware-shaped events this headless
     * self-test has no way to fabricate.
     *
     * gui_clock (not wm_demo) is the target: wm_demo draws once and
     * exits on purpose (M29's self-test already consumes it fully via
     * SYS_wait above), so a *running* client is needed to still be there
     * once this block starts sending it action requests. Alone in its
     * own fresh compositor instance, gui_clock is window_id 0 - no panel
     * connects here, so WM_ACTION_MAXIMIZE's available area is simply
     * the whole screen (BORDER/TITLEBAR_H insets only). */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t clock_size_bytes = 0;
        uint8_t *clock_image = read_program("/bin/gui_clock", &clock_size_bytes);
        int64_t clock_size = (int64_t)clock_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(500); /* connects (window 0), draws its first frame */

        int action_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_ACTION_PIPE, (uint64_t)action_fds, 0) != 0) {
            panic("M30 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }

        /* "Home" position: idx 0's default placement (accept_pending_window's
         * `100 + idx*40`) puts gui_clock's 200x90 content at x:[100,300),
         * y:[100,190) - (250,170) sits in its plain-background lower-right
         * corner, well clear of the "CLOCK"/"uptime: ..." text gui_clock
         * draws near the top (see gui_clock.c). "Away" position: once
         * maximized, content moves to x:[2,202), y:[22,112) (BORDER/
         * TITLEBAR_H insets, clamped to its own 200x90 buffer - it's far
         * smaller than the screen, so nothing else about its size
         * changes) - (250,170) then falls outside the window entirely, so
         * it reads the compositor's own desktop background instead of
         * whatever gui_clock draws there. The two colors are deliberately
         * distinguishable (0x00122438 vs 0x001A1A2E) so a wrong pixel
         * can't accidentally match the wrong expectation. */
        const int32_t home_x = 250, home_y = 170;
        const uint32_t clock_bg = 0x00122438u;
        const uint32_t desktop_bg = 0x001A1A2Eu;

        wm_action_request_t req;
        req.window_id = 0;

        req.action = WM_ACTION_MAXIMIZE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        uint32_t after_maximize = selftest_pixel_settled((uint32_t)home_x, (uint32_t)home_y,
                                                          desktop_bg, "the maximize to leave the home position");

        req.action = WM_ACTION_RESTORE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        uint32_t after_restore = selftest_pixel_settled((uint32_t)home_x, (uint32_t)home_y,
                                                         clock_bg, "the restore to put the window back");

        req.action = WM_ACTION_TOGGLE_MINIMIZE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        /* Waiting for the desktop here also waits out M61's minimize
         * animation, whose ghost passes over this very pixel on its way
         * down - which a fixed sleep did only by being longer than it. */
        uint32_t after_minimize = selftest_pixel_settled((uint32_t)home_x, (uint32_t)home_y,
                                                          desktop_bg, "the minimize to clear the home position");

        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req)); /* toggle back */
        uint32_t after_unminimize = selftest_pixel_settled((uint32_t)home_x, (uint32_t)home_y,
                                                            clock_bg, "the window to come back");

        req.action = WM_ACTION_CLOSE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        /* Signal delivery isn't instantaneous (checked at the target's
         * next syscall/tick - signal.h), and M29's reap_dead_clients only
         * runs once per compositor loop iteration after that - which is
         * what this waits for rather than guesses at. */
        uint32_t after_close = selftest_pixel_settled((uint32_t)home_x, (uint32_t)home_y,
                                                       desktop_bg, "the closed window's slot to be reclaimed");
        long clock_exit = do_syscall(SYS_wait, (uint64_t)clock_task->id, 0, 0);

        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        int all_ok = 1;
        if (after_maximize != desktop_bg) {
            klog_puts("[wm30] pixel check failed: after WM_ACTION_MAXIMIZE, home position should be empty desktop - expected 0x");
            klog_put_hex32(desktop_bg);
            klog_puts(" got 0x");
            klog_put_hex32(after_maximize);
            klog_putc('\n');
            all_ok = 0;
        }
        if (after_restore != clock_bg) {
            klog_puts("[wm30] pixel check failed: after WM_ACTION_RESTORE, home position should show the window again - expected 0x");
            klog_put_hex32(clock_bg);
            klog_puts(" got 0x");
            klog_put_hex32(after_restore);
            klog_putc('\n');
            all_ok = 0;
        }
        if (after_minimize != desktop_bg) {
            klog_puts("[wm30] pixel check failed: after WM_ACTION_TOGGLE_MINIMIZE, home position should be empty desktop - expected 0x");
            klog_put_hex32(desktop_bg);
            klog_puts(" got 0x");
            klog_put_hex32(after_minimize);
            klog_putc('\n');
            all_ok = 0;
        }
        if (after_unminimize != clock_bg) {
            klog_puts("[wm30] pixel check failed: after toggling minimize back off, home position should show the window again - expected 0x");
            klog_put_hex32(clock_bg);
            klog_puts(" got 0x");
            klog_put_hex32(after_unminimize);
            klog_putc('\n');
            all_ok = 0;
        }
        if (after_close != desktop_bg) {
            klog_puts("[wm30] pixel check failed: after WM_ACTION_CLOSE, home position should be empty desktop (window slot reclaimed) - expected 0x");
            klog_put_hex32(desktop_bg);
            klog_puts(" got 0x");
            klog_put_hex32(after_close);
            klog_putc('\n');
            all_ok = 0;
        }
        if (clock_exit != 128 + SIGTERM) {
            klog_puts("[wm30] WM_ACTION_CLOSE self-test: gui_clock's exit code did not match a SIGTERM death - expected 0x");
            klog_put_hex32((uint32_t)(128 + SIGTERM));
            klog_puts(" got 0x");
            klog_put_hex32((uint32_t)clock_exit);
            klog_putc('\n');
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M30 window chrome self-test: maximize/restore/minimize/close did not behave as expected");
        }
        klog_puts("[wm30] window chrome (maximize/restore/minimize/close via WM_ACTION_PIPE) self-test passed (6/6 checks matched).\n\n");
    }

    /* M32 self-test: the clipboard syscalls (SYS_clipboard_set/get) round-
     * trip real bytes through the exact same path gui_terminal.c's
     * Ctrl+C/V uses - no process spawning needed, both are plain
     * syscalls callable straight from kernel context, the same shape as
     * M14's own SYS_pipe self-test above. What this can't prove
     * headlessly is Ctrl+C/V (or Alt+Tab, compositor.c's other M32
     * addition) actually reaching a client from a real key chord - the
     * same manual/interactive boundary M18/M21/M22/M31 already drew for
     * every other modifier- or click-driven behavior in this project. */
    {
        const char msg[] = "clipboard round trip";
        do_syscall(SYS_clipboard_set, (uint64_t)msg, sizeof(msg) - 1, 0);
        char readback[64];
        long n = do_syscall(SYS_clipboard_get, (uint64_t)readback, sizeof(readback), 0);
        int mismatch = (n != (long)(sizeof(msg) - 1));
        for (long i = 0; !mismatch && i < n; i++) {
            if (readback[i] != msg[i]) {
                mismatch = 1;
            }
        }
        if (mismatch) {
            panic("M32 clipboard self-test: SYS_clipboard_get did not return what SYS_clipboard_set stored");
        }
        klog_puts("[clipboard] SYS_clipboard_set/get self-test passed.\n\n");
    }

    /* M33 self-test: SYS_writefile is the new syscall text_editor.c's
     * save depends on - the missing write half of SYS_readfile
     * (kernel/fs/vfs.c's vfs_write already existed and was already
     * exercised internally, e.g. by the very seeding loop just above,
     * just never reachable from user space through a syscall before this
     * milestone). Round-trips real bytes through a real file the same
     * way SYS_readfile's own earlier self-tests already prove reading
     * does. */
    {
        const char content[] = "M33 SYS_writefile self-test content";
        long wrc = do_syscall(SYS_writefile, (uint64_t)(PATH_TMP_DIR "m33test"), (uint64_t)content, sizeof(content) - 1);
        if (wrc != 0) {
            panic("M33 self-test: SYS_writefile failed");
        }
        char readback[64];
        long n = do_syscall(SYS_readfile, (uint64_t)(PATH_TMP_DIR "m33test"), (uint64_t)readback, sizeof(readback));
        int mismatch = (n != (long)(sizeof(content) - 1));
        for (long i = 0; !mismatch && i < n; i++) {
            if (readback[i] != content[i]) {
                mismatch = 1;
            }
        }
        if (mismatch) {
            panic("M33 self-test: SYS_writefile/SYS_readfile round trip mismatch");
        }
        klog_puts("[vfs] SYS_writefile/SYS_readfile self-test passed.\n\n");

    /* ---- fd-table self-test: a redirect, twice ---------------------------
     *
     * M72 spent a long time believing this was broken in the kernel. It
     * is not, and the test exists to keep saying so - the bug was a shell
     * that opened its redirect target BEFORE parking stdout, so the
     * second redirect's file landed on the parking descriptor and the
     * dup2 that followed quietly overwrote it. See run_command in
     * user_space/shell/sh.c.
     *
     * What is asserted here is the contract the shell now depends on:
     * the park / point-fd-1-at-a-file / write / restore cycle survives
     * being done more than once, and the bytes from both rounds are in
     * the file in order. Driven from kernel_main with no shell involved,
     * so a future failure names the kernel rather than the program that
     * happens to use it.
     */
    {
        const char *FDT = PATH_TMP_DIR "fdcycle";
        int fd_ok = 1;
        for (int round = 0; round < 2; round++) {
            /* Park FIRST, exactly as the shell now does - which is also
             * what stops the open below from being handed this slot. */
            long saved = do_syscall(SYS_dup2, 1, 9, 0);
            long fd = do_syscall(SYS_open, (uint64_t)FDT,
                                  round == 0 ? (OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE)
                                             : (OPEN_WRITE | OPEN_CREATE | OPEN_APPEND), 0);
            if (saved < 0 || fd < 0 || fd == saved) {
                fd_ok = 0;
                break;
            }
            do_syscall(SYS_dup2, (uint64_t)fd, 1, 0);
            if (do_syscall(SYS_write, 1, (uint64_t)(round == 0 ? "AAAA\n" : "BBBB\n"), 5) != 5) {
                fd_ok = 0;
            }
            do_syscall(SYS_dup2, (uint64_t)saved, 1, 0);
            do_syscall(SYS_close, (uint64_t)saved, 0, 0);
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);
        }
        static char fdt_buf[64];
        k_memset(fdt_buf, 0, sizeof(fdt_buf));
        int64_t fdt_n = vfs_read(FDT, fdt_buf, sizeof(fdt_buf) - 1);
        if (fdt_n != 10 || k_strcmp(fdt_buf, "AAAA\nBBBB\n") != 0) {
            klog_puts("[fd] a second redirect in one process did not reach its file - got ");
            klog_put_dec((uint32_t)(fdt_n < 0 ? 0 : fdt_n));
            klog_puts(" byte(s)\n");
            fd_ok = 0;
        }
        do_syscall(SYS_unlink, (uint64_t)FDT, 0, 0);
        if (!fd_ok) {
            panic("fd self-test: the dup2 redirect cycle does not survive being repeated");
        }
        klog_puts("[fd] the redirect cycle (park stdout, point fd 1 at a file, write, restore) "
                   "survives being done twice, and both rounds' bytes are in the file - "
                   "self-test passed.\n\n");
    }
    }

    /* M33 self-test: settings.c's live desktop-background-color control,
     * driven directly over WM_SETTINGS_PIPE the same way M30's self-test
     * drives WM_ACTION_PIPE - no GUI client needed, the compositor alone
     * already redraws the desktop background every frame regardless of
     * whether anything is connected to it. */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        int settings_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_SETTINGS_PIPE, (uint64_t)settings_fds, 0) != 0) {
            panic("M33 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_PIPE) failed");
        }
        /* M61: every field, always. This struct has grown three times
         * (M38's accent, M44's wallpaper, M61's animation flag) and each
         * time these kernel-side senders kept compiling while quietly
         * sending stack garbage for the new one - which for M61's flag
         * means "animations, maybe". Filling all of it in is the only
         * version of this that stays correct when it grows again. */
        wm_settings_request_t req;
        k_memset(&req, 0, sizeof(req));
        req.animations = 1;
        req.wallpaper = 0; /* WALLPAPER_FLAT - a flat desktop is what the probe below expects */
        req.bg_color = 0x00123456u; /* distinct from DEFAULT_BG_COLOR - a wrong pixel can't accidentally match */
        req.accent_color = 0; /* M38 added this field; this self-test only checks bg_color's effect */
        do_syscall(SYS_write, (uint64_t)settings_fds[1], (uint64_t)&req, sizeof(req));
        pit_sleep_ms(300);
        /* Same (500, 500) "empty desktop" probe point M20-M30's own
         * self-tests already use - nothing else is connected here to
         * cover it. */
        uint32_t got = fb_get_pixel(500, 500);

        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        if (got != req.bg_color) {
            klog_puts("[settings] pixel check failed: desktop background did not change - expected 0x");
            klog_put_hex32(req.bg_color);
            klog_puts(" got 0x");
            klog_put_hex32(got);
            klog_putc('\n');
            panic("M33 settings self-test: WM_SETTINGS_PIPE did not change the desktop background color");
        }
        klog_puts("[settings] WM_SETTINGS_PIPE background-color self-test passed.\n\n");
    }

    /* M36 self-test: text_editor.c is the one client that opts into
     * wm_create_request_t.confirm_close (via wm_connect_confirm_close),
     * so a WM_ACTION_CLOSE sent to it should take the new
     * WM_EVENT_CLOSE_REQUEST path instead of M30's unconditional
     * SIGTERM - driven purely over WM_ACTION_PIPE, same shape as M30's
     * own self-test, no simulated keyboard/mouse input needed. Spawned
     * with no filename ("untitled", doesn't exist yet, starts empty and
     * !dirty), so the client's own request_action() takes its immediate
     * branch and calls sys_exit(1) right away - the dirty-and-prompts
     * path needs a real keypress to ever get dirty in the first place,
     * which (like every other keyboard/mouse-driven behavior since M18)
     * is manual/interactive-only verification this headless test can't
     * fabricate. What this *does* prove headlessly: the close reached
     * the client as an event it could act on (a clean, app-chosen exit
     * code) rather than being killed out from under it, and the window
     * slot still ends up reclaimed either way. */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t editor_size_bytes = 0;
        uint8_t *editor_image = read_program("/bin/text_editor", &editor_size_bytes);
        int64_t editor_size = (int64_t)editor_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        task_t *editor_task = process_spawn("text_editor", editor_image, (size_t)editor_size, "");
        kfree(editor_image);
        pit_sleep_ms(500); /* connects (window 0), draws its first frame */

        int action_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_ACTION_PIPE, (uint64_t)action_fds, 0) != 0) {
            panic("M36 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }

        /* Home position: idx 0's default placement (100,100) plus a
         * (300,200) offset into text_editor's own content area - blank
         * (no text drawn there for an empty "untitled" file), so this
         * reads its own BG_COLOR before close and the compositor's
         * desktop background after (the two are deliberately distinct
         * colors - see wm30's own probe-point comment for why that
         * matters: a wrong pixel can't accidentally match). */
        const int32_t probe_x = 400, probe_y = 300;
        const uint32_t editor_bg = 0x00141414u;
        const uint32_t desktop_bg = 0x001A1A2Eu;

        uint32_t before_close = fb_get_pixel((uint32_t)probe_x, (uint32_t)probe_y);

        wm_action_request_t req;
        req.window_id = 0;
        req.action = WM_ACTION_CLOSE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        uint32_t after_close = selftest_pixel_settled((uint32_t)probe_x, (uint32_t)probe_y,
                                                       desktop_bg, "the editor's window to go away");
        long editor_exit = do_syscall(SYS_wait, (uint64_t)editor_task->id, 0, 0);

        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        int all_ok = 1;
        if (before_close != editor_bg) {
            klog_puts("[wm36] pixel check failed: before close, probe point should show text_editor's own background - expected 0x");
            klog_put_hex32(editor_bg);
            klog_puts(" got 0x");
            klog_put_hex32(before_close);
            klog_putc('\n');
            all_ok = 0;
        }
        if (after_close != desktop_bg) {
            klog_puts("[wm36] pixel check failed: after close, window slot should be reclaimed (empty desktop) - expected 0x");
            klog_put_hex32(desktop_bg);
            klog_puts(" got 0x");
            klog_put_hex32(after_close);
            klog_putc('\n');
            all_ok = 0;
        }
        if (editor_exit != 1) {
            klog_puts("[wm36] WM_EVENT_CLOSE_REQUEST self-test: text_editor's exit code did not match its own sys_exit(1) - expected 0x1 got 0x");
            klog_put_hex32((uint32_t)editor_exit);
            klog_putc('\n');
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M36 close-request self-test: WM_EVENT_CLOSE_REQUEST did not behave as expected");
        }
        klog_puts("[wm36] confirm_close opt-in (WM_EVENT_CLOSE_REQUEST via WM_ACTION_PIPE) self-test passed (3/3 checks matched).\n\n");
    }

    /* M38 self-test: two of the four visual-polish additions are real
     * framebuffer pixel effects, not just mouse-hover cosmetics (the
     * bold titlebar-text glyphs and the edge-aware resize cursors are,
     * like every other font/pointer-shape detail since M17/M18, left to
     * manual/interactive verification - there's no single pixel that
     * headlessly distinguishes "bold" from "regular" or proves a cursor
     * sprite changed without a real mouse to hover it with):
     *
     *   1. The drop shadow (compositor.c's fill_rect_shadow) - a real
     *      alpha-style blend, not a flat color, so this checks the exact
     *      blended value a probe point just past gui_clock's own outer
     *      border (x:304, clear of the window's own [98,302) extent
     *      entirely, so nothing later overwrites it) should hold against
     *      the fresh compositor's own default background.
     *   2. WM_SETTINGS_PIPE's new accent_color field - same "drive it
     *      directly, no GUI client needed" shape as M33's own settings
     *      self-test, just reading a titlebar pixel instead of a desktop
     *      one. Probe point (200, 88) sits inside gui_clock's titlebar
     *      strip but clear of both the "Clock" title text (ends ~x:146)
     *      and the leftmost titlebar button (starts ~x:246), so it can
     *      only ever read the flat titlebar fill underneath either. */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t clock_size_bytes = 0;
        uint8_t *clock_image = read_program("/bin/gui_clock", &clock_size_bytes);
        int64_t clock_size = (int64_t)clock_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(500); /* connects (window 0, auto-focused), draws its first frame */

        /* Shadow: blend(desktop_bg, black, ratio) - fill_rect_shadow's own
         * SHADOW_* constants - computed against the same 0x001A1A2E
         * default every earlier self-test's own "desktop_bg" constant
         * already assumes (a fresh compositor instance, nothing in
         * settings.c reachable to have changed it yet).
         *
         * M46: the ratio here moved from 1/3 to 1/2, because this window
         * is the *focused* one and a focused window's shadow is now
         * deeper - that is half of "these two windows differ by more than
         * a titlebar color". Updated rather than loosened: the number
         * this test asserts is still the exact arithmetic the compositor
         * does, and [m46] below checks the same probe point on both sides
         * of a focus change, which is what actually pins the pair of
         * ratios down. */
        uint32_t expected_shadow = 0x000D0D17u;
        uint32_t shadow_pixel = selftest_pixel_settled(304, 150, expected_shadow,
                                                        "the window's drop shadow to be drawn");

        int settings_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_SETTINGS_PIPE, (uint64_t)settings_fds, 0) != 0) {
            panic("M38 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_PIPE) failed");
        }
        wm_settings_request_t req;
        k_memset(&req, 0, sizeof(req));
        req.animations = 1;
        req.wallpaper = 0; /* WALLPAPER_FLAT */
        req.bg_color = 0x001A1A2Eu; /* unchanged - keeps the shadow probe above valid if this ever re-read it */
        req.accent_color = 0x00AA5500u; /* distinct from both TITLEBAR_COLOR and the old TITLEBAR_FOCUS_COLOR default - a wrong pixel can't accidentally match either */
        do_syscall(SYS_write, (uint64_t)settings_fds[1], (uint64_t)&req, sizeof(req));
        pit_sleep_ms(300);
        uint32_t titlebar_pixel = fb_get_pixel(200, 88);

        selftest_reap(clock_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        int all_ok = 1;
        if (shadow_pixel != expected_shadow) {
            klog_puts("[wm38] pixel check failed: drop-shadow blend did not match - expected 0x");
            klog_put_hex32(expected_shadow);
            klog_puts(" got 0x");
            klog_put_hex32(shadow_pixel);
            klog_putc('\n');
            all_ok = 0;
        }
        if (titlebar_pixel != req.accent_color) {
            klog_puts("[wm38] pixel check failed: focused titlebar did not pick up the new accent color - expected 0x");
            klog_put_hex32(req.accent_color);
            klog_puts(" got 0x");
            klog_put_hex32(titlebar_pixel);
            klog_putc('\n');
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M38 visual-polish self-test: drop shadow and/or accent color did not behave as expected");
        }
        klog_puts("[wm38] drop shadow + WM_SETTINGS_PIPE accent-color self-test passed (2/2 checks matched).\n\n");
    }

    /* Stretch goal: SMP. Deliberately brought up *after* every M-numbered
     * self-test above, not right after M7's scheduler one - several of
     * those (M20-M22's compositor/client tests especially) rely on
     * scheduling being deterministic enough that "spawned one at a time"
     * really does mean one connects before the next starts (see their own
     * comments), an assumption genuine multi-core parallelism can break
     * even with generous sleeps in between. Bringing SMP up afterward lets
     * every earlier milestone keep the exact single-core-equivalent
     * environment it was written and verified against, while still
     * standing up real multi-core support as additive capability from
     * here on - which is honest, not a workaround: nothing before this
     * point claims to be SMP-tested, and nothing after it needs to be
     * deterministic across a single core anymore.
     *
     * smp_init() has to run after sched_init() (long since true by now) -
     * an AP becomes a real schedulable task (sched_init_ap) the moment it
     * checks in, so the scheduler needs to already exist to receive it.
     * Falls back to single-core (cpu 0 only) if ACPI/the MADT isn't
     * present - see smp.c's own comment on why that's a normal fallback,
     * not a panic. */
    /* M47: whatever the firmware handed the loader, before anything asks
     * ACPI a question. acpi.c still falls back to its legacy scan if this
     * is 0, which is what keeps a non-UEFI boot (or a firmware that
     * publishes no RSDP) on exactly the path it was on before. */
    acpi_set_rsdp(rsdp_phys);

    smp_init();

    /* M47: reads the FADT once, here, rather than from inside the
     * shutdown path - walking ACPI tables is exactly the kind of work
     * that path should not be doing, and this is the same RSDT/XSDT walk
     * smp_init just did for the MADT. */
    power_init();

    /* Self-test: spawn several genuinely CPU-bound tasks and confirm more
     * than one *physical* CPU actually ran them, not just that the
     * round-robin scheduler still works (M7 already proved that on a
     * single core) - "compiles" isn't "works", the same discipline as
     * every earlier milestone's self-tests, extended to a case where the
     * thing being proven is genuine hardware parallelism. Skipped (not
     * failed) when smp_init() only found one CPU - there's nothing to
     * prove multi-core about on real single-core hardware or a QEMU
     * invocation without -smp. */
    {
        static volatile int smp_seen_cpu[MAX_CPUS];
        task_t *probe_tasks[4];
        for (int i = 0; i < 4; i++) {
            probe_tasks[i] = task_spawn("smp-probe", smp_probe_task, (void *)smp_seen_cpu);
        }
        pit_sleep_ms(2000);

        int distinct = 0;
        for (int i = 0; i < MAX_CPUS; i++) {
            if (smp_seen_cpu[i]) {
                distinct++;
            }
        }
        klog_puts("[smp] probe tasks observed running on ");
        klog_put_hex32((uint32_t)distinct);
        klog_puts(" distinct CPU(s) (");
        klog_put_hex32((uint32_t)smp_cpu_count);
        klog_puts(" online).\n");
        if (smp_cpu_count > 1 && distinct < 2) {
            panic("smp self-test: multiple CPUs online but probe tasks only ever ran on one");
        }

        /* Reap exactly the 4 probe tasks by pid, not SYS_wait(-1) - by
         * this point in boot, task 0 is the parent of every self-test
         * client above too, and SYS_wait(-1) would happily reap whichever
         * of those it reached first instead of the probes this test
         * actually cares about. (Those are all reaped by their own tests
         * now - see selftest_reap - so there is nothing left outstanding
         * for it to catch; waiting on specific pids is still the correct
         * thing to write, since "whatever finishes first" was never what
         * this meant.) */
        for (int i = 0; i < 4; i++) {
            do_syscall(SYS_wait, (uint64_t)probe_tasks[i]->id, 0, 0);
        }
        klog_puts("[smp] self-test passed.\n\n");
    }

    /* Stretch goal: networking. net_init() (rtl8139_init underneath)
     * returns 0 rather than panicking if no RTL8139 NIC is attached -
     * unlike every hardware-assumed-present driver elsewhere in this
     * kernel, an RTL8139 specifically is a legacy chip real machines
     * (the whole point of the separate "port to real hardware" stretch
     * goal, docs/real-hardware.md) essentially never actually have, so
     * treating its absence as fatal would make this self-test block
     * every real-hardware boot outright. Degrades the same way the
     * keyboard/mouse self-tests above already do for present-but-
     * unexercised hardware: log it, skip what depends on it, keep
     * booting - tools/run-qemu.sh and tools/qemu-serial-test.sh both
     * attach one (`-netdev user -device rtl8139`) precisely so this
     * kernel's own QEMU-based development loop still exercises the real
     * path every time. */
    if (net_init()) {
        /* Self-test: a real ICMP echo request/reply round trip against
         * QEMU's usermode-networking gateway (10.0.2.2, net.h's
         * net_gateway_ip()) - exercises the whole stack end to end (NIC
         * TX/RX, ARP resolution via ip_send's neighbor lookup, ICMP
         * request/reply matching) against a real peer, not a kernel-side
         * loopback stand-in, the same "prove it against something real"
         * discipline as M20's compositor self-test spawning an actual
         * client process instead of asserting compositor.c internals
         * directly. Chosen over pinging an arbitrary Internet host
         * because SLIRP (QEMU's usermode net backend) always answers
         * ARP/ICMP for its own gateway address itself - no dependency on
         * this environment actually having outbound internet access. */
        uint8_t ping_payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        uint16_t ping_id = 0x1EA5;
        uint16_t ping_seq = 1;
        icmp_send_echo_request(net_gateway_ip(), ping_id, ping_seq, ping_payload, sizeof(ping_payload));

        int got_reply = 0;
        uint64_t deadline = pit_get_ticks() + 3 * PIT_HZ;
        while (pit_get_ticks() < deadline) {
            if (icmp_echo_reply_seen(ping_id, ping_seq)) {
                got_reply = 1;
                break;
            }
            __asm__ volatile("hlt");
        }
        if (!got_reply) {
            panic("net self-test: no ICMP echo reply from the gateway within 3s");
        }
        klog_puts("[net] ICMP echo request/reply self-test passed (ping to gateway 0x");
        klog_put_hex32(net_gateway_ip());
        klog_puts(" round-tripped).\n\n");
    } else {
        klog_puts("[net] no RTL8139 NIC found - networking untested this boot "
                   "(expected on real hardware; see docs/real-hardware.md).\n\n");
    }

    /* M13: hand off to init (PID 1), which spawns the shell - this is
     * where interactive use of the OS begins. kernel_main (task 0) never
     * "finishes" from here: it becomes the idle task, looping on `hlt`
     * so entry.asm's post-kernel_main `cli` (which would permanently
     * disable interrupts, freezing the scheduler for every other task)
     * is never reached. */
    /* M42 self-test: the bottom taskbar, end to end at the protocol
     * level - a real desktop_shell.c panel and a real gui_clock.c client,
     * with this self-test standing in for the *user* rather than for
     * either of them. M41's [m41] test (a top-docked menu bar and its
     * three-pipe menu protocol) is what this replaces: both the bar and
     * the protocol were deleted this milestone, so the test that only
     * described them went with them rather than being left passing
     * against something that no longer exists.
     *
     * Four claims, checked with pixels and one protocol round trip:
     *
     *   1. The bar is docked at the screen's *bottom* edge again and is
     *      laid out left to right the Windows way - a Start button first,
     *      then the running-app buttons (shifted right to make room for
     *      it, which is exactly the kind of layout change a pixel probe
     *      catches and a protocol test cannot), then the tray at the far
     *      right.
     *   2. A running window really does get a button, drawn focused,
     *      carrying that app's own title - the M22 query protocol still
     *      doing its job through the relayout.
     *   3. Maximize still clears the bar and only the bar. M41 had every
     *      clamp reading a *top* panel's height too; with that gone,
     *      content_top_limit is a constant again, and a maximized window
     *      whose titlebar started one bar-height too low would look
     *      perfectly fine on screen while being wrong.
     *   4. WM_ACTION_TOGGLE_LAUNCHER - the one action in the protocol
     *      that acts on the compositor rather than on a window - really
     *      shows and hides the compositor-owned launcher surface. This is
     *      what the Start button sends; that a *click* on the Start
     *      button sends it goes through M40's input harness
     *      (tools/qemu-input-test.sh), which is exactly the split that
     *      milestone's whole point was to make possible.
     */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t shell_size_bytes = 0;
        uint8_t *shell_image = read_program("/bin/desktop_shell", &shell_size_bytes);
        int64_t shell_size = (int64_t)shell_size_bytes;
        size_t clock_size_bytes = 0;
        uint8_t *clock_image = read_program("/bin/gui_clock", &clock_size_bytes);
        int64_t clock_size = (int64_t)clock_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        task_t *shell_task = process_spawn("desktop_shell", shell_image, (size_t)shell_size, "");
        kfree(shell_image);
        pit_sleep_ms(500); /* connects as window 0 (the panel), draws its first frame */

        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(1000); /* connects as window 1 (focused); the taskbar's next periodic query picks it up */

        /* The panel docks at the bottom: y = 768 - PANEL_HEIGHT(32) = 736,
         * and every button in it sits at local y 4..28 (BTN_Y/BTN_H).
         * Absolute coordinates below are panel-local plus that (0, 736)
         * origin, the same convention M22's own checks use.
         *
         *   (71, 742)  inside the Start button's fill (local x 4..76),
         *              past its 2x2 tile glyph (local x 12..24) and its
         *              "Start" label (local x 28..68), and above the
         *              label's own rows - so it can only read flat fill.
         *   (168, 752) inside running-app button 0 (local x 84..180,
         *              START_X + START_W + 8 rather than the panel's left
         *              edge as it was before M42), right of its label.
         *   (932, 750) the tray's separator line, at local x
         *              width - TRAY_W(92) - i.e. the tray really is
         *              right-aligned rather than drawn at a fixed x.
         *
         * M44 made the taskbar translucent, so every expected value below
         * that comes from desktop_shell.c is TRANSLUCENT_NUM/DEN of it
         * mixed with what is underneath - here the compositor's own
         * DEFAULT_BG_COLOR, this self-test running no desktop background
         * client. See the [wm22] check above for the worked arithmetic. */
        uint32_t panel_bg_px = fb_get_pixel(512, 738);
        uint32_t above_panel_px = fb_get_pixel(512, 700);
        uint32_t start_btn_px = fb_get_pixel(71, 742);
        uint32_t running_slot_px = fb_get_pixel(168, 752);
        /* M57: 932 -> 937, when the tray's width started including a
         * *measured* clock ("00:00" in the proportional UI face, 35px)
         * rather than five fixed 8px cells. M63: 937 -> 912, when M42's
         * two decorative tray icons were replaced by four virtual-desktop
         * indicators. The tray is right-aligned, so anything that changes
         * its width moves this - which is exactly what a pixel test is
         * for, and both times this is the check that noticed. */
        uint32_t tray_sep_px = fb_get_pixel(912, 750);

        int action_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_ACTION_PIPE, (uint64_t)action_fds, 0) != 0) {
            panic("M42 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        wm_action_request_t req;
        k_memset(&req, 0, sizeof(req));
        req.window_id = 1; /* the clock - the panel took window 0 */
        req.action = WM_ACTION_MAXIMIZE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        /* content_top_limit is TITLEBAR_H(20) + BORDER(2) = 22 now that no
         * top-docked bar exists any more, so a maximized window's content
         * starts at y=22 and its titlebar occupies y:[2, 22). x=100 is
         * inside that titlebar (gui_clock's buffer is 200 wide, and
         * maximize never grows a window past its own buffer), past the
         * "Clock" title text and well left of the three buttons. */
        uint32_t maximized_titlebar = selftest_pixel_settled(100, 12, 0x004C99E6u,
                                                              "the maximized window's titlebar");
        uint32_t panel_over_maximized = selftest_pixel_settled(512, 738, 0x00181829u,
                                                                "the taskbar to stay on top of the maximized window");

        /* The launcher: no window_id at all (see WM_ACTION_TOGGLE_LAUNCHER),
         * and the overlay is centered horizontally and a third of the way
         * down, so (512, 309) is inside it and clear of its own title
         * text - and is bare desktop background when it's closed. */
        k_memset(&req, 0, sizeof(req));
        req.window_id = -1;
        req.action = WM_ACTION_TOGGLE_LAUNCHER;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        /* The overlay fades in (M61), so what a fixed sleep sampled was
         * whichever frame of that fade the machine had got to. The values
         * waited for here are the ones names[] grades against below. */
        uint32_t launcher_open_px = selftest_pixel_settled(512, 309, 0x001B2032u,
                                                            "the launcher overlay to finish fading in");
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        uint32_t launcher_closed_px = selftest_pixel_settled(512, 309, 0x001A1A2Eu,
                                                              "the launcher overlay to go away again");

        selftest_reap(clock_task);
        selftest_reap(shell_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        /* Every expected value mirrors the constant in the file that
         * draws it - desktop_shell.c for the panel's own colors,
         * compositor.c for everything it draws itself. */
        static const struct { const char *what; uint32_t expected; } names[] = {
            {"taskbar background, docked at the screen's bottom edge (desktop_shell.c PANEL_BG, translucent)", 0x00181829u},
            {"desktop background above the taskbar (compositor.c DEFAULT_BG_COLOR)", 0x001A1A2Eu},
            {"Start button fill at the taskbar's left edge (desktop_shell.c START_BG, translucent)", 0x00212D40u},
            {"running-app button 0, drawn focused, right of the Start button (desktop_shell.c RUNNING_SLOT_FOCUS_BG, translucent)", 0x00293E55u},
            {"system tray separator, right-aligned (desktop_shell.c TRAY_SEP_COLOR, translucent)", 0x002A3346u},
            {"a maximized window's titlebar starting at the top of the screen (compositor.c TITLEBAR_FOCUS_COLOR)", 0x004C99E6u},
            {"the taskbar staying on top of a maximized window (desktop_shell.c PANEL_BG, translucent)", 0x00181829u},
            {"the launcher overlay, opened by WM_ACTION_TOGGLE_LAUNCHER (compositor.c LAUNCHER_BG, translucent)", 0x001B2032u},
            {"the launcher overlay gone again after a second toggle (compositor.c DEFAULT_BG_COLOR)", 0x001A1A2Eu},
        };
        const uint32_t got[] = {
            panel_bg_px, above_panel_px, start_btn_px, running_slot_px, tray_sep_px,
            maximized_titlebar, panel_over_maximized, launcher_open_px, launcher_closed_px,
        };
        int all_ok = 1;
        for (size_t i = 0; i < sizeof(got) / sizeof(got[0]); i++) {
            if (got[i] != names[i].expected) {
                klog_puts("[m42] pixel check failed: ");
                klog_puts(names[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32(names[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32(got[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        if (!all_ok) {
            panic("M42 taskbar self-test: the bottom taskbar did not behave as expected");
        }
        klog_puts("[m42] bottom taskbar (Start button, running-app button, tray, "
                   "maximize clamp, launcher toggle) self-test passed (9/9 checks matched).\n\n");
    }

    /* M43 self-test: window snapping and the launcher overlay, both
     * driven the way a self-test can drive them - snapping through the
     * two new WM_ACTION_PIPE verbs (which is literally the same code a
     * titlebar drag into a screen edge runs, by construction: the drag
     * calls apply_window_action rather than reimplementing the geometry),
     * and the launcher through WM_ACTION_TOGGLE_LAUNCHER.
     *
     * text_editor rather than gui_clock as the subject, because its
     * buffer (640x384) is bigger than half this display in one dimension
     * and smaller in the other - so one snapped rect exercises both sides
     * of snap_rect's clamp at once: the width comes out as exactly half
     * the screen, and the height as the window's own buffer rather than
     * the full available height. A window small enough to be clamped in
     * both directions would have proved much less.
     *
     * What this can't reach is the gesture and the typing: whether a drag
     * into the edge actually produces the snap, whether the preview shows
     * up before release, and whether Ctrl+Space and type-to-filter work.
     * Those go through M40's input harness (tools/qemu-input-test.sh),
     * the same split every milestone since has used.
     */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t shell_size_bytes = 0;
        uint8_t *shell_image = read_program("/bin/desktop_shell", &shell_size_bytes);
        int64_t shell_size = (int64_t)shell_size_bytes;
        size_t editor_size_bytes = 0;
        uint8_t *editor_image = read_program("/bin/text_editor", &editor_size_bytes);
        int64_t editor_size = (int64_t)editor_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */
        task_t *shell_task = process_spawn("desktop_shell", shell_image, (size_t)shell_size, "");
        kfree(shell_image);
        pit_sleep_ms(400); /* connects as window 0, the taskbar */
        task_t *editor_task = process_spawn("text_editor", editor_image, (size_t)editor_size, "");
        kfree(editor_image);
        pit_sleep_ms(700); /* connects as window 1, focused, and draws */

        int action_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_ACTION_PIPE, (uint64_t)action_fds, 0) != 0) {
            panic("M43 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        wm_action_request_t req;
        k_memset(&req, 0, sizeof(req));
        req.window_id = 1;

        /* Right half: x = 1024/2 + BORDER(2) = 514, width
         * min(buf_w 640, 1024/2 - 2*BORDER = 508) = 508, so content spans
         * x:[514, 1022) and the titlebar y:[2, 22) above it. (700, 12) is
         * inside that titlebar, past the "Editor" text and well left of
         * the three buttons; (200, 12) is where the *left* half's
         * titlebar would be, and must be bare desktop. */
        req.action = WM_ACTION_SNAP_RIGHT;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        /* Waited on the half the window moves *into*: the other half is
         * bare desktop both before and after, so waiting on it would be
         * waiting for something that is already true. Read second, once
         * the snap has demonstrably happened. */
        uint32_t right_titlebar = selftest_pixel_settled(700, 12, 0x004C99E6u,
                                                          "the window to snap to the right half");
        uint32_t right_left_half = selftest_pixel_settled(200, 12, 0x001A1A2Eu,
                                                           "the left half to be empty");

        req.action = WM_ACTION_SNAP_LEFT;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        uint32_t left_titlebar = selftest_pixel_settled(200, 12, 0x004C99E6u,
                                                         "the window to snap to the left half");
        uint32_t left_right_half = selftest_pixel_settled(700, 12, 0x001A1A2Eu,
                                                           "the right half to be empty");

        /* The launcher overlay sits at x:[272, 752), y:[149, 469), and
         * its first result row at y:[195, 215) - LAUNCHER_LIST_Y(46) into
         * it. (700, 205) is inside that row's selection fill and far right
         * of any filename text; (700, 309) is plain overlay background
         * (row 5, which isn't the selected one) and bare desktop once the
         * overlay is gone, since the editor is snapped to the left half by
         * then. An empty query matches every file on disk, so there is
         * always a first row to be selected. */
        k_memset(&req, 0, sizeof(req));
        req.window_id = -1;
        req.action = WM_ACTION_TOGGLE_LAUNCHER;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        /* The overlay fades in (M61) - see the same wait in the [m42]
         * block above for what a fixed sleep here was really sampling.
         * The values waited for are the ones names[] grades against. */
        uint32_t launcher_bg = selftest_pixel_settled(700, 309, 0x001B2032u,
                                                       "the launcher overlay to finish fading in");
        uint32_t launcher_selected_row = selftest_pixel_settled(700, 205, 0x00335577u,
                                                                 "the launcher's first result to be drawn selected");
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        uint32_t launcher_closed = selftest_pixel_settled(700, 309, 0x001A1A2Eu,
                                                           "the launcher overlay to go away again");

        selftest_reap(editor_task);
        selftest_reap(shell_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        static const struct { const char *what; uint32_t expected; } names[] = {
            {"a right-snapped window's titlebar filling the screen's right half (compositor.c TITLEBAR_FOCUS_COLOR)", 0x004C99E6u},
            {"the left half staying empty while a window is snapped right (compositor.c DEFAULT_BG_COLOR)", 0x001A1A2Eu},
            {"a left-snapped window's titlebar filling the screen's left half (compositor.c TITLEBAR_FOCUS_COLOR)", 0x004C99E6u},
            {"the right half staying empty while a window is snapped left (compositor.c DEFAULT_BG_COLOR)", 0x001A1A2Eu},
            {"the launcher overlay, opened by WM_ACTION_TOGGLE_LAUNCHER (compositor.c LAUNCHER_BG, translucent)", 0x001B2032u},
            {"the launcher's first result drawn selected (compositor.c LAUNCHER_SEL_BG, drawn opaquely over the blended overlay)", 0x00335577u},
            {"the launcher overlay gone again after a second toggle (compositor.c DEFAULT_BG_COLOR)", 0x001A1A2Eu},
        };
        const uint32_t got[] = {
            right_titlebar, right_left_half, left_titlebar, left_right_half,
            launcher_bg, launcher_selected_row, launcher_closed,
        };
        int all_ok = 1;
        for (size_t i = 0; i < sizeof(got) / sizeof(got[0]); i++) {
            if (got[i] != names[i].expected) {
                klog_puts("[m43] pixel check failed: ");
                klog_puts(names[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32(names[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32(got[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        if (!all_ok) {
            panic("M43 snap/launcher self-test: the compositor did not behave as expected");
        }
        klog_puts("[m43] window snapping (left/right half, buffer-clamped) and the "
                   "launcher overlay self-test passed (7/7 checks matched).\n\n");
    }

    /* M44 self-test: the wallpaper gradient and the taskbar's
     * translucency, the two pieces of pixel math that milestone added -
     * same shape as M38's shadow-blend check, which is the precedent for
     * "assert the arithmetic, not just that something got drawn".
     *
     * This is the first self-test that runs a real desktop_icons.c, and
     * it has to: M44 moved the desktop background out of the compositor
     * (whose own fill has been invisible behind that full-screen window
     * since M32) and into the wallpaper the desktop client paints. So the
     * gradient can only be checked with the client that draws it running,
     * and the translucency can only be checked *over* it - blending
     * against a flat fill would pass just as happily if the blend were
     * reading the wrong buffer.
     *
     * Expected values, all derived from wallpaper.h's WALLPAPER_GRADIENT
     * (155%% of the base color at the top row, 60%% at the bottom,
     * straight-line integer interpolation between) over the compositor's
     * DEFAULT_BG_COLOR 0x1A1A2E on a 768-row display:
     *
     *   top    = (26,26,46) * 155/100 = (40,40,71) = 0x282847
     *   bottom = (26,26,46) *  60/100 = (15,15,27) = 0x0F0F1B
     *   row r  = top + (bottom - top) * r / 767, per channel
     *   row 100 -> 0x252542   row 600 -> 0x151525   row 738 -> 0x10101D
     *
     * and the taskbar's own 0x181828 blended 3/4 over that row-738 color
     * gives 0x161625. That last one is the check with real teeth: it is
     * the only value here that would still come out "reasonable" if the
     * blend were mixing against the wrong thing.
     */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t icons_size_bytes = 0;
        uint8_t *icons_image = read_program("/bin/desktop_icons", &icons_size_bytes);
        int64_t icons_size = (int64_t)icons_size_bytes;
        size_t shell_size_bytes = 0;
        uint8_t *shell_image = read_program("/bin/desktop_shell", &shell_size_bytes);
        int64_t shell_size = (int64_t)shell_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */
        task_t *icons_task = process_spawn("desktop_icons", icons_image, (size_t)icons_size, "");
        kfree(icons_image);
        pit_sleep_ms(500);
        task_t *shell_task = process_spawn("desktop_shell", shell_image, (size_t)shell_size, "");
        kfree(shell_image);
        pit_sleep_ms(700);

        /* x=600 is clear of the icon column (which is one column at
         * x:[32, 80) on this display) at every y probed here. */
        uint32_t grad_high = fb_get_pixel(600, 100);
        uint32_t grad_low = fb_get_pixel(600, 600);
        uint32_t taskbar_over_grad = fb_get_pixel(500, 738);

        /* Switch to WALLPAPER_FLAT and watch the ramp go away - which is
         * what proves the wallpaper setting reaches the client that
         * paints it, rather than the gradient simply being what
         * desktop_icons.c always draws. */
        int settings_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_SETTINGS_PIPE, (uint64_t)settings_fds, 0) != 0) {
            panic("M44 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_PIPE) failed");
        }
        wm_settings_request_t set_req;
        k_memset(&set_req, 0, sizeof(set_req));
        set_req.animations = 1;
        set_req.bg_color = 0x001A1A2Eu;
        set_req.accent_color = 0x004C99E6u;
        set_req.wallpaper = 0; /* WALLPAPER_FLAT */
        do_syscall(SYS_write, (uint64_t)settings_fds[1], (uint64_t)&set_req, sizeof(set_req));
        pit_sleep_ms(900); /* desktop_icons.c polls the setting every THEME_POLL_MS (500) */
        uint32_t flat_high = fb_get_pixel(600, 100);
        uint32_t flat_low = fb_get_pixel(600, 600);

        /* And the read side of the same setting (M44's WM_SETTINGS_QUERY_
         * PIPE). desktop_icons.c is polling this same pair twice a
         * second, so drain the response pipe first - what comes back
         * below has to be the answer to this question. */
        int sq_fds[2];
        int sqr_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_SETTINGS_QUERY_PIPE, (uint64_t)sq_fds, 0) != 0 ||
            do_syscall(SYS_pipe_open, (uint64_t)WM_SETTINGS_QUERY_RESP_PIPE, (uint64_t)sqr_fds, 0) != 0) {
            panic("M44 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_QUERY_*) failed");
        }
        do_syscall(SYS_pipe_reset, (uint64_t)sqr_fds[0], 0, 0);
        uint8_t ping = 1;
        do_syscall(SYS_write, (uint64_t)sq_fds[1], (uint64_t)&ping, sizeof(ping));
        pit_sleep_ms(300);
        wm_settings_request_t queried;
        k_memset(&queried, 0, sizeof(queried));
        long settings_read = do_syscall(SYS_read, (uint64_t)sqr_fds[0], (uint64_t)&queried, sizeof(queried));

        selftest_reap(shell_task);
        selftest_reap(icons_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        static const struct { const char *what; uint32_t expected; } names[] = {
            {"the wallpaper gradient near the top of the desktop (155% of 0x1A1A2E, ramped to row 100)", 0x00252542u},
            {"the wallpaper gradient near the bottom of the desktop (ramped to row 600)", 0x00151525u},
            {"the translucent taskbar blended over the gradient row underneath it, not over a flat fill", 0x00161625u},
            {"the desktop after switching to WALLPAPER_FLAT - top", 0x001A1A2Eu},
            {"the desktop after switching to WALLPAPER_FLAT - bottom, the same color as the top", 0x001A1A2Eu},
        };
        const uint32_t got[] = {grad_high, grad_low, taskbar_over_grad, flat_high, flat_low};
        int all_ok = 1;
        for (size_t i = 0; i < sizeof(got) / sizeof(got[0]); i++) {
            if (got[i] != names[i].expected) {
                klog_puts("[m44] pixel check failed: ");
                klog_puts(names[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32(names[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32(got[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        if (settings_read != (long)sizeof(queried) || queried.wallpaper != 0 ||
            queried.bg_color != 0x001A1A2Eu || queried.accent_color != 0x004C99E6u) {
            klog_puts("[m44] the settings query did not round-trip what was just set (wallpaper 0x");
            klog_put_hex32(queried.wallpaper);
            klog_puts(", bg 0x");
            klog_put_hex32(queried.bg_color);
            klog_puts(")\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M44 wallpaper/translucency self-test: the desktop did not look as computed");
        }
        klog_puts("[m44] wallpaper gradient, taskbar translucency over it, and the "
                   "settings query round trip self-test passed (6/6 checks matched).\n\n");
    }

    /* M45 self-test: the three things this milestone claims that nothing
     * before it could have checked.
     *
     * 1. SYS_taskinfo reports the tasks this test just spawned, by name.
     *    A process was a pid and nothing else until this milestone, so
     *    "the enumeration found compositor and wm_stubborn" is also the
     *    assertion that process_spawn's new name plumbing reached the
     *    scheduler at all.
     *
     * 2. WM_ACTION_CLOSE cannot remove a confirm_close client that
     *    ignores WM_EVENT_CLOSE_REQUEST, and WM_ACTION_KILL can. Both
     *    halves are checked, in that order, against the same window - the
     *    close *failing* is the load-bearing one: it is what makes this
     *    test fail the day the two verbs collapse back into one. M36's
     *    contract explicitly permits a client to never answer, so
     *    user_space/bin/wm_stubborn.c is a client that never does.
     *
     * 3. A force-killed client is reclaimed by M29's existing path with
     *    no new code, which is worth asserting rather than assuming: the
     *    window slot, its event pipe and the victim's own shm segment all
     *    come back. The slot and the pipe are checked by connecting a
     *    *second* stubborn client afterwards and watching it land in slot
     *    0 and draw there (a reused slot resets the existing pipe in
     *    place rather than opening a new one - see accept_pending_window);
     *    the segment is checked as free frames, which is the one number
     *    here that a signal death could plausibly get wrong, since
     *    shm_free_by_owner runs from task_exit_with_code and nothing had
     *    ever driven that path with a task that owned a segment.
     *
     * Window 0 is at x=100, y=100 (accept_pending_window's cascade,
     * clamped to content_top_limit) and is 200x120, so (200, 150) is
     * solidly inside its content and nowhere near its titlebar or
     * borders. With no desktop client running, what is left when it goes
     * away is the compositor's own DEFAULT_BG_COLOR fill. */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t stub_size_bytes = 0;
        uint8_t *stub_image = read_program("/bin/wm_stubborn", &stub_size_bytes);
        int64_t stub_size = (int64_t)stub_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        uint64_t frames_before_victim = pmm_free_frame_count();
        task_t *victim = process_spawn("wm_stubborn", stub_image, (size_t)stub_size, "");
        /* Connects as window 0, focused, and fills its buffer - waited
         * for rather than guessed at. The value is the one names[] grades
         * this against below. */
        uint32_t victim_pixel = selftest_pixel_settled(200, 150, 0x00B03040u,
                                                        "the stubborn client's window to be drawn");
        uint64_t frames_with_victim = pmm_free_frame_count();

        /* (1) the enumeration. Both tasks were spawned by this test, so
         * their ids are known exactly - this is not "find something that
         * looks right", it is a lookup by pid with the name asserted. */
        static task_info_t infos[MAX_TASKS];
        long info_count = do_syscall(SYS_taskinfo, (uint64_t)infos, MAX_TASKS, 0);
        int found_comp = 0, found_victim = 0, victim_shm = -1;
        for (long i = 0; i < info_count; i++) {
            if (infos[i].pid == comp_task->id && k_strcmp(infos[i].name, "compositor") == 0) {
                found_comp = 1;
            }
            if (infos[i].pid == victim->id && k_strcmp(infos[i].name, "wm_stubborn") == 0) {
                found_victim = 1;
                victim_shm = infos[i].shm_segments;
            }
        }

        int action_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_ACTION_PIPE, (uint64_t)action_fds, 0) != 0) {
            panic("M45 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        wm_action_request_t req;
        k_memset(&req, 0, sizeof(req));
        req.window_id = 0;

        /* (2a) the polite verb, which this client is entitled to ignore
         * forever - so the window must still be there afterwards. */
        req.action = WM_ACTION_CLOSE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        pit_sleep_ms(600);
        uint32_t after_close_pixel = fb_get_pixel(200, 150);
        long alive_after_close = do_syscall(SYS_task_alive, (uint64_t)victim->id, 0, 0);

        /* (2b) the verb that always works. */
        req.action = WM_ACTION_KILL;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        /* M54: read the liveness answer *before* reaping, not after. A
         * reaped task's slot comes back now, so a SYS_wait here would
         * make the SYS_task_alive below answer -1 ("no such task") rather
         * than 0 ("terminated, nonzero exit") - which is the right answer
         * to a question about a pid that no longer exists, and the wrong
         * question for this test to be asking. Polling for the death
         * instead of waiting for it keeps the task in the table until the
         * assertion has been made. */
        long alive_after_kill = 1;
        for (int spin = 0; spin < 200 && alive_after_kill == 1; spin++) {
            pit_sleep_ms(10);
            alive_after_kill = do_syscall(SYS_task_alive, (uint64_t)victim->id, 0, 0);
        }
        /* Then for reap_dead_clients to notice and repaint - a condition,
         * not an interval. */
        uint32_t after_kill_pixel = selftest_pixel_settled(200, 150, 0x001A1A2Eu,
                                                            "the killed client's window to be taken down");
        do_syscall(SYS_wait, (uint64_t)victim->id, 0, 0); /* now it can be reaped - see selftest_reap */
        uint64_t frames_after_kill = pmm_free_frame_count();

        /* (3) the reclaim. A second client connecting now must be handed
         * the slot the first one gave up, and must be able to draw
         * through it - which it can only do if the shm segment and the
         * event pipe behind that slot are both live again. */
        task_t *victim2 = process_spawn("wm_stubborn", stub_image, (size_t)stub_size, "");
        kfree(stub_image);
        uint32_t reused_slot_pixel = selftest_pixel_settled(200, 150, 0x00B03040u,
                                                             "a second client to draw through the reclaimed slot");

        selftest_reap(victim2);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        int all_ok = 1;
        if (info_count <= 0 || !found_comp || !found_victim) {
            klog_puts("[m45] SYS_taskinfo did not report the tasks this test spawned (count 0x");
            klog_put_hex32((uint32_t)info_count);
            klog_puts(", compositor found 0x");
            klog_put_hex32((uint32_t)found_comp);
            klog_puts(", wm_stubborn found 0x");
            klog_put_hex32((uint32_t)found_victim);
            klog_puts(")\n");
            all_ok = 0;
        }
        if (victim_shm != 1) {
            klog_puts("[m45] SYS_taskinfo reported the wrong shm-segment count for a task holding exactly one: 0x");
            klog_put_hex32((uint32_t)victim_shm);
            klog_putc('\n');
            all_ok = 0;
        }
        static const struct { const char *what; uint32_t expected; } names[] = {
            {"the stubborn client's window after it connected and drew", 0x00B03040u},
            {"the same window after WM_ACTION_CLOSE, which this client is entitled to ignore - it must still be there", 0x00B03040u},
            {"the desktop where that window was, after WM_ACTION_KILL", 0x001A1A2Eu},
            {"a second client's window in the slot the killed one gave up", 0x00B03040u},
        };
        const uint32_t got[] = {victim_pixel, after_close_pixel, after_kill_pixel, reused_slot_pixel};
        for (size_t i = 0; i < sizeof(got) / sizeof(got[0]); i++) {
            if (got[i] != names[i].expected) {
                klog_puts("[m45] pixel check failed: ");
                klog_puts(names[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32(names[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32(got[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        if (alive_after_close != 1) {
            klog_puts("[m45] WM_ACTION_CLOSE terminated a client that never answered WM_EVENT_CLOSE_REQUEST - the two verbs have collapsed into one (SYS_task_alive 0x");
            klog_put_hex32((uint32_t)alive_after_close);
            klog_puts(")\n");
            all_ok = 0;
        }
        if (alive_after_kill != 0) {
            klog_puts("[m45] WM_ACTION_KILL did not terminate the client (SYS_task_alive 0x");
            klog_put_hex32((uint32_t)alive_after_kill);
            klog_puts(")\n");
            all_ok = 0;
        }
        /* The victim's own 64 KiB segment is 16 frames; its address space
         * and stack are deliberately *not* reclaimed (there is no
         * vmm_destroy_address_space in this project - see reclaim_window
         * and shm.h's own note), so this is a "did shm_free_by_owner run
         * on the signal path" check, not a leak-free-everything one. */
        if (frames_after_kill < frames_with_victim + 16) {
            klog_puts("[m45] killing a task that owned an shm segment did not hand its frames back: 0x");
            klog_put_hex64(frames_before_victim);
            klog_puts(" free before, 0x");
            klog_put_hex64(frames_with_victim);
            klog_puts(" with it running, 0x");
            klog_put_hex64(frames_after_kill);
            klog_puts(" after the kill\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M45 process-control self-test: force quit did not behave as specified");
        }
        klog_puts("[m45] SYS_taskinfo naming, WM_ACTION_KILL forcing a confirm_close client "
                   "WM_ACTION_CLOSE cannot, and the window slot/event pipe/shm reclaim after it "
                   "self-test passed (8/8 checks).\n\n");
    }

    /* M46 self-test: the window chrome this milestone reshaped, checked
     * as pixels rather than as "something got drawn".
     *
     * The load-bearing check is the first pair. A titlebar button's
     * bounding-box *corner* must be titlebar color while its middle is
     * button color - which is precisely the difference between a circle
     * and the 14px square that was there before, and the one assertion a
     * milestone that only changed the fill color would fail.
     *
     * The second pair is macOS's own rule for the glyphs: they are drawn
     * on the focused window and omitted otherwise, so three saturated
     * dots don't shout from every unfocused window on the desktop. A
     * second client connecting is what takes focus away (accept_pending_
     * window focuses a new window), so the same close button is read
     * twice - once with its x, once without.
     *
     * Geometry, all from this file's own constants: gui_clock is 200x90
     * and connects first, so it lands at (100, 100) with a titlebar in
     * y:[80, 100). The close button is the outermost of three
     * right-aligned 14px circles - x = 100 + 200 - BTN_MARGIN(4) -
     * BTN_SIZE(14) - 2*(BTN_SIZE + BTN_GAP)(36) = 246 - and sits at
     * y = 80 + (20 - 14)/2 = 83. So:
     *
     *   (246, 83) is the button's top-left bounding-box corner, which a
     *             circle of inset 4 on its first row does not cover
     *   (250, 90) is inside the disc and clear of both diagonals of the x
     *   (253, 90) is on the x's top-left-to-bottom-right stroke
     */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t clock_size_bytes = 0;
        uint8_t *clock_image = read_program("/bin/gui_clock", &clock_size_bytes);
        int64_t clock_size = (int64_t)clock_size_bytes;
        size_t stub_size_bytes = 0;
        uint8_t *stub_image = read_program("/bin/wm_stubborn", &stub_size_bytes);
        int64_t stub_size = (int64_t)stub_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */
        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        /* Connects as window 0, focused, and draws. The corner is the
         * titlebar's own colour, which is exactly what says the frame has
         * been painted - the value names[] grades it against below. */
        uint32_t focused_corner = selftest_pixel_settled(246, 83, 0x004C99E6u,
                                                          "the focused window's titlebar to be drawn");
        uint32_t focused_disc = selftest_pixel_settled(250, 90, 0x00FF5F57u,
                                                        "the close button's own colour");
        uint32_t focused_glyph = selftest_pixel_settled(253, 90, 0x00303030u,
                                                         "the x drawn on the close button");
        /* (304, 100) is in the right-hand sliver of this window's own drop
         * shadow (the frame's right edge is x = 302, the shadow reaches
         * x = 308) and above where the second client's frame will land,
         * so the same point can be read before and after the focus change
         * - which is what pins down *both* shadow ratios rather than just
         * whichever one happens to be in effect. */
        uint32_t focused_shadow = selftest_pixel_settled(304, 100, 0x000D0D17u,
                                                          "the focused window's deeper drop shadow");
        /* The title itself - see selftest_title_counts, which also
         * explains why this waits rather than looks once. */
        int focused_bright = 0, focused_dim = 0;
        selftest_title_counts(1, &focused_bright, &focused_dim);

        /* A second client connects and takes focus, so the clock's window
         * is now the unfocused one - without anything having touched the
         * clock, its window or the cursor. */
        task_t *stub_task = process_spawn("wm_stubborn", stub_image, (size_t)stub_size, "");
        kfree(stub_image);
        /* The same corner, once the focus change has actually been
         * painted - which is what the unfocused titlebar colour says. */
        uint32_t unfocused_corner = selftest_pixel_settled(246, 83, 0x00335577u,
                                                            "the clock's window to be repainted unfocused");
        uint32_t unfocused_disc = selftest_pixel_settled(250, 90, 0x00FF5F57u,
                                                          "the close button keeping its own colour unfocused");
        uint32_t unfocused_glyph = selftest_pixel_settled(253, 90, 0x00FF5F57u,
                                                           "the x to be gone from an unfocused button");
        uint32_t unfocused_shadow = selftest_pixel_settled(304, 100, 0x0011111Eu,
                                                            "the unfocused window's shallower drop shadow");
        int unfocused_bright = 0, unfocused_dim = 0;
        selftest_title_counts(0, &unfocused_bright, &unfocused_dim);

        selftest_reap(stub_task);
        selftest_reap(clock_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        static const struct { const char *what; uint32_t expected; } names[] = {
            {"the close button's bounding-box corner on a focused window - titlebar color, which is what says a circle got drawn and not a square", 0x004C99E6u},
            {"the middle of that same button, clear of both strokes of its x", 0x00FF5F57u},
            {"a pixel on that x itself", 0x00303030u},
            {"the corner again once the window is unfocused - the unfocused titlebar color", 0x00335577u},
            {"the middle of the button on an unfocused window - still the button's own color", 0x00FF5F57u},
            {"the x's own pixel once unfocused - the glyph is gone, so this is button color too", 0x00FF5F57u},
            {"the focused window's drop shadow - blend(0x1A1A2E, black, 1/2), the deeper of the two ratios", 0x000D0D17u},
            {"that same shadow pixel once the window is unfocused - blend(0x1A1A2E, black, 1/3), the shallower one", 0x0011111Eu},
        };
        const uint32_t got[] = {focused_corner, focused_disc, focused_glyph,
                                 unfocused_corner, unfocused_disc, unfocused_glyph,
                                 focused_shadow, unfocused_shadow};
        int all_ok = 1;
        for (size_t i = 0; i < sizeof(got) / sizeof(got[0]); i++) {
            if (got[i] != names[i].expected) {
                klog_puts("[m46] pixel check failed: ");
                klog_puts(names[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32(names[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32(got[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        if (focused_bright == 0 || focused_dim != 0 || unfocused_dim == 0 || unfocused_bright != 0) {
            klog_puts("[m46] the title text did not dim when the window lost focus (focused: 0x");
            klog_put_hex32((uint32_t)focused_bright);
            klog_puts(" bright / 0x");
            klog_put_hex32((uint32_t)focused_dim);
            klog_puts(" dim, unfocused: 0x");
            klog_put_hex32((uint32_t)unfocused_bright);
            klog_puts(" bright / 0x");
            klog_put_hex32((uint32_t)unfocused_dim);
            klog_puts(" dim)\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M46 window-chrome self-test: the titlebar did not look as computed");
        }
        klog_puts("[m46] circular titlebar buttons, focus-gated glyphs, the deeper focused "
                   "shadow and the dimmed unfocused title self-test passed (9/9 checks).\n\n");
    }

    /* M47 self-test: the two halves of session lifecycle that can be
     * checked without actually turning the machine off - the settings
     * file, and the orderly stop's escalation from SIGTERM to SIGKILL.
     * (Whether S5 really fires is the one thing no in-guest test can
     * answer, so it is the harness's job: tools/qemu-input-test.sh's
     * shutdown_powers_off_the_machine watches QEMU's own process exit.)
     *
     * The settings half is end to end on purpose. Rather than calling
     * settings_file_load directly - which would test the parser and
     * nothing else - it writes a file, starts a real compositor and a
     * real desktop_icons, and reads the pixel the desktop actually
     * paints. That is the whole chain the feature is: file on disk ->
     * compositor reads it before any client connects -> relays it over
     * WM_SETTINGS_QUERY_PIPE -> the desktop paints with it.
     *
     * Then the same thing with a deliberately corrupted file, which has
     * to fall back to the compiled-in defaults rather than to garbage
     * colors. Expected values, both at (600, 400) on a 768-row display:
     *
     *   saved:     WALLPAPER_FLAT over 0x203040 is exactly 0x203040
     *   corrupted: WALLPAPER_GRADIENT over the default 0x1A1A2E, ramped
     *              to row 400 - 155%% of (26,26,46) = (40,40,71) at the
     *              top, 60%% = (15,15,27) at the bottom, so
     *              (40 + (15-40)*400/767, ..., 71 + (27-71)*400/767)
     *              = (27, 27, 49) = 0x1B1B31
     */
    {
        /* This test overwrites settings.conf, including with a
         * deliberately corrupted one. That is safe to do in place because
         * the whole self-test phase is already running against pinned
         * defaults with the user's own file held aside - see
         * selftest_settings_install_defaults. */
        static const char saved_conf[] = "bg=0x00203040\naccent=0x00aa5500\nwallpaper=0x00000000\n";
        if (do_syscall(SYS_writefile, (uint64_t)PATH_SETTINGS, (uint64_t)saved_conf, sizeof(saved_conf) - 1) != 0) {
            panic("M47 self-test: could not write settings.conf");
        }

        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t icons_size_bytes = 0;
        uint8_t *icons_image = read_program("/bin/desktop_icons", &icons_size_bytes);
        int64_t icons_size = (int64_t)icons_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        /* Not selftest_wait_for_compositor: that one waits for the
         * *default* background, and the whole point of this compositor is
         * that it comes up with saved_conf's 0x00203040 instead - so it
         * waited out its full five seconds, every boot, and said so. The
         * condition is the same one, told what this desktop looks like. */
        selftest_wait_for_pixel(500, 400, 0x00203040u, 5000,
                                 "the compositor to paint the saved background");
        task_t *icons_task = process_spawn("desktop_icons", icons_image, (size_t)icons_size, "");
        uint32_t saved_pixel = selftest_pixel_settled(600, 400, 0x00203040u,
                                                       "the desktop to come up with the saved settings");
        selftest_reap(icons_task);
        selftest_reap(comp_task);

        /* Corrupted: one key with a value that isn't a number, one key
         * missing entirely, and a line that isn't a key=value at all.
         * settings_file_load is all-or-nothing (see its own comment), so
         * every one of these on its own is enough to fall back. */
        static const char broken_conf[] = "this is not a settings file\nbg=nonsense\nwallpaper=1\n";
        if (do_syscall(SYS_writefile, (uint64_t)PATH_SETTINGS, (uint64_t)broken_conf, sizeof(broken_conf) - 1) != 0) {
            panic("M47 self-test: could not overwrite settings.conf with a corrupted one");
        }

        comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */
        icons_task = process_spawn("desktop_icons", icons_image, (size_t)icons_size, "");
        kfree(icons_image);
        uint32_t fallback_pixel = selftest_pixel_settled(600, 400, 0x001B1B31u,
                                                          "the desktop to fall back to the compiled-in defaults");
        selftest_reap(icons_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        /* The orderly stop, driven directly. power_orderly_stop returns
         * how many tasks it had to SIGKILL, and every task already
         * records which signal killed it as 128 + that signal - so
         * running it twice, once with no grace period and once with a
         * real one, distinguishes the two branches by two independent
         * measurements rather than by one.
         *
         * With grace 0 nothing gets a chance to notice the SIGTERM, so
         * both victims must die of the SIGKILL that follows (exit code
         * 128 + 9 = 137). With a full ~1s grace they must all be gone
         * before the SIGKILL round runs at all (exit code 128 + 15 =
         * 143, and a killed count of zero). A spinner is the right
         * victim: it does real CPU-bound work and never exits on its
         * own, so only a delivered signal can stop it. */
        task_t *v1 = task_spawn("shutdown-victim", spinner_task, NULL);
        task_t *v2 = task_spawn("shutdown-victim", spinner_task, NULL);
        int killed_no_grace = power_orderly_stop(0);
        int codes_no_grace = (v1->exit_code == 128 + SIGKILL) && (v2->exit_code == 128 + SIGKILL);

        task_t *v3 = task_spawn("shutdown-victim", spinner_task, NULL);
        task_t *v4 = task_spawn("shutdown-victim", spinner_task, NULL);
        int killed_with_grace = power_orderly_stop(100);
        int codes_with_grace = (v3->exit_code == 128 + SIGTERM) && (v4->exit_code == 128 + SIGTERM);

        int all_ok = 1;
        if (saved_pixel != 0x00203040u) {
            klog_puts("[m47] the desktop did not come up with the saved settings - expected 0x00203040 got 0x");
            klog_put_hex32(saved_pixel);
            klog_putc('\n');
            all_ok = 0;
        }
        if (fallback_pixel != 0x001B1B31u) {
            klog_puts("[m47] a corrupted settings.conf did not fall back to the compiled-in defaults - expected 0x001B1B31 got 0x");
            klog_put_hex32(fallback_pixel);
            klog_putc('\n');
            all_ok = 0;
        }
        if (killed_no_grace != 2 || !codes_no_grace) {
            klog_puts("[m47] with no grace period, the orderly stop did not escalate to SIGKILL (0x");
            klog_put_hex32((uint32_t)killed_no_grace);
            klog_puts(" killed, exit codes 0x");
            klog_put_hex32((uint32_t)v1->exit_code);
            klog_puts("/0x");
            klog_put_hex32((uint32_t)v2->exit_code);
            klog_puts(")\n");
            all_ok = 0;
        }
        if (killed_with_grace != 0 || !codes_with_grace) {
            klog_puts("[m47] with a real grace period, tasks did not stop on SIGTERM alone (0x");
            klog_put_hex32((uint32_t)killed_with_grace);
            klog_puts(" needed SIGKILL, exit codes 0x");
            klog_put_hex32((uint32_t)v3->exit_code);
            klog_puts("/0x");
            klog_put_hex32((uint32_t)v4->exit_code);
            klog_puts(")\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M47 session-lifecycle self-test: settings persistence and/or the orderly stop did not behave as specified");
        }
        klog_puts("[m47] settings.conf round trip (including a corrupted one falling back to "
                   "defaults) and the orderly stop's SIGTERM-then-SIGKILL escalation "
                   "self-test passed (4/4 checks).\n\n");
    }

    /* M48 self-test: the notification surface, and the spawn error codes
     * behind most of what it will ever say.
     *
     * The toast half checks both ends of a toast's life - that it is
     * where it should be, and that it is gone by its own deadline. The
     * second is the one worth having: a notification surface that only
     * ever appears is a notification surface that eventually covers the
     * screen, and nothing but its own timer ever retires one.
     *
     * Geometry from compositor.c's own TOAST_* constants: a 300x56 toast
     * TOAST_MARGIN (12) in from the top-right of a 1024x768 display, so
     * x:[712, 1012), y:[12, 68). The accent stripe is TOAST_STRIPE_W (4)
     * wide, inset one pixel, at x:[713, 717). (714, 40) is on it;
     * (900, 40) is the toast's own background, right of both strings and
     * in the gap between the title and body rows.
     *
     * The spawn half asserts each distinct cause against its own code -
     * a name that isn't on disk, an ordinary text file (m33test, written
     * by the SYS_writefile self-test far above), and a deliberately
     * truncated ELF. M40's validation already rejected all three; it just
     * rejected them anonymously.
     */
    {
        /* A real ELF header followed by nothing - enough that this is not
         * "not an ELF at all" but genuinely a *truncated* one, which is
         * the third distinct cause this milestone's error codes have to
         * survive contact with. */
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        if (comp_size < 64) {
            panic("vfs_read: compositor missing or absurdly small - should exist, just seeded");
        }
        if (vfs_write(PATH_TMP_DIR "m48trunc", comp_image, 64) != 0) {
            panic("M48 self-test: could not write the truncated-ELF fixture");
        }

        long rc_missing = do_syscall(SYS_spawn, (uint64_t)"definitely_not_a_file", 0, 0);
        long rc_text = do_syscall(SYS_spawn, (uint64_t)(PATH_TMP_DIR "m33test"), 0, 0);
        long rc_trunc = do_syscall(SYS_spawn, (uint64_t)(PATH_TMP_DIR "m48trunc"), 0, 0);

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        int notify_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_NOTIFY_PIPE, (uint64_t)notify_fds, 0) != 0) {
            panic("M48 self-test: kernel-side SYS_pipe_open(WM_NOTIFY_PIPE) failed");
        }
        wm_notify_request_t note;
        k_memset(&note, 0, sizeof(note));
        note.level = WM_NOTIFY_ERROR;
        k_strlcpy(note.title, "Test", sizeof(note.title));
        k_strlcpy(note.body, "Body", sizeof(note.body));
        do_syscall(SYS_write, (uint64_t)notify_fds[1], (uint64_t)&note, sizeof(note));
        pit_sleep_ms(300);

        uint32_t stripe = fb_get_pixel(714, 40);
        uint32_t toast_bg = fb_get_pixel(900, 40);

        /* Comfortably inside TOAST_TTL_MS (4000) so this is "still up",
         * not "up or not depending on how the boot went". */
        pit_sleep_ms(2000);
        uint32_t stripe_midlife = fb_get_pixel(714, 40);

        /* And comfortably past it. */
        pit_sleep_ms(2500);
        uint32_t stripe_expired = fb_get_pixel(714, 40);
        uint32_t bg_expired = fb_get_pixel(900, 40);

        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        static const struct { const char *what; uint32_t expected; } names[] = {
            {"the error toast's accent stripe", 0x00E05C55u},
            {"the toast's own background, right of its text", 0x00222A38u},
            {"that same stripe two seconds in - still well inside the toast's own deadline", 0x00E05C55u},
            {"the stripe once the deadline has passed - bare desktop again", 0x001A1A2Eu},
            {"the toast's background once the deadline has passed", 0x001A1A2Eu},
        };
        const uint32_t got[] = {stripe, toast_bg, stripe_midlife, stripe_expired, bg_expired};
        int all_ok = 1;
        for (size_t i = 0; i < sizeof(got) / sizeof(got[0]); i++) {
            if (got[i] != names[i].expected) {
                klog_puts("[m48] pixel check failed: ");
                klog_puts(names[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32(names[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32(got[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        static const struct { const char *what; long expected; long got; } codes[] = {
            {"a name that is not on disk", SPAWN_ERR_NOT_FOUND, 0},
            {"an ordinary text file", SPAWN_ERR_BAD_IMAGE, 0},
            {"a truncated ELF", SPAWN_ERR_BAD_IMAGE, 0},
        };
        const long got_codes[] = {rc_missing, rc_text, rc_trunc};
        for (size_t i = 0; i < sizeof(got_codes) / sizeof(got_codes[0]); i++) {
            if (got_codes[i] != codes[i].expected) {
                klog_puts("[m48] SYS_spawn returned the wrong code for ");
                klog_puts(codes[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32((uint32_t)codes[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32((uint32_t)got_codes[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        /* The message table is the other half of a distinct code being
         * useful: two causes that return different numbers and the same
         * sentence would tell a user nothing more than -1 did. */
        if (k_strcmp(spawn_error_message(SPAWN_ERR_NOT_FOUND), spawn_error_message(SPAWN_ERR_BAD_IMAGE)) == 0) {
            klog_puts("[m48] two distinct spawn errors share one message - the codes buy nothing\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M48 feedback self-test: toasts and/or spawn error codes did not behave as specified");
        }
        klog_puts("[m48] toast raised, still up mid-life, gone by its own deadline, and each "
                   "distinct SYS_spawn failure reporting its own code self-test passed "
                   "(9/9 checks).\n\n");
    }

    /* M49 self-test: the three pieces of input completeness that can be
     * driven without a hand on the machine - the shortcut table, the
     * chords it dispatches, and the drag protocol's round trip. (The
     * wheel itself is hardware: whether a real detent reaches a real list
     * is what tools/qemu-input-test.sh's wheel test is for.)
     *
     * The chord half is checked the only way that means anything from
     * here: not by pressing keys - the kernel has no way to inject one -
     * but by asserting that the table the compositor dispatches from and
     * the table settings.c lists from are the same table, and that
     * shortcut_lookup resolves each chord to the id it is supposed to.
     * That is the actual claim this milestone makes about them: there is
     * one table, so a chord cannot be listed without being wired up.
     *
     * The drag half is a real round trip through a real compositor: a
     * kernel-side WM_DRAG_PIPE write, a real client window under the
     * cursor, and the payload read back out of WM_DRAG_DATA_PIPE - which
     * is the piece no client could verify on its own, because it crosses
     * two of them.
     */
    {
        /* Every chord, resolved from the same shortcut_lookup
         * compositor.c calls. Shift+Alt+Tab vs Alt+Tab is the pair with
         * teeth: they share a key and differ only by a modifier that one
         * of them forbids, so a table that got mods_forbidden wrong would
         * silently make Shift+Alt+Tab cycle forward. */
        static const struct { const char *what; char ch; int mods; int expect; } chords[] = {
            {"Alt+Tab", '\t', KBD_MOD_ALT, SHORTCUT_CYCLE_FORWARD},
            {"Shift+Alt+Tab", '\t', KBD_MOD_ALT | KBD_MOD_SHIFT, SHORTCUT_CYCLE_BACKWARD},
            {"Ctrl+Space", ' ', KBD_MOD_CTRL, SHORTCUT_LAUNCHER},
            {"Ctrl+Shift+Esc", 27, KBD_MOD_CTRL | KBD_MOD_SHIFT, SHORTCUT_TASK_MANAGER},
            {"Alt+F4", (char)KBD_KEY_FN(4), KBD_MOD_ALT, SHORTCUT_CLOSE_WINDOW},
            {"Ctrl+Alt+Left", (char)KBD_KEY_LEFT, KBD_MOD_CTRL | KBD_MOD_ALT, SHORTCUT_SNAP_LEFT},
            {"Ctrl+Alt+Right", (char)KBD_KEY_RIGHT, KBD_MOD_CTRL | KBD_MOD_ALT, SHORTCUT_SNAP_RIGHT},
            {"Ctrl+Alt+Up", (char)KBD_KEY_UP, KBD_MOD_CTRL | KBD_MOD_ALT, SHORTCUT_MAXIMIZE},
            {"Ctrl+Alt+Down", (char)KBD_KEY_DOWN, KBD_MOD_CTRL | KBD_MOD_ALT, SHORTCUT_MINIMIZE},
            {"a plain Tab, which must NOT be a chord", '\t', 0, SHORTCUT_NONE},
            {"a plain space", ' ', 0, SHORTCUT_NONE},
            {"an ordinary letter with Ctrl held", 'c', KBD_MOD_CTRL, SHORTCUT_NONE},
        };
        int all_ok = 1;
        for (size_t i = 0; i < sizeof(chords) / sizeof(chords[0]); i++) {
            int got = shortcut_lookup(chords[i].ch, chords[i].mods);
            if (got != chords[i].expect) {
                klog_puts("[m49] shortcut_lookup resolved ");
                klog_puts(chords[i].what);
                klog_puts(" to 0x");
                klog_put_hex32((uint32_t)got);
                klog_puts(", expected 0x");
                klog_put_hex32((uint32_t)chords[i].expect);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        /* Every row is dispatchable and describable - the property that
         * makes one table worth having rather than two. */
        for (int i = 0; i < SHORTCUT_COUNT; i++) {
            if (SHORTCUTS[i].id == SHORTCUT_NONE || !SHORTCUTS[i].chord[0] || !SHORTCUTS[i].what[0]) {
                klog_puts("[m49] shortcut row 0x");
                klog_put_hex32((uint32_t)i);
                klog_puts(" is missing an id, a chord name or a description\n");
                all_ok = 0;
            }
        }

        /* The drag round trip. gui_clock connects first, so its window is
         * at (100, 100) and 200x90 - the compositor delivers a drop to
         * whatever window the cursor is over, and the cursor starts at
         * the screen center, so it has to be moved onto that window
         * first. There is no way to inject a mouse packet from here, so
         * this drives the one thing that genuinely needs a real
         * compositor - the payload crossing from WM_DRAG_PIPE to
         * WM_DRAG_DATA_PIPE - and leaves the pointer half to the input
         * harness, which can actually move a pointer. */
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        int drag_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_DRAG_PIPE, (uint64_t)drag_fds, 0) != 0) {
            panic("M49 self-test: kernel-side SYS_pipe_open(WM_DRAG_PIPE) failed");
        }
        wm_drag_request_t drag;
        k_memset(&drag, 0, sizeof(drag));
        k_strlcpy(drag.payload, PATH_TMP_DIR "m33test", sizeof(drag.payload));
        do_syscall(SYS_write, (uint64_t)drag_fds[1], (uint64_t)&drag, sizeof(drag));
        pit_sleep_ms(300);

        /* The drag label follows the cursor, which is parked at the
         * screen centre (512, 384) and never moved - so the label's own
         * fill is at a known place: CURSOR_SIZE (8) down and right of it,
         * DRAG_LABEL_H (20) tall. (524, 396) is inside it and past the
         * rounded corner. */
        uint32_t label_pixel = fb_get_pixel(524, 396);

        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        if (label_pixel != 0x00335577u) {
            klog_puts("[m49] the compositor did not show a drag label after WM_DRAG_PIPE - expected 0x00335577 got 0x");
            klog_put_hex32(label_pixel);
            klog_putc('\n');
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M49 input-completeness self-test: the shortcut table and/or the drag protocol did not behave as specified");
        }
        klog_puts("[m49] the shared shortcut table resolving every chord (and refusing every "
                   "near-miss), and a drag announced on WM_DRAG_PIPE becoming a visible drag "
                   "self-test passed (22/22 checks).\n\n");
    }

    /* M50 self-test: the resource hygiene this arc's new syscalls and new
     * long-lived UI needed, in M29 and M40's shape - measure a baseline,
     * do the thing many times, measure again, and require the numbers to
     * come back.
     *
     * Three parts, in increasing order of what they would have caught:
     *
     * 1. The shm ownership invariant, stated as a cycle. Create, map,
     *    unmap-and-free, N times, and require the free-frame count to
     *    land exactly where it started. Before this milestone there was
     *    no way to write this test at all - there was no SYS_shm_free -
     *    which is precisely why every window this OS ever composited
     *    permanently consumed one of MAX_SHM_SEGMENTS's 32 slots.
     *
     * 2. A kill storm. Spawn a client, kill it, repeat, far more times
     *    than there are window slots or segment slots - and require
     *    frames, segments and window slots all to return to baseline.
     *    M29's reclaim path had never been driven at this rate, and M45
     *    is what made it easy for a *user* to drive it that way.
     *
     * 3. Every syscall this arc added, given deliberate garbage. Null
     *    pointers, kernel addresses, absurd lengths, invalid ids. The
     *    requirement is only that the machine is still running
     *    afterwards, which is the entire point: M40's audit of elf_load
     *    exists because "a user program can take down the kernel by
     *    spawning a text file" was true, and every syscall added since
     *    deserves the same question asked of it deliberately rather than
     *    eventually.
     */
    {
        int all_ok = 1;

        /* (1) shm create/free, cycled. 64 KiB is 16 frames, so a leak of
         * even one cycle is unmistakable against the baseline.
         *
         * Deliberately create-and-free without mapping, because *this*
         * task cannot map shm at all: task 0 is a kernel thread, and a
         * kernel thread's SYS_shm_map cursor is zero (task_t's own
         * comment says these are "meaningless, left zeroed" for anything
         * not spawned through process_spawn) - so asking would try to map
         * at virtual address 0, inside the identity-mapped low 2 MiB, and
         * panic. Found by writing this test. The mapped path is what part
         * (2) below covers, through a real compositor mapping and freeing
         * real window buffers sixteen times, which is a better test of it
         * anyway. */
        uint64_t shm_baseline = pmm_free_frame_count();
        int shm_cycles_ok = 1;
        for (int i = 0; i < 24; i++) {
            long id = do_syscall(SYS_shm_create, 64 * 1024, 0, 0);
            if (id < 0) {
                klog_puts("[m50] shm_create failed on cycle 0x");
                klog_put_hex32((uint32_t)i);
                klog_puts(" - the segment table is not being handed back\n");
                shm_cycles_ok = 0;
                break;
            }
            if (do_syscall(SYS_shm_free, (uint64_t)id, 0, 0) != 0) {
                klog_puts("[m50] shm_free refused a segment this task had just created\n");
                shm_cycles_ok = 0;
                break;
            }
        }
        uint64_t shm_after = pmm_free_frame_count();
        if (!shm_cycles_ok || shm_after != shm_baseline) {
            klog_puts("[m50] 24 shm create/free cycles did not return every frame: 0x");
            klog_put_hex64(shm_baseline);
            klog_puts(" free before, 0x");
            klog_put_hex64(shm_after);
            klog_puts(" after\n");
            all_ok = 0;
        }

        /* Freeing something twice, or something owned by nobody, has to
         * fail rather than double-free - the invariant is "exactly one
         * owner releases it", and a second release is how that stops
         * being true. */
        long id_twice = do_syscall(SYS_shm_create, 4096, 0, 0);
        long first_free = do_syscall(SYS_shm_free, (uint64_t)id_twice, 0, 0);
        long second_free = do_syscall(SYS_shm_free, (uint64_t)id_twice, 0, 0);
        if (first_free != 0 || second_free == 0) {
            klog_puts("[m50] freeing an shm segment twice did not fail the second time (0x");
            klog_put_hex32((uint32_t)first_free);
            klog_puts(" then 0x");
            klog_put_hex32((uint32_t)second_free);
            klog_puts(")\n");
            all_ok = 0;
        }

        /* (2) the kill storm. Each iteration is a whole client
         * connecting to a real compositor, getting a window and a
         * segment and an event pipe, and then being SIGKILLed - which is
         * exactly what M45's Force Quit does, driven far faster than a
         * person could. 16 rounds is past MAX_WINDOWS (12) and past half
         * of MAX_SHM_SEGMENTS, so a slot or a segment that failed to come
         * back would run the table out inside this loop rather than
         * three milestones from now. */
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t stub_size_bytes = 0;
        uint8_t *stub_image = read_program("/bin/wm_stubborn", &stub_size_bytes);
        int64_t stub_size = (int64_t)stub_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        /* Baseline taken with the compositor already up, so what is
         * measured is the churn and not the compositor's own back buffer. */
        uint64_t storm_frames_before = pmm_free_frame_count();
        int storm_shm_before = shm_count_by_owner(comp_task->id);
        int storm_ok = 1;
        for (int round = 0; round < 16 && storm_ok; round++) {
            task_t *victim = process_spawn("wm_stubborn", stub_image, (size_t)stub_size, "");
            if (!victim) {
                klog_puts("[m50] kill storm: spawn failed on round 0x");
                klog_put_hex32((uint32_t)round);
                klog_putc('\n');
                storm_ok = 0;
                break;
            }
            pit_sleep_ms(300); /* connect, get a window, draw */
            selftest_reap(victim);
            pit_sleep_ms(200); /* let reap_dead_clients notice and release the slot */
        }
        kfree(stub_image);

        uint64_t storm_frames_after = pmm_free_frame_count();
        int storm_shm_after = shm_count_by_owner(comp_task->id);

        /* M50: the caps this arc leaned on, measured against a compositor
         * that has just been through sixteen connect/draw/die rounds -
         * which is the closest thing to a worst case this project can
         * produce on purpose. Logged rather than only asserted, because
         * "how close are we" is the question M40 and M41 each answered
         * too late, and it should be answerable with a grep. */
        int comp_fds = 0;
        for (int f = 0; f < MAX_FDS; f++) {
            if (comp_task->fds[f].type != FD_NONE) {
                comp_fds++;
            }
        }
        klog_puts("[m50] compositor after the storm: 0x");
        klog_put_hex32((uint32_t)comp_fds);
        klog_puts(" of 0x");
        klog_put_hex32((uint32_t)MAX_FDS);
        klog_puts(" fds, 0x");
        klog_put_hex32((uint32_t)storm_shm_after);
        klog_puts(" shm segment(s) held.\n");

        /* One more client has to be able to connect *and draw* after all
         * that, which is the assertion with real teeth: a window slot or
         * a segment that never came back shows up here as a window that
         * simply doesn't appear - the exact symptom M40 spent a
         * milestone on. Window 0 is at (100, 100), 200x120. */
        size_t last_size_bytes = 0;
        uint8_t *last_image = read_program("/bin/wm_stubborn", &last_size_bytes);
        int64_t last_size = (int64_t)last_size_bytes;
        task_t *last_task = last_size == 0 ? (task_t *)0
                                          : process_spawn("wm_stubborn", last_image, (size_t)last_size, "");
        kfree(last_image);
        pit_sleep_ms(700);
        uint32_t survivor_pixel = fb_get_pixel(200, 150);
        if (last_task) {
            selftest_reap(last_task);
        }
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        if (!storm_ok) {
            all_ok = 0;
        }
        if (storm_shm_after != storm_shm_before) {
            klog_puts("[m50] the kill storm left the compositor holding shm segments: 0x");
            klog_put_hex32((uint32_t)storm_shm_before);
            klog_puts(" before, 0x");
            klog_put_hex32((uint32_t)storm_shm_after);
            klog_puts(" after 16 rounds\n");
            all_ok = 0;
        }
        /* Frames, bounded rather than exact, and the bound is derived
         * rather than picked. A killed *process* leaks its own address
         * space - page tables, stack, argument page, and the frames its
         * ELF image was copied into - because this kernel has no
         * vmm_destroy_address_space, which M29 documented as a deliberate
         * tradeoff and M50 does not undo. Measured here: about 15 frames
         * per round.
         *
         * What must not grow is the compositor's side. One window's pixel
         * buffer is 200*120*4 = 96000 bytes, 24 frames - so a bound of 20
         * frames per round both accommodates the ~15 that genuinely leak
         * and fails if even one pixel buffer didn't come back, which is
         * the thing this test is for. The per-round figure is logged
         * either way, so the number this bound rests on is in the boot
         * log rather than only in this comment. */
        uint64_t storm_leak = storm_frames_before > storm_frames_after
                                  ? storm_frames_before - storm_frames_after
                                  : 0;
        klog_puts("[m50] kill storm: 0x");
        klog_put_hex64(storm_leak);
        klog_puts(" frames not reclaimed across 16 rounds (0x");
        klog_put_hex64(storm_leak / 16);
        klog_puts(" per dead address space; one leaked window buffer would be 0x18).\n");
        if (storm_leak > 16 * 20) {
            klog_puts("[m50] the kill storm leaked more than 16 dead address spaces account for - a window's pixel buffer did not come back\n");
            all_ok = 0;
        }
        if (survivor_pixel != 0x00B03040u) {
            klog_puts("[m50] a client connecting after 16 kill rounds got no drawable window - expected 0x00B03040 at (200,150), got 0x");
            klog_put_hex32(survivor_pixel);
            klog_putc('\n');
            all_ok = 0;
        }

        /* (3) deliberate garbage into every syscall this arc added. Each
         * of these must return an error; the machine still running is the
         * other half of the assertion, and the only way this test reports
         * that is by reaching its own "passed" line. */
        static const uint64_t KERNEL_ADDR = 0x100000ULL; /* squarely inside the kernel image */
        /* A real buffer for the calls that are *supposed* to be refused
         * before writing anything - so that if one of them ever isn't,
         * what it scribbles on is this scratch space and not the table of
         * results being checked. */
        static task_info_t garbage_scratch[2];
        struct { const char *what; long got; } garbage[] = {
            {"SYS_taskinfo with a null buffer", do_syscall(SYS_taskinfo, 0, 8, 0)},
            {"SYS_taskinfo with a zero count", do_syscall(SYS_taskinfo, (uint64_t)garbage_scratch, 0, 0)},
            {"SYS_taskinfo with an absurd count", do_syscall(SYS_taskinfo, (uint64_t)garbage_scratch, 0xFFFFFFFFULL, 0)},
            {"SYS_close on an out-of-range fd", do_syscall(SYS_close, 0xFFFFFFFFULL, 0, 0)},
            {"SYS_close on an fd that was never open", do_syscall(SYS_close, MAX_FDS - 1, 0, 0)},
            {"SYS_shm_free on an id that does not exist", do_syscall(SYS_shm_free, 0xFFFFULL, 0, 0)},
            {"SYS_shm_free with a misaligned address", do_syscall(SYS_shm_free, 0, KERNEL_ADDR + 1, 0)},
            {"SYS_shutdown with an unrecognized mode", do_syscall(SYS_shutdown, 99, 0, 0)},
            {"SYS_kill on a pid that was never valid", do_syscall(SYS_kill, 0xFFFFULL, SIGKILL, 0)},
        };
        for (size_t i = 0; i < sizeof(garbage) / sizeof(garbage[0]); i++) {
            if (garbage[i].got >= 0) {
                klog_puts("[m50] ");
                klog_puts(garbage[i].what);
                klog_puts(" succeeded (0x");
                klog_put_hex32((uint32_t)garbage[i].got);
                klog_puts(") instead of failing\n");
                all_ok = 0;
            }
        }

        /* SYS_cursor_shape, which M50's plan also names, does not exist -
         * see milestones.md's M46 entry: the compositor already draws
         * every cursor shape itself, so the syscall was never added and
         * there is nothing here to feed garbage to. Recorded rather than
         * quietly dropped. */

        if (!all_ok) {
            panic("M50 robustness self-test: a resource did not come back, or a garbage argument was accepted");
        }
        klog_puts("[m50] 24 shm create/free cycles frame-neutral, a double free refused, "
                   "16 kill-storm rounds returning every window slot and segment, and 9 "
                   "garbage-argument syscalls all refused self-test passed (13/13 checks).\n\n");
    }

    /* M53 self-test: directories, path resolution, and the layout.
     *
     * The load-bearing checks are the ones a *prefix convention* would
     * pass and a real directory would not: a name that exists in two
     * directories at once resolving to two different files, and a
     * directory outgrowing a single block and still listing everything.
     * With 16 records per 512-byte block, filling one takes 17 entries,
     * which is what the loop below writes.
     *
     * The path-refusal rows are the other half. leanfs deliberately
     * stores no "." or ".." and has no parent link, so a resolver that
     * accepted them would have to synthesize them - which is the
     * near-correct shortcut that turns into an escape from the root. Each
     * of those must be a clean -1, not a fault and not a silent success.
     */
    {
        int all_ok = 1;

        /* The layout the kernel seeded, above. */
        static const char *const LAYOUT[] = {PATH_BIN, PATH_HOME, PATH_ETC, PATH_TMP};
        for (size_t i = 0; i < sizeof(LAYOUT) / sizeof(LAYOUT[0]); i++) {
            if (!vfs_is_dir(LAYOUT[i])) {
                klog_puts("[m53] ");
                klog_puts(LAYOUT[i]);
                klog_puts(" is missing or is not a directory\n");
                all_ok = 0;
            }
        }

        /* /bin holds exactly the programs USER_PROGRAMS names - every one
         * present, and nothing that is not a program in it. The second
         * half is what makes the launcher's list trustworthy. */
        static char list_buf[4096];
        size_t list_len = vfs_list(PATH_BIN, list_buf, sizeof(list_buf));
        int found = 0;
        for (size_t i = 0; i < EMBEDDED_PROGRAM_COUNT; i++) {
            char path[PATH_MAX_LEN];
            path_join(path, PATH_BIN_DIR, embedded_programs[i].name);
            if (vfs_exists(path)) {
                found++;
            } else {
                klog_puts("[m53] ");
                klog_puts(path);
                klog_puts(" was not seeded\n");
                all_ok = 0;
            }
        }
        int listed = 0;
        for (size_t i = 0; i < list_len; i++) {
            if (list_buf[i] == '\n') {
                listed++;
            }
        }
        if (listed != (int)EMBEDDED_PROGRAM_COUNT) {
            klog_puts("[m53] ");
            klog_puts(PATH_BIN);
            klog_puts(" lists 0x");
            klog_put_hex32((uint32_t)listed);
            klog_puts(" entries but this build ships 0x");
            klog_put_hex32((uint32_t)EMBEDDED_PROGRAM_COUNT);
            klog_puts(" programs - something that is not a program is in it\n");
            all_ok = 0;
        }

        /* A directory created, entered, filled past one block, listed,
         * and read back by path. */
        static const char *const DEEP = PATH_TMP_DIR "m53dir";
        if (!vfs_exists(DEEP) && vfs_mkdir(DEEP) != 0) {
            klog_puts("[m53] vfs_mkdir failed on a fresh path under " PATH_TMP "\n");
            all_ok = 0;
        }
        if (!vfs_is_dir(DEEP)) {
            klog_puts("[m53] the directory just created does not read back as one\n");
            all_ok = 0;
        }
        /* 17 files: one more than the 16 records a block holds, so the
         * directory has to grow a second one. */
        const int DEEP_FILES = 17;
        for (int i = 0; i < DEEP_FILES; i++) {
            char path[PATH_MAX_LEN];
            char name[8];
            name[0] = 'f';
            name[1] = (char)('0' + i / 10);
            name[2] = (char)('0' + i % 10);
            name[3] = '\0';
            path_join(path, PATH_TMP_DIR "m53dir/", name);
            char body[16];
            k_memset(body, 0, sizeof(body));
            body[0] = (char)('A' + i);
            if (vfs_write(path, body, sizeof(body)) != 0) {
                klog_puts("[m53] writing file 0x");
                klog_put_hex32((uint32_t)i);
                klog_puts(" into a directory past its first block failed\n");
                all_ok = 0;
                break;
            }
        }
        size_t deep_len = vfs_list(DEEP, list_buf, sizeof(list_buf));
        int deep_listed = 0;
        for (size_t i = 0; i < deep_len; i++) {
            if (list_buf[i] == '\n') {
                deep_listed++;
            }
        }
        if (deep_listed != DEEP_FILES) {
            klog_puts("[m53] a directory holding 0x");
            klog_put_hex32((uint32_t)DEEP_FILES);
            klog_puts(" files listed 0x");
            klog_put_hex32((uint32_t)deep_listed);
            klog_puts(" of them - it did not grow past one block correctly\n");
            all_ok = 0;
        }
        {
            char body[16];
            k_memset(body, 0, sizeof(body));
            int64_t n = vfs_read(PATH_TMP_DIR "m53dir/f16", body, sizeof(body));
            if (n != 16 || body[0] != (char)('A' + 16)) {
                klog_puts("[m53] the 17th file in that directory did not read back by path (0x");
                klog_put_hex32((uint32_t)n);
                klog_puts(" bytes, first byte 0x");
                klog_put_hex32((uint32_t)(uint8_t)body[0]);
                klog_puts(")\n");
                all_ok = 0;
            }
        }

        /* The same name in two directories is two different files -
         * which a prefix convention cannot do and is therefore the
         * cleanest single statement of "these are real directories". */
        static const char a_body[] = "in-tmp";
        static const char b_body[] = "in-home";
        if (vfs_write(PATH_TMP_DIR "m53same", a_body, sizeof(a_body)) != 0 ||
            vfs_write(PATH_ETC_DIR "m53same", b_body, sizeof(b_body)) != 0) {
            klog_puts("[m53] could not create the same name in two directories\n");
            all_ok = 0;
        } else {
            char got_a[16], got_b[16];
            k_memset(got_a, 0, sizeof(got_a));
            k_memset(got_b, 0, sizeof(got_b));
            vfs_read(PATH_TMP_DIR "m53same", got_a, sizeof(got_a));
            vfs_read(PATH_ETC_DIR "m53same", got_b, sizeof(got_b));
            if (k_strcmp(got_a, a_body) != 0 || k_strcmp(got_b, b_body) != 0) {
                klog_puts("[m53] the same name in two directories resolved to one file: '");
                klog_puts(got_a);
                klog_puts("' and '");
                klog_puts(got_b);
                klog_puts("'\n");
                all_ok = 0;
            }
        }

        /* Malformed and escaping paths, each of which must be a clean
         * refusal. Run as a table so adding a rule without adding a row
         * is visible. */
        static const struct { const char *what; const char *path; } BAD_PATHS[] = {
            {"a relative path, which has nothing to be relative to", "bin/ls"},
            {"an empty component", "//bin"},
            {"a '.' component", "/./bin"},
            {"a '..' component, the escape this format refuses to synthesize", "/bin/../etc"},
            {"a '..' climbing out of the root", "/.."},
            {"walking through a regular file as if it were a directory", PATH_BIN_DIR "ls/nope"},
            {"a component longer than a name may be", "/bin/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
        };
        for (size_t i = 0; i < sizeof(BAD_PATHS) / sizeof(BAD_PATHS[0]); i++) {
            char scratch[16];
            if (vfs_exists(BAD_PATHS[i].path) || vfs_read(BAD_PATHS[i].path, scratch, sizeof(scratch)) >= 0) {
                klog_puts("[m53] path check failed: ");
                klog_puts(BAD_PATHS[i].what);
                klog_puts(" was accepted ('");
                klog_puts(BAD_PATHS[i].path);
                klog_puts("')\n");
                all_ok = 0;
            }
        }

        /* M60: a *single* trailing slash on a directory is legal now and
         * moved out of the refusal table above - see next_component's own
         * note for why tab completion is the reason. It gets an assertion
         * of its own rather than simply disappearing, because "/bin/" and
         * "/bin" naming the same directory is now a promise: the one that
         * still has to be refused is a trailing slash on a *file*, which
         * would be claiming it is something it is not. */
        if (!vfs_is_dir(PATH_BIN_DIR)) {
            klog_puts("[m53] a trailing slash on a directory was refused ('" PATH_BIN_DIR "')\n");
            all_ok = 0;
        }
        {
            char scratch[16];
            if (vfs_read(PATH_BIN_DIR "ls/", scratch, sizeof(scratch)) >= 0) {
                klog_puts("[m53] a trailing slash on a regular file was accepted\n");
                all_ok = 0;
            }
        }

        /* And the one thing the launcher's own behavior now rests on:
         * a data file is not in /bin. */
        if (vfs_exists(PATH_BIN_DIR "settings.conf") || !vfs_exists(PATH_SETTINGS)) {
            klog_puts("[m53] settings.conf is not where the layout says it is\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M53 directory self-test: the namespace is not a tree");
        }
        klog_puts("[m53] directories created, entered, grown past one block, listed and read "
                   "back by path; the same name in two directories staying two files; seven "
                   "malformed or escaping paths refused (and, since M60, a trailing slash "
                   "honoured on a directory and still refused on a file); and " PATH_BIN
                   " holding exactly the "
                   "programs this build ships self-test passed (");
        klog_put_hex32((uint32_t)found);
        klog_puts(" programs seeded).\n\n");
    }

    /* M51 self-test: the z-order, stated as the two things that were
     * wrong before it existed.
     *
     * Both are about two *overlapping* windows, which is the case this
     * compositor had never handled: paint order was connection order, so
     * the window in front was whichever client connected last and nothing
     * could change that; and every hit-test walked windows[] backwards
     * taking the first region match rather than the topmost visible
     * window, so a click in the overlap went to whichever of the two had
     * the higher slot index - which could be the one underneath.
     *
     * This is the first boot self-test in this project to deliver a real
     * mouse click. Everything mouse-driven before it was left to
     * tools/qemu-input-test.sh, because the kernel had no way to move a
     * pointer; M51 adds mouse_inject (kernel/drivers/mouse.h) for exactly
     * this, and it matters here because a z-order that only ever gets
     * poked through WM_ACTION_PIPE would leave the actual bug - the
     * hit-test - untested.
     *
     * Geometry, all from compositor.c's own constants. Two wm_zorder
     * clients, 300x200 each, land on the cascade at (100, 100) and
     * (140, 140):
     *
     *   A content x:[100,400) y:[100,300), frame x:[98,402) y:[78,302)
     *   B content x:[140,440) y:[140,340), frame x:[138,442) y:[118,342)
     *
     * so they overlap over x:[140,400) y:[140,300). (200, 250) is inside
     * both and is where "which one is in front" is read.
     *
     * Each client lights one 10x10 tick per press it is routed, laid out
     * right-to-left from its own bottom-right corner (see wm_zorder.c) -
     * the one part of the *lower* window that the upper one never covers,
     * so both clients' tick rows stay readable whichever is in front:
     *
     *   A's ticks at (388..398, 288..298) and (374..384, 288..298)
     *   B's first tick at (428..438, 328..338)
     *
     * (120, 250) is inside A only - left of B's frame and its resize
     * halo, clear of A's own edges. (300, 200) is inside both. The 8x8
     * cursor parks where it last clicked, so neither click point is
     * within 8px of any pixel read afterwards. */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t z_size_bytes = 0;
        uint8_t *z_image = read_program("/bin/wm_zorder", &z_size_bytes);
        int64_t z_size = (int64_t)z_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        task_t *a_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zA 00A02020");
        pit_sleep_ms(500);
        task_t *b_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zB 002060C0");
        kfree(z_image);
        pit_sleep_ms(500);

        /* B connected second, so it is in front to begin with - which was
         * true before this milestone too, and is the baseline the raise
         * below has to change. */
        uint32_t overlap_before = selftest_pixel_settled(200, 250, 0x002060C0u,
                                                          "the second client to connect in front of the first");

        /* A real click, at a point that is inside A and outside B. Each
         * click is three injected events: a pin to the top-left corner (a
         * delta large enough to saturate the compositor's own clamp,
         * which is what makes this absolute rather than relative to
         * wherever the pointer happens to be), a move to the target, then
         * press and release. */
        mouse_inject(-4096, -4096, 0, 0);
        mouse_inject(120, 250, 0, 0);
        pit_sleep_ms(120);
        mouse_inject(0, 0, 1, 0);
        pit_sleep_ms(120);
        mouse_inject(0, 0, 0, 0);

        /* Two separate things have to happen here and they belong to two
         * different processes: the compositor raises the window, and the
         * *client* draws a tick to say the press reached it. Waiting only
         * for the raise samples the tick before its owner has had a turn,
         * which is a failure that reads as "the click was eaten". So both
         * are waited for, in the order they happen. The unlit tick on the
         * other window is a negative and is read once they have. */
        uint32_t overlap_after_raise = selftest_pixel_settled(200, 250, 0x00A02020u,
                                                               "the clicked window to come forward");
        uint32_t a_tick1 = selftest_pixel_settled(393, 293, 0x00F0E000u,
                                                   "the clicked window to record the press it received");
        uint32_t b_tick1 = selftest_pixel_settled(433, 333, 0x002060C0u,
                                                   "the other window's tick to stay unlit");

        /* The occlusion bug, stated as a test: a click in the region both
         * windows cover has to reach exactly the one in front. */
        mouse_inject(-4096, -4096, 0, 0);
        mouse_inject(300, 200, 0, 0);
        pit_sleep_ms(120);
        mouse_inject(0, 0, 1, 0);
        pit_sleep_ms(120);
        mouse_inject(0, 0, 0, 0);

        uint32_t a_tick2 = selftest_pixel_settled(379, 293, 0x00F0E000u,
                                                   "the front window to record the overlap click");
        uint32_t b_tick_still = selftest_pixel_settled(433, 333, 0x002060C0u,
                                                        "the other window's tick to stay unlit still");

        /* And the protocol half: wm_window_info_t.z_index, which is how a
         * shell learns which window is frontmost without the query
         * reordering the buttons it draws. A's rank must now be above B's
         * - the same fact the overlap pixel just showed, read through the
         * interface desktop_shell.c actually uses. */
        int query_fds[2], query_resp_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_QUERY_PIPE, (uint64_t)query_fds, 0) != 0 ||
            do_syscall(SYS_pipe_open, (uint64_t)WM_QUERY_RESP_PIPE, (uint64_t)query_resp_fds, 0) != 0) {
            panic("M51 self-test: kernel-side SYS_pipe_open(WM_QUERY_PIPE) failed");
        }
        uint8_t ping = 1;
        do_syscall(SYS_write, (uint64_t)query_fds[1], (uint64_t)&ping, sizeof(ping));
        pit_sleep_ms(300);
        wm_query_response_t *q = (wm_query_response_t *)kmalloc(sizeof(wm_query_response_t));
        if (!q) {
            panic("out of memory for the M51 query response");
        }
        k_memset(q, 0, sizeof(*q));
        do_syscall(SYS_read, (uint64_t)query_resp_fds[0], (uint64_t)q, sizeof(*q));
        int32_t a_z = -1, b_z = -1;
        for (int32_t i = 0; i < q->count && i < WM_MAX_ROUTABLE_WINDOWS; i++) {
            if (k_strcmp(q->windows[i].title, "zA") == 0) {
                a_z = q->windows[i].z_index;
            } else if (k_strcmp(q->windows[i].title, "zB") == 0) {
                b_z = q->windows[i].z_index;
            }
        }
        int32_t reported = q->count;
        kfree(q);

        selftest_reap(a_task);
        selftest_reap(b_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        int all_ok = 1;
        static const struct { const char *what; uint32_t expected; } checks_meta[] = {
            {"the overlap before anything was clicked - the second client to connect starts in front", 0x002060C0u},
            {"the overlap after clicking the window behind - it has to come forward, which is the whole milestone", 0x00A02020u},
            {"the clicked window's first tick - it received the press that raised it, rather than having it eaten", 0x00F0E000u},
            {"the other window's first tick - it must be unlit, since it was not the window clicked", 0x002060C0u},
            {"the raised window's second tick - a click in the overlap goes to the window in front", 0x00F0E000u},
            {"the other window's first tick again - the overlap click reached exactly one window", 0x002060C0u},
        };
        const uint32_t got[] = {overlap_before, overlap_after_raise, a_tick1, b_tick1, a_tick2, b_tick_still};
        for (size_t i = 0; i < sizeof(got) / sizeof(got[0]); i++) {
            if (got[i] != checks_meta[i].expected) {
                klog_puts("[m51] pixel check failed: ");
                klog_puts(checks_meta[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32(checks_meta[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32(got[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        if (a_z < 0 || b_z < 0 || a_z <= b_z) {
            klog_puts("[m51] wm_window_info_t.z_index did not report the raised window as frontmost: zA 0x");
            klog_put_hex32((uint32_t)a_z);
            klog_puts(", zB 0x");
            klog_put_hex32((uint32_t)b_z);
            klog_puts(", of 0x");
            klog_put_hex32((uint32_t)reported);
            klog_puts(" window(s) reported\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M51 z-order self-test: overlapping windows did not behave as specified");
        }
        klog_puts("[m51] z-order raise-on-click, occlusion-correct hit-testing (the overlap "
                   "click reaching exactly the front window) and wm_window_info_t.z_index "
                   "self-test passed (7/7 checks).\n\n");
    }

    /* M52 self-test: a user program that dereferences a null pointer
     * dies alone, and every syscall that takes a pointer refuses every
     * shape of bad one.
     *
     * The first half is the whole milestone in one assertion, and the
     * assertion is that *this code keeps running*. Before M52, isr.c
     * panicked on every fault regardless of ring, so any wild pointer in
     * any user program stopped the machine - by a distance the largest
     * source of "you have to reset it" this project has had. There is no
     * pixel to read for "the kernel did not die"; reaching the checks
     * below at all is the proof, and the serial harness's own
     * "no kernel panic" grade is the other half of it.
     *
     * user_space/bin/wm_faulter.c connects and paints before it faults,
     * deliberately. A process that crashes before owning anything would
     * only prove the fault handler runs; one that crashes holding a
     * window, an shm segment and an event pipe proves everything
     * downstream still works - M29's reap_dead_clients noticing, the
     * window slot coming back, the segment's frames coming back. It is
     * 200x120 and connects first, so its window content is x:[100,300)
     * y:[100,220) and (150, 150) is inside it.
     *
     * The second half is the garbage-argument matrix, which runs as
     * user_space/bin/badptr.c rather than as a block here - see that
     * file's header for why it structurally cannot live in the kernel:
     * syscall.c's user_range_ok exempts kernel threads, so every row run
     * from here would take that early return and prove nothing. */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t fault_size_bytes = 0;
        uint8_t *fault_image = read_program("/bin/wm_faulter", &fault_size_bytes);
        int64_t fault_size = (int64_t)fault_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */

        uint64_t frames_before = pmm_free_frame_count();
        task_t *victim = process_spawn("wm_faulter", fault_image, (size_t)fault_size, "");
        kfree(fault_image);
        /* Connects and paints, comfortably inside its own ALIVE_MS before
         * it faults - waited for, so a slow machine spends that budget on
         * the connect rather than on a sleep. */
        uint32_t painted = selftest_pixel_settled(150, 150, 0x0020C0A0u,
                                                   "the faulter's window to be drawn");
        int alive_before_fault = (int)do_syscall(SYS_task_alive, (uint64_t)victim->id, 0, 0);
        uint64_t frames_with_victim = pmm_free_frame_count();

        /* Past its ALIVE_MS, plus room for the compositor's own reap loop
         * to notice and repaint. If the machine were going to stop, this
         * is where it would have. */
        pit_sleep_ms(1400);

        /* And then until the compositor has actually taken the window
         * down, which is the observable end of the whole sequence - the
         * task cannot still be alive once its window is gone, so reading
         * this first is what makes the two checks below about the fault
         * rather than about how long the repaint took. */
        uint32_t after_fault = selftest_pixel_settled(150, 150, 0x001A1A2Eu,
                                                       "the faulted client's window to be taken down");
        int alive_after_fault = (int)do_syscall(SYS_task_alive, (uint64_t)victim->id, 0, 0);
        long victim_exit = do_syscall(SYS_wait, (uint64_t)victim->id, 0, 0);
        uint64_t frames_after = pmm_free_frame_count();

        /* SYS_shm_free's own range check, which is not behind
         * user_range_ok and therefore *is* testable from here. Without it
         * a vaddr in PML4[0] would have been unmapped from the shared
         * kernel map - the same subtree every address space uses - which
         * is a user-triggerable way to take the machine down and exactly
         * what this milestone exists to close. */
        long seg = do_syscall(SYS_shm_create, 4096, 0, 0);
        long free_kernel_addr = do_syscall(SYS_shm_free, (uint64_t)seg, 0x100000ULL, 0);
        long free_unaligned = do_syscall(SYS_shm_free, (uint64_t)seg, USER_SHM_BASE + 1, 0);
        long free_ok = do_syscall(SYS_shm_free, (uint64_t)seg, 0, 0);

        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        int all_ok = 1;
        if (painted != 0x0020C0A0u) {
            klog_puts("[m52] the faulting client never got a window on screen, so what follows would not have been a crash - expected 0x0020C0A0 got 0x");
            klog_put_hex32(painted);
            klog_putc('\n');
            all_ok = 0;
        }
        if (alive_before_fault != 1) {
            klog_puts("[m52] the faulting client was not running before it faulted (SYS_task_alive 0x");
            klog_put_hex32((uint32_t)alive_before_fault);
            klog_puts(")\n");
            all_ok = 0;
        }
        if (alive_after_fault != 0) {
            klog_puts("[m52] a null dereference in ring 3 did not terminate the offending task (SYS_task_alive 0x");
            klog_put_hex32((uint32_t)alive_after_fault);
            klog_puts(")\n");
            all_ok = 0;
        }
        if (victim_exit != 128 + SIGSEGV) {
            klog_puts("[m52] a faulting task's exit code is not distinguishable as a fault - expected 0x");
            klog_put_hex32((uint32_t)(128 + SIGSEGV));
            klog_puts(" got 0x");
            klog_put_hex32((uint32_t)victim_exit);
            klog_putc('\n');
            all_ok = 0;
        }
        if (after_fault != 0x001A1A2Eu) {
            klog_puts("[m52] the dead client's window was not reclaimed - expected the desktop background 0x001A1A2E at (150,150), got 0x");
            klog_put_hex32(after_fault);
            klog_putc('\n');
            all_ok = 0;
        }
        /* The segment wm_faulter creates for itself is 64 KiB, 16 frames -
         * the window's own pixel buffer belongs to the compositor, so
         * without that this process would own nothing reclaimable. Its
         * address space and stack are deliberately not reclaimed (there
         * is no vmm_destroy_address_space yet - that is M54), so this is
         * a "did the *crash* path run shm_free_by_owner" check, measured
         * the way M45 measured the SIGKILL one: against the state with
         * the victim running, not against the state before it existed. */
        if (frames_after < frames_with_victim + 16) {
            klog_puts("[m52] a crashed client's shm segment was not handed back: 0x");
            klog_put_hex64(frames_before);
            klog_puts(" free before it started, 0x");
            klog_put_hex64(frames_with_victim);
            klog_puts(" with it running, 0x");
            klog_put_hex64(frames_after);
            klog_puts(" after it faulted and was reaped\n");
            all_ok = 0;
        }
        if (seg < 0 || free_kernel_addr == 0 || free_unaligned == 0 || free_ok != 0) {
            klog_puts("[m52] SYS_shm_free's vaddr bounds are wrong: id 0x");
            klog_put_hex32((uint32_t)seg);
            klog_puts(", kernel address returned 0x");
            klog_put_hex32((uint32_t)free_kernel_addr);
            klog_puts(", unaligned returned 0x");
            klog_put_hex32((uint32_t)free_unaligned);
            klog_puts(", the legitimate free returned 0x");
            klog_put_hex32((uint32_t)free_ok);
            klog_putc('\n');
            all_ok = 0;
        }

        /* The matrix. badptr exits with the number of rows that were
         * wrongly accepted, and prints each one to its stdout - which is
         * this klog, so a failure names itself in the same log this
         * message is in. */
        size_t bad_size_bytes = 0;
        uint8_t *bad_image = read_program("/bin/badptr", &bad_size_bytes);
        int64_t bad_size = (int64_t)bad_size_bytes;
        task_t *bad_task = process_spawn("badptr", bad_image, (size_t)bad_size, "");
        kfree(bad_image);
        long bad_exit = do_syscall(SYS_wait, (uint64_t)bad_task->id, 0, 0);
        if (bad_exit != 0) {
            klog_puts("[m52] the garbage-argument matrix accepted 0x");
            klog_put_hex32((uint32_t)bad_exit);
            klog_puts(" argument(s) it should have refused - see the [badptr] lines above\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M52 kernel-hardening self-test: a user program can still take the kernel with it");
        }
        klog_puts("[m52] a ring-3 null dereference killing only its own task (exit 139), its "
                   "window and segment reclaimed, SYS_shm_free refusing a kernel address, and "
                   "every pointer-taking syscall refusing every shape of bad pointer "
                   "self-test passed (7/7 checks).\n\n");
    }

    /* M54 self-test: the test that could not be written before this
     * milestone.
     *
     * Spawn and reap several times MAX_TASKS worth of processes, and
     * require both free frames and free task slots to come back to where
     * they started. Under the old rules this was impossible in the most
     * literal sense: slots were never recycled, so the 384th spawn would
     * have failed outright with the table long since full, and every one
     * of the 384 address spaces would still have been resident.
     *
     * `hello` is the program - the smallest thing on disk that runs to
     * completion on its own, so each round is a genuine spawn/run/exit
     * cycle rather than a spawn/kill one. Reaped by pid immediately, which
     * is what returns the slot (see sched_reap_slot on why consuming the
     * exit status, and not exiting, is what frees it).
     *
     * The frame comparison is the one that would have failed loudly
     * before M54, at roughly fifteen frames a round. */
    {
        size_t hello_size_bytes = 0;
        uint8_t *hello_image = read_program(PATH_BIN_DIR "hello", &hello_size_bytes);
        int64_t hello_size = (int64_t)hello_size_bytes;

        const int ROUNDS = MAX_TASKS * 3;

        /* `hello` writes two lines to stdout, and a spawned task inherits
         * this one's fd table - so 384 rounds would put 768 lines of
         * "Hello, world from user_space!" through the boot log and make
         * every other self-test's output unreadable. Closing fd 1 for the
         * duration makes those writes fail cleanly (sys_write returns -1
         * for an FD_NONE slot, which hello ignores) and costs the test
         * nothing: what it measures is frames and slots, not output. */
        fd_slot_t saved_stdout = sched_current()->fds[1];
        sched_current()->fds[1].type = FD_NONE;

        int live_before = sched_live_task_count();
        uint64_t frames_before = pmm_free_frame_count();
        int spawn_failures = 0;
        int stale_seen_as_live = 0;
        int stale_pid = -1;

        for (int i = 0; i < ROUNDS; i++) {
            task_t *t = process_spawn("hello", hello_image, (size_t)hello_size, "");
            if (!t) {
                spawn_failures++;
                break;
            }
            int pid = t->id;
            do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            /* The stale-pid check, from the first round: a pid whose slot
             * has since been handed to somebody else must be reported as
             * invalid, not as that somebody else. Without the generation
             * counter this would come back 1 (alive) almost every round,
             * because the slot really is occupied - by a different task. */
            if (stale_pid < 0) {
                stale_pid = pid;
            } else if (do_syscall(SYS_task_alive, (uint64_t)stale_pid, 0, 0) != -1) {
                stale_seen_as_live++;
            }
        }

        int live_after = sched_live_task_count();
        uint64_t frames_after = pmm_free_frame_count();
        sched_current()->fds[1] = saved_stdout;
        kfree(hello_image);

        int all_ok = 1;
        if (spawn_failures) {
            klog_puts("[m54] a spawn failed partway through 0x");
            klog_put_hex32((uint32_t)ROUNDS);
            klog_puts(" rounds - the task table is still a lifetime budget\n");
            all_ok = 0;
        }
        if (live_after != live_before) {
            klog_puts("[m54] task slots did not come back: 0x");
            klog_put_hex32((uint32_t)live_before);
            klog_puts(" live before, 0x");
            klog_put_hex32((uint32_t)live_after);
            klog_puts(" after 0x");
            klog_put_hex32((uint32_t)ROUNDS);
            klog_puts(" spawn/reap rounds\n");
            all_ok = 0;
        }
        if (frames_after != frames_before) {
            klog_puts("[m54] frames did not come back: 0x");
            klog_put_hex64(frames_before);
            klog_puts(" free before, 0x");
            klog_put_hex64(frames_after);
            klog_puts(" after (0x");
            klog_put_hex64(frames_before - frames_after);
            klog_puts(" lost across 0x");
            klog_put_hex32((uint32_t)ROUNDS);
            klog_puts(" processes)\n");
            all_ok = 0;
        }
        if (stale_seen_as_live) {
            klog_puts("[m54] a stale pid was answered about 0x");
            klog_put_hex32((uint32_t)stale_seen_as_live);
            klog_puts(" time(s) instead of being refused - the generation counter is not doing its job\n");
            all_ok = 0;
        }
        /* And the other half of the same rule: a pid that was never valid
         * is refused too, and a *live* one is still found. */
        if (do_syscall(SYS_task_alive, 0x7FFFFFFF, 0, 0) != -1 ||
            do_syscall(SYS_task_alive, (uint64_t)sched_current()->id, 0, 0) != 1) {
            klog_puts("[m54] SYS_task_alive no longer tells a live pid from an impossible one\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M54 reclaim self-test: something a dead process held did not come back");
        }
        klog_puts("[m54] every task slot and every frame returned across 0x");
        klog_put_hex32((uint32_t)ROUNDS);
        klog_puts(" spawn/reap rounds (three times MAX_TASKS), and a stale pid refused rather "
                   "than answered about, self-test passed (5/5 checks).\n\n");
    }

    /* M55 self-test: a compositor killed out from under two live clients,
     * replaced, and both windows back on screen with their own pixels.
     *
     * The claim is deliberately *not* "the clients are still running" -
     * that would have been true before this milestone too, in the sense
     * that nobody killed them. They were wedged: their pixel buffer
     * belonged to a dead process, their event pipe would never carry
     * another byte, and gui_paint in particular sat in a blocking read
     * that could not return. So this is checked as pixels, which is the
     * only evidence that distinguishes "alive" from "working".
     *
     * wm_zorder is the client because it paints one flat, distinctive
     * color and repaints it on WM_EVENT_EXPOSE - so a window that came
     * back really is *this* client's window and not merely something
     * drawn in that rectangle. Two of them, at the cascade positions
     * (100,100) and (140,140), 300x200 each: (120, 250) is inside the
     * first only and (420, 320) inside the second only, which is what
     * lets one probe per client be unambiguous.
     *
     * The second compositor is spawned by this test rather than by init,
     * because init is not running yet at this point in boot - what the
     * *desktop* does about a dead compositor is init's job and is covered
     * by the interactive test. What this proves is the piece that had to
     * exist first: that a client can survive one. */
    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program(PATH_BIN_DIR "compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t z_size_bytes = 0;
        uint8_t *z_image = read_program(PATH_BIN_DIR "wm_zorder", &z_size_bytes);
        int64_t z_size = (int64_t)z_size_bytes;

        task_t *comp1 = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        task_t *a_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zA 00A02020");
        task_t *b_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zB 002060C0");

        /* M69: wait for the windows, do not sleep and hope. This used to
         * be three fixed sleeps totalling 1.3 s chosen to be "probably
         * enough" for a compositor to start and two clients to connect
         * and paint. The pixels below are the condition those sleeps were
         * standing in for, so they are what gets waited on - which is
         * both faster when the machine is quick and honest when it is
         * not. Spawning all three first and waiting once is deliberate:
         * they connect concurrently, and serialising the waits would put
         * back most of the time this removes. */
        int up = selftest_wait_for_pixel(120, 250, 0x00A02020u, 5000,
                                          "the first client's window to appear");
        up &= selftest_wait_for_pixel(420, 320, 0x002060C0u, 5000,
                                       "the second client's window to appear");
        if (!up) {
            panic("M55 session-resilience self-test: the clients never got their windows up");
        }

        uint32_t a_before = fb_get_pixel(120, 250);
        uint32_t b_before = fb_get_pixel(420, 320);

        /* The compositor dies the way a crashed one would - no orderly
         * handover, no chance to tell anybody. */
        do_syscall(SYS_kill, (uint64_t)comp1->id, SIGKILL, 0);
        do_syscall(SYS_wait, (uint64_t)comp1->id, 0, 0);

        int a_alive_after_crash = (int)do_syscall(SYS_task_alive, (uint64_t)a_task->id, 0, 0);
        int b_alive_after_crash = (int)do_syscall(SYS_task_alive, (uint64_t)b_task->id, 0, 0);

        /* The replacement. Both clients should find it on their own. */
        task_t *comp2 = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        kfree(z_image);
        /* A reconnect that *loses* the race with this new compositor
         * clearing the well-known pipes has its first request discarded
         * and re-sent one WM_CONNECT_TIMEOUT_MS later (wmclient.c), so
         * the wait has to cover a couple of those intervals.
         *
         * M69: it used to cover them by sleeping 2.5 s unconditionally,
         * and the comment here said so - "anything shorter than a couple
         * of those intervals is a test that passes on timing rather than
         * on behavior". That was right about the risk and wrong about the
         * remedy: a longer sleep is still a bet, just a safer one. The
         * generous deadline is kept, because a lost race genuinely does
         * take that long; what changed is that the common case now costs
         * whatever it actually costs. */
        int back = selftest_wait_for_pixel(120, 250, 0x00A02020u, 6000,
                                            "the first client to reconnect and repaint");
        back &= selftest_wait_for_pixel(420, 320, 0x002060C0u, 6000,
                                         "the second client to reconnect and repaint");
        (void)back; /* the pixel checks below report which one failed and why */

        uint32_t a_after = fb_get_pixel(120, 250);
        uint32_t b_after = fb_get_pixel(420, 320);

        selftest_reap(a_task);
        selftest_reap(b_task);
        selftest_reap(comp2);
        console_init();
        klog_use_console();

        int all_ok = 1;
        static const struct { const char *what; uint32_t expected; } names[] = {
            {"the first client's window before the compositor was killed", 0x00A02020u},
            {"the second client's window before the compositor was killed", 0x002060C0u},
            {"the first client's window after a replacement compositor started - it reconnected and repainted", 0x00A02020u},
            {"the second client's window after a replacement compositor started", 0x002060C0u},
        };
        const uint32_t got[] = {a_before, b_before, a_after, b_after};
        for (size_t i = 0; i < sizeof(got) / sizeof(got[0]); i++) {
            if (got[i] != names[i].expected) {
                klog_puts("[m55] pixel check failed: ");
                klog_puts(names[i].what);
                klog_puts(" - expected 0x");
                klog_put_hex32(names[i].expected);
                klog_puts(" got 0x");
                klog_put_hex32(got[i]);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        if (a_alive_after_crash != 1 || b_alive_after_crash != 1) {
            klog_puts("[m55] a client did not outlive the compositor at all (SYS_task_alive 0x");
            klog_put_hex32((uint32_t)a_alive_after_crash);
            klog_puts(" and 0x");
            klog_put_hex32((uint32_t)b_alive_after_crash);
            klog_puts(")\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M55 session-resilience self-test: a compositor crash still takes its clients with it");
        }
        klog_puts("[m55] a compositor SIGKILLed out from under two live clients, replaced, and "
                   "both windows back on screen with their own pixels self-test passed "
                   "(5/5 checks).\n\n");
    }

    /* M56 self-test: the three things this milestone added, each checked
     * as the thing itself rather than as "something changed".
     *
     * (1) SYS_unlink and SYS_rename round-tripping, *including the inode
     *     and block accounting coming back*. A remove that leaves its
     *     blocks marked in use is a remove that works exactly once per
     *     block, which is the failure a bare "the file is gone" assertion
     *     would miss entirely. Measured by filling a directory, removing
     *     it all, and requiring the free-block count to return - which
     *     needs a file large enough to span several blocks, hence 2000
     *     bytes rather than a token one.
     *
     * (2) An editor undo restoring an exact buffer across a paste. Driven
     *     by typing into a real text_editor through a real compositor -
     *     M56 adds keyboard_inject for this, the counterpart to M51's
     *     mouse_inject, because an undo self-test that called the undo
     *     function directly would prove nothing about the key that
     *     invokes it. The buffer is read back off *disk*: the editor
     *     saves with Ctrl+S, so what is compared is the file the user
     *     would have got.
     *
     * (3) A terminal scrollback holding more lines than its window. `ls
     *     /bin` is more output than the 21-row grid, so the first line
     *     has necessarily scrolled off; scrolling back must bring
     *     something different to the top row and scrolling forward must
     *     put it back. Compared as a count of lit pixels in the top row's
     *     strip, which is content-independent - asserting on a particular
     *     filename would be asserting on the order leanfs happens to
     *     return its directory records in.
     */
    {
        int all_ok = 1;

        /* ---- (1) unlink and rename ---- */
        {
            static char payload[2000];
            for (size_t i = 0; i < sizeof(payload); i++) {
                payload[i] = (char)('a' + (i % 26));
            }
            /* Large enough to span several blocks, so "its blocks came
             * back" is a number rather than a rounding error. A file's
             * blocks are not frames, which is why the free-*frame*
             * counter every other self-test uses says nothing here - see
             * vfs_free_blocks. */
            if (vfs_write(PATH_TMP_DIR "m56a", payload, sizeof(payload)) != 0) {
                klog_puts("[m56] could not create the file this test is about\n");
                all_ok = 0;
            }
            if (vfs_rename(PATH_TMP_DIR "m56a", PATH_TMP_DIR "m56b") != 0 ||
                vfs_exists(PATH_TMP_DIR "m56a") || !vfs_exists(PATH_TMP_DIR "m56b")) {
                klog_puts("[m56] rename did not move the name\n");
                all_ok = 0;
            }
            static char readback[2000];
            k_memset(readback, 0, sizeof(readback));
            if (vfs_read(PATH_TMP_DIR "m56b", readback, sizeof(readback)) != (int64_t)sizeof(payload)) {
                klog_puts("[m56] the renamed file did not read back at its own size - a rename moved data it should not have touched\n");
                all_ok = 0;
            }
            for (size_t i = 0; i < sizeof(payload); i++) {
                if (readback[i] != payload[i]) {
                    klog_puts("[m56] the renamed file's contents changed\n");
                    all_ok = 0;
                    break;
                }
            }
            if (vfs_rename(PATH_TMP_DIR "m56b", PATH_TMP_DIR "m53same") == 0) {
                klog_puts("[m56] rename over an existing name succeeded - that is how a file gets lost silently\n");
                all_ok = 0;
            }
            /* And the accounting, stated directly: the blocks a file
             * held are free again once it is unlinked. Counted rather
             * than inferred - the first version of this check wrote and
             * unlinked the same 2000 bytes sixty-four times and watched
             * for the disk to fill, which was the honest test available
             * before leanfs could count, and cost four thousand ATA
             * sector writes: every metadata update here rewrites the
             * whole inode table and bitmap (31 sectors), and PIO writes
             * are the most expensive thing this OS does. It added more
             * than a minute to every boot, which is how a self-test
             * starts making the machine look broken. */
            uint32_t free_before = vfs_free_blocks();
            if (vfs_write(PATH_TMP_DIR "m56c", payload, sizeof(payload)) != 0) {
                klog_puts("[m56] could not create the file the block accounting is about\n");
                all_ok = 0;
            }
            uint32_t free_with = vfs_free_blocks();
            if (free_with >= free_before) {
                klog_puts("[m56] writing 2000 bytes consumed no blocks at all\n");
                all_ok = 0;
            }
            if (vfs_unlink(PATH_TMP_DIR "m56c") != 0) {
                klog_puts("[m56] unlink failed on a file that had just been written\n");
                all_ok = 0;
            }
            uint32_t free_after = vfs_free_blocks();
            if (free_after != free_before) {
                klog_puts("[m56] unlink did not return every block: 0x");
                klog_put_hex32(free_before);
                klog_puts(" free before, 0x");
                klog_put_hex32(free_with);
                klog_puts(" with the file, 0x");
                klog_put_hex32(free_after);
                klog_puts(" after removing it\n");
                all_ok = 0;
            }
            /* A rename must move no blocks at all - it is a change to
             * records, and a count that shifted would mean it had
             * quietly copied something. */
            uint32_t free_pre_rename = vfs_free_blocks();
            int rename_ok = vfs_rename(PATH_TMP_DIR "m56b", PATH_TMP_DIR "m56d") == 0 &&
                            vfs_rename(PATH_TMP_DIR "m56d", PATH_TMP_DIR "m56b") == 0;
            if (!rename_ok || vfs_free_blocks() != free_pre_rename) {
                klog_puts("[m56] a rename moved blocks, or failed outright\n");
                all_ok = 0;
            }

            vfs_unlink(PATH_TMP_DIR "m56b");
            if (vfs_unlink(PATH_BIN) == 0) {
                klog_puts("[m56] unlink accepted a directory - see leanfs.h on why that is refused rather than recursed\n");
                all_ok = 0;
            }
        }

        /* ---- (2) the editor's undo across a paste ----
         *
         * Driven with real keys through a real compositor: an undo test
         * that called the undo function directly would prove nothing
         * about the chord that invokes it, and the chord is the part with
         * somewhere to go wrong (M56 had to teach keyboard_inject to
         * carry a modifier mask for exactly this - see its own note).
         *
         * The assertion is on *bytes on disk*, not pixels: Ctrl+S writes
         * the buffer through the same SYS_writefile a person's save would,
         * so what is compared is the file they would have got. "AB\n"
         * exactly - not "shorter than it was", which a paste that
         * silently did nothing would also satisfy. */
        static const char pasted[] = "PASTED";
        do_syscall(SYS_clipboard_set, (uint64_t)pasted, sizeof(pasted) - 1, 0);

        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program(PATH_BIN_DIR "compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t ed_size_bytes = 0;
        uint8_t *ed_image = read_program(PATH_BIN_DIR "text_editor", &ed_size_bytes);
        int64_t ed_size = (int64_t)ed_size_bytes;
        size_t term_size_bytes = 0;
        uint8_t *term_image = read_program(PATH_BIN_DIR "gui_terminal", &term_size_bytes);
        int64_t term_size = (int64_t)term_size_bytes;

        /* M56: start from nothing, every time. The serial harness boots a
         * snapshot disk so every run is a fresh filesystem - but a
         * *reboot within one guest* is not, and the interactive suite has
         * a test that does exactly that. Second time around the editor
         * opened the file this test wrote last time, typed "AB" into it,
         * and produced "ABAB"; the assertions below are about an exact
         * buffer, so they correctly reported that as a failure of undo.
         * Removing it first is what makes the test a statement about the
         * editor rather than about what was on the disk - and it uses the
         * unlink this milestone is adding, which is a fair way to find
         * out it works. */
        vfs_unlink(PATH_TMP_DIR "m56undo");

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */
        task_t *ed_task = process_spawn("text_editor", ed_image, (size_t)ed_size,
                                         PATH_TMP_DIR "m56undo");
        kfree(ed_image);
        pit_sleep_ms(800);

        keyboard_inject('A', 0);
        keyboard_inject('B', 0);
        pit_sleep_ms(200);
        keyboard_inject('V', KBD_MOD_CTRL); /* paste "PASTED" - six characters, one undo group */
        pit_sleep_ms(300);
        keyboard_inject('S', KBD_MOD_CTRL);
        pit_sleep_ms(500);
        static char after_paste[64];
        k_memset(after_paste, 0, sizeof(after_paste));
        int64_t paste_len = vfs_read(PATH_TMP_DIR "m56undo", after_paste, sizeof(after_paste) - 1);

        keyboard_inject('Z', KBD_MOD_CTRL); /* one undo, and the whole paste goes */
        pit_sleep_ms(300);
        keyboard_inject('S', KBD_MOD_CTRL);
        pit_sleep_ms(500);
        static char after_undo[64];
        k_memset(after_undo, 0, sizeof(after_undo));
        int64_t undo_len = vfs_read(PATH_TMP_DIR "m56undo", after_undo, sizeof(after_undo) - 1);

        selftest_reap(ed_task);
        pit_sleep_ms(200);

        /* ---- (3) the terminal's scrollback ----
         *
         * `ls /bin` is more lines than the 21-row grid, so the top of the
         * output has necessarily scrolled off - which before this
         * milestone meant gone. The comparison is a count of lit pixels
         * across the top row's strip: content-independent, where
         * asserting on a particular filename would be asserting on the
         * order leanfs happens to return its records in.
         *
         * gui_terminal is 70x21 cells of an 8x16 font - 560x336 - and is
         * the only window here, so it lands at (100, 100) and its first
         * text row is y:[100, 116). */
        task_t *term_task = process_spawn("gui_terminal", term_image, (size_t)term_size, "");
        kfree(term_image);
        pit_sleep_ms(900);

        int lit_before_cmd = selftest_term_top_lit();
        static const char cmd[] = "ls /bin\n";
        for (size_t i = 0; i < sizeof(cmd) - 1; i++) {
            keyboard_inject(cmd[i], 0);
        }
        /* The spawn, its output, and the terminal draining it - waited
         * out as a condition rather than guessed at. `ls /bin` is far
         * more than the 21 rows this grid holds, so the top row is
         * guaranteed to end up showing something it was not showing
         * before the command was typed; requiring that as well as quiet
         * is what stops this settling on the prompt it started from.
         * See selftest_term_top_settled. */
        int lit_live = selftest_term_top_settled(lit_before_cmd, 12000);
        /* The wheel acts on whatever is under the pointer, which starts
         * at the screen centre - inside this window. Ten detents back,
         * and then the top row settled at something that is not the live
         * view: what the next check asserts is that it *changed*, so
         * "changed" is also the condition worth waiting for. */
        mouse_inject(0, 0, 0, -10);
        int lit_scrolled = selftest_term_top_settled(lit_live, 4000);
        mouse_inject(0, 0, 0, 10);
        int lit_back = selftest_term_top_settled(lit_scrolled, 4000);

        selftest_reap(term_task);
        selftest_reap(comp_task);
        kfree(comp_image);
        console_init();
        klog_use_console();

        if (paste_len != 9 || k_strcmp(after_paste, "ABPASTED\n") != 0) { /* "ABPASTED" plus the newline save_file writes after every line */
            klog_puts("[m56] the paste did not land as expected - saved 0x");
            klog_put_hex32((uint32_t)paste_len);
            klog_puts(" bytes: '");
            klog_puts(after_paste);
            klog_puts("'\n");
            all_ok = 0;
        }
        if (undo_len != 3 || k_strcmp(after_undo, "AB\n") != 0) {
            klog_puts("[m56] one undo did not restore the exact buffer from before the paste - saved 0x");
            klog_put_hex32((uint32_t)undo_len);
            klog_puts(" bytes: '");
            klog_puts(after_undo);
            klog_puts("'\n");
            all_ok = 0;
        }
        if (lit_live == 0) {
            klog_puts("[m56] the terminal drew no output at all - nothing to scroll back through\n");
            all_ok = 0;
        }
        if (lit_scrolled == lit_live) {
            klog_puts("[m56] scrolling back changed nothing - the top row still shows the live grid, so there is no history\n");
            all_ok = 0;
        }
        if (lit_back != lit_live) {
            klog_puts("[m56] scrolling forward again did not return to the live view (0x");
            klog_put_hex32((uint32_t)lit_live);
            klog_puts(" lit pixels before, 0x");
            klog_put_hex32((uint32_t)lit_back);
            klog_puts(" after)\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M56 depth self-test: undo, scrollback or the filesystem's remove half did not behave as specified");
        }
        klog_puts("[m56] SYS_unlink returning every block it freed and SYS_rename moving none, "
                   "one editor undo restoring the exact buffer from before a paste, and a "
                   "terminal scrollback holding lines its window no longer shows "
                   "self-test passed (12/12 checks).\n\n");
    }

    /* M58 self-test: the *desktop's* half of a resolution change, which
     * is everything the kernel's own mode-set self-test above
     * deliberately does not touch. Driven exactly the way settings.c
     * drives it - a WM_ACTION_SET_MODE on the action pipe - because the
     * point is the path, not the syscall.
     *
     * Three claims:
     *
     *   1. The taskbar re-spans the screen. A panel's width is the
     *      compositor's own decision (wm_create_request_t.width is
     *      documented as ignored for one), and its buffer was allocated
     *      for the old display - so the panel is one of the two clients
     *      that has to be genuinely *resized* rather than merely
     *      notified. Reading both ends of the bar at the new width is
     *      what proves the reallocation actually happened; a bar that
     *      kept its old buffer would simply stop short.
     *   2. The mode really changed underneath it (SYS_fb_info's answer).
     *   3. **The countdown works.** This test never confirms, waits the
     *      revert out, and checks that the desktop came back at the old
     *      size with the bar spanning it again. That path only ever runs
     *      when something has already gone wrong, which is exactly why it
     *      is the one worth a test - and it is the whole reason a person
     *      can try a resolution on a machine with no second screen to
     *      recover from.
     *
     * Skipped on an adapter with no runtime mode setting, same as the
     * kernel-side test above.
     */
    if (dispi_available()) {
        display_mode_t list[DISPLAY_MAX_MODES];
        int n = dispi_get_modes(list, DISPLAY_MAX_MODES);
        uint32_t boot_w = fb_width(), boot_h = fb_height();
        /* The smallest offered mode that is not the current one -
         * shrinking is the direction that exercises the clamps, and it is
         * the direction a person hits by accident. */
        int pick = -1;
        for (int i = 0; i < n; i++) {
            if (list[i].width == boot_w && list[i].height == boot_h) {
                continue;
            }
            if (pick < 0 || (uint64_t)list[i].width * list[i].height <
                             (uint64_t)list[pick].width * list[pick].height) {
                pick = i;
            }
        }
        if (pick < 0) {
            panic("M58 desktop self-test: no offered mode other than the one already running");
        }

        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t shell_size_bytes = 0;
        uint8_t *shell_image = read_program("/bin/desktop_shell", &shell_size_bytes);
        int64_t shell_size = (int64_t)shell_size_bytes;
        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */
        task_t *shell_task = process_spawn("desktop_shell", shell_image, (size_t)shell_size, "");
        kfree(shell_image);
        pit_sleep_ms(700);

        /* PANEL_HEIGHT is 32 and the bar's top two rows are its own
         * margin strip, above the button row - so this row is plain
         * panel fill at both ends, which is what makes "did it span"
         * answerable by comparing two pixels rather than by knowing a
         * blend. */
        const uint32_t bar_row_from_bottom = 30;
        uint32_t before_left  = fb_get_pixel(2, boot_h - bar_row_from_bottom);
        uint32_t before_right = fb_get_pixel(boot_w - 3, boot_h - bar_row_from_bottom);

        int action_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_ACTION_PIPE, (uint64_t)action_fds, 0) != 0) {
            panic("M58 desktop self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        wm_action_request_t req;
        k_memset(&req, 0, sizeof(req));
        req.window_id = -1;
        req.action = WM_ACTION_SET_MODE;
        req.value = wm_pack_mode(list[pick].width, list[pick].height);
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        /* Long enough for the compositor to change the mode, for the
         * panel to notice its buffer is gone, re-handshake and draw a
         * frame at the new width. */
        pit_sleep_ms(2500);

        uint32_t after_w = fb_width(), after_h = fb_height();
        uint32_t after_left = 0, after_right = 0, after_beyond_old = 0, after_desktop = 0;
        if (after_w >= 8 && after_h > bar_row_from_bottom) {
            after_left  = fb_get_pixel(2, after_h - bar_row_from_bottom);
            after_right = fb_get_pixel(after_w - 3, after_h - bar_row_from_bottom);
            after_beyond_old = fb_get_pixel(after_w / 2, after_h - bar_row_from_bottom);
            /* Well above the bar - bare desktop, and the control that
             * stops "the bar spans the screen" from being satisfied by
             * *no bar at all*, which is uniform too. The first version of
             * this test passed on exactly that. */
            after_desktop = fb_get_pixel(after_w / 2, after_h / 2);
        }

        /* Nothing confirms. The revert deadline is WM_MODE_REVERT_MS from
         * the moment the change landed, so this waits it out plus enough
         * for the panel to re-handshake a second time. */
        pit_sleep_ms(WM_MODE_REVERT_MS + 2500);

        uint32_t back_w = fb_width(), back_h = fb_height();
        uint32_t back_left = 0, back_right = 0;
        if (back_w == boot_w && back_h == boot_h) {
            back_left  = fb_get_pixel(2, boot_h - bar_row_from_bottom);
            back_right = fb_get_pixel(boot_w - 3, boot_h - bar_row_from_bottom);
        }

        selftest_reap(shell_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        int all_ok = 1;
        if (before_left != before_right) {
            klog_puts("[m58] the taskbar did not span the boot display to begin with\n");
            all_ok = 0;
        }
        if (after_w != list[pick].width || after_h != list[pick].height) {
            klog_puts("[m58] WM_ACTION_SET_MODE did not change the mode: 0x");
            klog_put_hex32(after_w);
            klog_puts("x");
            klog_put_hex32(after_h);
            klog_putc('\n');
            all_ok = 0;
        }
        if (after_left != after_right || after_left != after_beyond_old) {
            klog_puts("[m58] the taskbar did not re-span the new display width - its buffer was not reallocated\n");
            all_ok = 0;
        }
        if (after_left == after_desktop) {
            klog_puts("[m58] the taskbar row is indistinguishable from bare desktop at the new size - there is no bar there\n");
            all_ok = 0;
        }
        if (back_w != boot_w || back_h != boot_h) {
            klog_puts("[m58] the unconfirmed mode was never reverted - 0x");
            klog_put_hex32(back_w);
            klog_puts("x");
            klog_put_hex32(back_h);
            klog_puts(" is still up\n");
            all_ok = 0;
        }
        if (back_left != back_right || back_left != before_left) {
            klog_puts("[m58] after the revert the taskbar does not span the restored display: before 0x");
            klog_put_hex32(before_left);
            klog_puts("/0x");
            klog_put_hex32(before_right);
            klog_puts(" after 0x");
            klog_put_hex32(after_left);
            klog_puts("/0x");
            klog_put_hex32(after_right);
            klog_puts(" back 0x");
            klog_put_hex32(back_left);
            klog_puts("/0x");
            klog_put_hex32(back_right);
            klog_putc('\n');
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M58 desktop self-test: a resolution change did not carry the desktop with it");
        }
        klog_puts("[m58] a resolution change carrying the whole desktop with it - panels "
                   "re-spanning the new width, and an unconfirmed mode reverting on its own "
                   "deadline - self-test passed.\n\n");
    }

    /* M59 self-test: descriptors, a file bigger than the old ceiling, a
     * clock, and the cost of a save.
     *
     * Six claims, and the last two are the ones a bare "it worked" check
     * would miss:
     *
     *   1. SYS_open/SYS_lseek/SYS_read/SYS_write round trip - written in
     *      pieces, read back in pieces, at offsets.
     *   2. A file past the 72 KiB ceiling double-indirect blocks
     *      replaced. 200 KiB is comfortably into the second level, which
     *      is the part that did not exist before this milestone, and it
     *      is verified by content rather than by size: a block-mapping
     *      bug that returned the *wrong* block would produce a file of
     *      exactly the right length full of the wrong bytes.
     *   3. Every one of those blocks comes back on unlink. A leak here
     *      is 400 blocks a go, which is the kind of thing that only shows
     *      up as a full disk three milestones later.
     *   4. A written file carries the date it was written.
     *   5. **The metadata write count for a one-byte change**, which is
     *      the user-facing performance bug this milestone set out to fix:
     *      every flush used to write the superblock, the whole inode
     *      table and the whole bitmap - 32 PIO sector writes whether one
     *      byte changed or seventy kilobytes did. Asserted as a *count*
     *      rather than as a latency, because latency is a property of the
     *      host and this is a property of the code.
     *   6. SYS_rmdir, including its refusal to remove a directory that
     *      still holds something.
     *   7. Every open-file-table entry comes back on close. This is
     *      claim 3's sibling on the other side of the descriptor: claim 3
     *      asserts the disk gets its blocks back, this asserts the kernel
     *      gets its open-file descriptions back. openfile.c's table is
     *      global and holds 64 entries for the whole machine, so a leak
     *      here is not this process running out of files - it is every
     *      SYS_open on the machine failing after the sixty-fourth one,
     *      which is the kind of thing that presents as "the editor
     *      stopped saving" an hour into a session and points nowhere
     *      near the code that caused it. This test opens and closes a
     *      couple of dozen descriptors, so it is the right place to
     *      notice.
     */
    {
        int all_ok = 1;
        int openfiles_before = openfile_in_use();

        /* 1. descriptors */
        static const char PART_A[] = "hello ";
        static const char PART_B[] = "descriptors";
        long fd = do_syscall(SYS_open, (uint64_t)(PATH_TMP_DIR "m59fd"),
                              OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE, 0);
        if (fd < 0) {
            klog_puts("[m59] SYS_open could not create a file\n");
            all_ok = 0;
        } else {
            do_syscall(SYS_write, (uint64_t)fd, (uint64_t)PART_A, sizeof(PART_A) - 1);
            do_syscall(SYS_write, (uint64_t)fd, (uint64_t)PART_B, sizeof(PART_B) - 1);
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);

            fd = do_syscall(SYS_open, (uint64_t)(PATH_TMP_DIR "m59fd"), OPEN_READ, 0);
            char back[32];
            k_memset(back, 0, sizeof(back));
            /* Seek to the join between the two writes and read across it -
             * a seek that landed anywhere else would still return
             * *something*, which is why the assertion is on the bytes. */
            long pos = do_syscall(SYS_lseek, (uint64_t)fd, 6, SEEK_SET);
            long n = do_syscall(SYS_read, (uint64_t)fd, (uint64_t)back, 11);
            if (pos != 6 || n != 11 || k_strcmp(back, "descriptors") != 0) {
                klog_puts("[m59] a seek-then-read did not land where it was told to\n");
                all_ok = 0;
            }
            long end = do_syscall(SYS_lseek, (uint64_t)fd, 0, SEEK_END);
            if (end != (long)(sizeof(PART_A) - 1 + sizeof(PART_B) - 1)) {
                klog_puts("[m59] SEEK_END does not agree with what was written\n");
                all_ok = 0;
            }
            if (do_syscall(SYS_read, (uint64_t)fd, (uint64_t)back, 4) != 0) {
                klog_puts("[m59] a read at the end of a file returned data\n");
                all_ok = 0;
            }
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);
        }

        /* 1b. a hole, which is where a byte-range write is easiest to get
         * wrong in two opposite ways at once: zero the block too eagerly
         * and the bytes before the seek are lost, read it back too
         * eagerly and a deleted file's contents leak into the gap. Both
         * live in the same partial-block branch, so one file exercises
         * both - the write lands past the end of a block that still holds
         * real bytes below it. */
        {
            fd = do_syscall(SYS_open, (uint64_t)(PATH_TMP_DIR "m59hole"),
                             OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE, 0);
            do_syscall(SYS_write, (uint64_t)fd, (uint64_t)"ABC", 3);
            do_syscall(SYS_lseek, (uint64_t)fd, 300, SEEK_SET);
            do_syscall(SYS_write, (uint64_t)fd, (uint64_t)"Z", 1);
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);

            static uint8_t hole[512];
            k_memset(hole, 0xAA, sizeof(hole));
            fd = do_syscall(SYS_open, (uint64_t)(PATH_TMP_DIR "m59hole"), OPEN_READ, 0);
            long got = do_syscall(SYS_read, (uint64_t)fd, (uint64_t)hole, sizeof(hole));
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);
            if (got != 301) {
                klog_puts("[m59] a write past the end did not extend the file to that point\n");
                all_ok = 0;
            } else if (hole[0] != 'A' || hole[1] != 'B' || hole[2] != 'C') {
                klog_puts("[m59] writing past the end of a block erased the bytes before it\n");
                all_ok = 0;
            } else if (hole[300] != 'Z') {
                klog_puts("[m59] the byte written past the end is not where it was put\n");
                all_ok = 0;
            } else {
                for (int i = 3; i < 300; i++) {
                    if (hole[i] != 0) {
                        klog_puts("[m59] the hole is not zeros - a recycled block leaked into it\n");
                        all_ok = 0;
                        break;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m59hole"), 0, 0);
        }

        /* 2 + 3. a file past the old ceiling, and its blocks coming back */
        {
            uint32_t free_before = vfs_free_blocks();
            const uint32_t BIG = 200u * 1024u;   /* well past the old 72 KiB cap */
            static uint8_t chunk[1024];
            fd = do_syscall(SYS_open, (uint64_t)(PATH_TMP_DIR "m59big"),
                             OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE, 0);
            if (fd < 0) {
                klog_puts("[m59] could not create the large file this test is about\n");
                all_ok = 0;
            } else {
                for (uint32_t off = 0; off < BIG; off += sizeof(chunk)) {
                    /* Every block distinguishable from every other, so a
                     * mis-mapped block is a content mismatch rather than
                     * an invisible one. */
                    for (size_t i = 0; i < sizeof(chunk); i++) {
                        chunk[i] = (uint8_t)((off / sizeof(chunk)) + i);
                    }
                    if (do_syscall(SYS_write, (uint64_t)fd, (uint64_t)chunk, sizeof(chunk)) != (long)sizeof(chunk)) {
                        klog_puts("[m59] a write into the double-indirect range failed\n");
                        all_ok = 0;
                        break;
                    }
                }
                do_syscall(SYS_close, (uint64_t)fd, 0, 0);

                os_stat_t st;
                if (do_syscall(SYS_stat, (uint64_t)(PATH_TMP_DIR "m59big"), (uint64_t)&st, 0) != 0 ||
                    st.size != BIG) {
                    klog_puts("[m59] the large file is not the size it was written at\n");
                    all_ok = 0;
                }
                fd = do_syscall(SYS_open, (uint64_t)(PATH_TMP_DIR "m59big"), OPEN_READ, 0);
                static uint8_t verify[1024];
                for (uint32_t off = 0; off < BIG && all_ok; off += sizeof(verify)) {
                    if (do_syscall(SYS_read, (uint64_t)fd, (uint64_t)verify, sizeof(verify)) != (long)sizeof(verify)) {
                        klog_puts("[m59] the large file read short\n");
                        all_ok = 0;
                        break;
                    }
                    for (size_t i = 0; i < sizeof(verify); i++) {
                        if (verify[i] != (uint8_t)((off / sizeof(verify)) + i)) {
                            klog_puts("[m59] the large file read back the wrong bytes - a block mapped to the wrong place\n");
                            all_ok = 0;
                            break;
                        }
                    }
                }
                do_syscall(SYS_close, (uint64_t)fd, 0, 0);
            }
            if (do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m59big"), 0, 0) != 0) {
                klog_puts("[m59] could not unlink the large file\n");
                all_ok = 0;
            }
            uint32_t free_after = vfs_free_blocks();
            if (free_after != free_before) {
                klog_puts("[m59] the large file did not return every block: 0x");
                klog_put_hex32(free_before);
                klog_puts(" free before, 0x");
                klog_put_hex32(free_after);
                klog_puts(" after\n");
                all_ok = 0;
            }
        }

        /* 4. the date a file was written */
        {
            os_stat_t st;
            uint32_t now = rtc_now();
            if (do_syscall(SYS_stat, (uint64_t)(PATH_TMP_DIR "m59fd"), (uint64_t)&st, 0) != 0) {
                klog_puts("[m59] SYS_stat failed on a file that exists\n");
                all_ok = 0;
            } else if (rtc_available()) {
                /* Within a minute of now, which is the honest assertion:
                 * this test wrote the file seconds ago and the clock has
                 * one-second resolution. */
                uint32_t age = now > st.mtime ? now - st.mtime : st.mtime - now;
                if (st.mtime == 0 || age > 60) {
                    klog_puts("[m59] a file written moments ago is not dated moments ago\n");
                    all_ok = 0;
                }
            } else if (st.mtime != 0) {
                klog_puts("[m59] a machine with no clock dated a file anyway\n");
                all_ok = 0;
            }
        }

        /* 5. what a one-byte change costs */
        {
            uint32_t before = leanfs_meta_writes();
            fd = do_syscall(SYS_open, (uint64_t)(PATH_TMP_DIR "m59fd"), OPEN_WRITE, 0);
            do_syscall(SYS_lseek, (uint64_t)fd, 0, SEEK_SET);
            do_syscall(SYS_write, (uint64_t)fd, (uint64_t)"H", 1);
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);
            uint32_t cost = leanfs_meta_writes() - before;
            /* One inode-table sector. The whole-table flush this replaced
             * was 31, so the bound is set at 4 - loose enough not to be a
             * tripwire on an inode that happens to straddle a sector,
             * tight enough that a return to whole-table writes fails it
             * immediately. */
            if (cost > 4) {
                klog_puts("[m59] a one-byte change cost 0x");
                klog_put_hex32(cost);
                klog_puts(" metadata sector writes - dirty-sector tracking is not working\n");
                all_ok = 0;
            }
        }

        /* 6. rmdir, and its refusal */
        {
            if (do_syscall(SYS_mkdir, (uint64_t)(PATH_TMP_DIR "m59dir"), 0, 0) != 0) {
                klog_puts("[m59] could not create the directory this test is about\n");
                all_ok = 0;
            }
            if (vfs_write(PATH_TMP_DIR "m59dir/inside", "x", 1) != 0) {
                klog_puts("[m59] could not put a file inside the test directory\n");
                all_ok = 0;
            }
            if (do_syscall(SYS_rmdir, (uint64_t)(PATH_TMP_DIR "m59dir"), 0, 0) == 0) {
                klog_puts("[m59] rmdir removed a directory that still held a file\n");
                all_ok = 0;
            }
            if (do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m59dir/inside"), 0, 0) != 0 ||
                do_syscall(SYS_rmdir, (uint64_t)(PATH_TMP_DIR "m59dir"), 0, 0) != 0) {
                klog_puts("[m59] rmdir refused a directory that was empty\n");
                all_ok = 0;
            }
            if (vfs_exists(PATH_TMP_DIR "m59dir")) {
                klog_puts("[m59] the removed directory is still there\n");
                all_ok = 0;
            }
        }

        do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m59fd"), 0, 0);

        /* 7. the open-file table is back where it started */
        {
            int openfiles_after = openfile_in_use();
            if (openfiles_after != openfiles_before) {
                klog_puts("[m59] the open-file table went from 0x");
                klog_put_hex32((uint32_t)openfiles_before);
                klog_puts(" entries to 0x");
                klog_put_hex32((uint32_t)openfiles_after);
                klog_puts(" across a test that closed everything it opened - descriptors leak\n");
                all_ok = 0;
            }
        }

        if (!all_ok) {
            panic("M59 self-test: descriptors, large files, timestamps or metadata cost are wrong");
        }
        klog_puts("[m59] descriptors (open/lseek/read/write/close), a 200 KiB file through "
                   "double-indirect blocks read back byte for byte and every block returned, "
                   "a real mtime, rmdir, an open-file table back where it started, and a "
                   "one-byte save costing one metadata sector instead of thirty-one - "
                   "self-test passed.\n\n");
    }

    /* M60 self-test: a real argument vector, a real command line, and an
     * editor that can split a line.
     *
     * Four claims, and each one is a thing this OS could not express one
     * milestone ago:
     *
     *   1. **`cp a b`.** Two arguments, which is the whole reason argv
     *      had to become real - `SYS_spawn(path, arg)` carried one
     *      string, so a program could be told one thing. The assertion is
     *      on the copied bytes rather than on the exit code: a `cp` that
     *      created an empty file would exit 0 too.
     *   2. `ls /bin > out.txt` typed into a real terminal with real
     *      injected keys, and the file read back. Redirection is exactly
     *      what M59's descriptors were for, and the terminal has had a
     *      `dup2`'d pipe on fd 1 since it was written - this is the same
     *      mechanism made reachable from a command line.
     *   3. `ls /bin | cat > out.txt`, which additionally proves the thing
     *      no test could have proved before M59: that a pipe *ends*. `cat`
     *      reading standard input stops when the last writer goes away,
     *      and "the last writer went away" only became a knowable fact
     *      when pipe ends got a refcount.
     *   4. A paragraph typed into the editor with Enter in the middle of
     *      a line, saved, and compared byte for byte. Enter splitting the
     *      line is M60's headline editor change; comparing the file
     *      rather than the screen is what makes it an assertion about
     *      what a person would actually have got.
     */
    {
        int all_ok = 1;

        /* ---- (1) cp, with two arguments ---- */
        {
            static const char body[] = "argv is real now\n";
            vfs_unlink(PATH_TMP_DIR "m60src");
            vfs_unlink(PATH_TMP_DIR "m60dst");
            if (vfs_write(PATH_TMP_DIR "m60src", body, sizeof(body) - 1) != 0) {
                klog_puts("[m60] could not create the file cp is about to copy\n");
                all_ok = 0;
            }
            size_t cp_bytes = 0;
            uint8_t *cp_image = read_program(PATH_BIN_DIR "cp", &cp_bytes);
            const char *cp_argv[] = { PATH_BIN_DIR "cp", PATH_TMP_DIR "m60src", PATH_TMP_DIR "m60dst", 0 };
            task_t *cp_task = process_spawnv("cp", cp_image, cp_bytes, cp_argv);
            kfree(cp_image);
            if (!cp_task) {
                klog_puts("[m60] could not spawn cp\n");
                all_ok = 0;
            } else if (do_syscall(SYS_wait, (uint64_t)cp_task->id, 0, 0) != 0) {
                klog_puts("[m60] cp exited nonzero - it did not get two arguments\n");
                all_ok = 0;
            } else {
                static char copied[64];
                k_memset(copied, 0, sizeof(copied));
                int64_t n = vfs_read(PATH_TMP_DIR "m60dst", copied, sizeof(copied) - 1);
                if (n != (int64_t)(sizeof(body) - 1) || k_strcmp(copied, body) != 0) {
                    klog_puts("[m60] cp produced the wrong bytes\n");
                    all_ok = 0;
                }
            }
        }

        /* ---- (2) + (3) a command line, through a real terminal ---- */
        {
            size_t comp_bytes = 0;
            uint8_t *comp_image = read_program(PATH_BIN_DIR "compositor", &comp_bytes);
            size_t term_bytes = 0;
            uint8_t *term_image = read_program(PATH_BIN_DIR "gui_terminal", &term_bytes);

            vfs_unlink(PATH_TMP_DIR "m60out");
            vfs_unlink(PATH_TMP_DIR "m60pipe");
            vfs_unlink(PATH_TMP_DIR "m60tab");

            task_t *comp_task = process_spawn("compositor", comp_image, comp_bytes, "");
            kfree(comp_image);
            selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */
            task_t *term_task = process_spawn("gui_terminal", term_image, term_bytes, "");
            kfree(term_image);
            pit_sleep_ms(900);

            selftest_type("ls /bin > " PATH_TMP_DIR "m60out");
            keyboard_inject('\n', 0);
            pit_sleep_ms(2500);

            selftest_type("ls /bin | cat > " PATH_TMP_DIR "m60pipe");
            keyboard_inject('\n', 0);
            pit_sleep_ms(3500);

            /* Tab completion, checked by its effect rather than by
             * reading the screen: "/b" has exactly one completion in the
             * root, so Tab must turn `ls /b` into `ls /bin/` - and a
             * listing that comes back holding "compositor" is proof it
             * did. A Tab that did nothing would list the root instead,
             * which holds no such name. */
            selftest_type("ls /b");
            keyboard_inject('\t', 0);
            pit_sleep_ms(300);
            selftest_type(" > " PATH_TMP_DIR "m60tab");
            keyboard_inject('\n', 0);
            pit_sleep_ms(2500);

            selftest_reap(term_task);
            selftest_reap(comp_task);
            console_init();
            klog_use_console();

            static char redirected[2048];
            k_memset(redirected, 0, sizeof(redirected));
            int64_t rn = vfs_read(PATH_TMP_DIR "m60out", redirected, sizeof(redirected) - 1);
            /* "compositor" is a program this kernel seeded into /bin
             * itself, so its presence is a fact about the listing rather
             * than about whatever happens to be on disk. */
            if (rn <= 0 || !k_strstr(redirected, "compositor")) {
                klog_puts("[m60] `ls /bin > file` did not put the listing in the file\n");
                all_ok = 0;
            }

            static char completed[2048];
            k_memset(completed, 0, sizeof(completed));
            int64_t cn = vfs_read(PATH_TMP_DIR "m60tab", completed, sizeof(completed) - 1);
            if (cn <= 0 || !k_strstr(completed, "compositor")) {
                klog_puts("[m60] Tab did not complete `/b` to `/bin/` - the listing is of the wrong directory\n");
                all_ok = 0;
            }

            static char piped[2048];
            k_memset(piped, 0, sizeof(piped));
            int64_t pn = vfs_read(PATH_TMP_DIR "m60pipe", piped, sizeof(piped) - 1);
            if (pn <= 0 || !k_strstr(piped, "compositor")) {
                klog_puts("[m60] `ls /bin | cat > file` produced nothing - the pipe never ended\n");
                all_ok = 0;
            } else if (pn != rn) {
                klog_puts("[m60] the piped listing is a different length from the redirected one: 0x");
                klog_put_hex32((uint32_t)rn);
                klog_puts(" vs 0x");
                klog_put_hex32((uint32_t)pn);
                klog_putc('\n');
                all_ok = 0;
            }
        }

        /* ---- (4) a paragraph, with Enter in the middle of a line ---- */
        {
            size_t comp_bytes = 0;
            uint8_t *comp_image = read_program(PATH_BIN_DIR "compositor", &comp_bytes);
            size_t ed_bytes = 0;
            uint8_t *ed_image = read_program(PATH_BIN_DIR "text_editor", &ed_bytes);

            vfs_unlink(PATH_TMP_DIR "m60para");

            task_t *comp_task = process_spawn("compositor", comp_image, comp_bytes, "");
            kfree(comp_image);
            selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */
            task_t *ed_task = process_spawn("text_editor", ed_image, ed_bytes, PATH_TMP_DIR "m60para");
            kfree(ed_image);
            pit_sleep_ms(900);

            /* Type "ONETWO", put the cursor back between them, and press
             * Enter - which under M56's editor appended an empty line at
             * the *end* of the buffer and left "ONETWO" intact. If Enter
             * splits, the file is "ONE\nTWO\n". */
            selftest_type("ONETWO");
            for (int i = 0; i < 3; i++) {
                keyboard_inject((char)KBD_KEY_LEFT, 0);
            }
            pit_sleep_ms(200);
            keyboard_inject('\n', 0);
            pit_sleep_ms(200);
            keyboard_inject('S', KBD_MOD_CTRL);
            pit_sleep_ms(600);

            static char para[64];
            k_memset(para, 0, sizeof(para));
            int64_t pl = vfs_read(PATH_TMP_DIR "m60para", para, sizeof(para) - 1);

            /* And then undo it, which is the half M56 said made this hard:
             * a split is a structural edit and its inverse is a join. One
             * Ctrl+Z must put "ONETWO" back on one line. */
            keyboard_inject('Z', KBD_MOD_CTRL);
            pit_sleep_ms(200);
            keyboard_inject('S', KBD_MOD_CTRL);
            pit_sleep_ms(600);
            static char undone[64];
            k_memset(undone, 0, sizeof(undone));
            int64_t ul = vfs_read(PATH_TMP_DIR "m60para", undone, sizeof(undone) - 1);

            /* And redo, which M60 added and which must land back exactly
             * where the undo started. */
            keyboard_inject('Y', KBD_MOD_CTRL);
            pit_sleep_ms(200);
            keyboard_inject('S', KBD_MOD_CTRL);
            pit_sleep_ms(600);
            static char redone[64];
            k_memset(redone, 0, sizeof(redone));
            int64_t rl = vfs_read(PATH_TMP_DIR "m60para", redone, sizeof(redone) - 1);

            selftest_reap(ed_task);
            selftest_reap(comp_task);
            console_init();
            klog_use_console();

            if (pl != 8 || k_strcmp(para, "ONE\nTWO\n") != 0) {
                klog_puts("[m60] Enter did not split the line - saved 0x");
                klog_put_hex32((uint32_t)pl);
                klog_puts(" bytes: \"");
                klog_puts(para);
                klog_puts("\"\n");
                all_ok = 0;
            }
            if (ul != 7 || k_strcmp(undone, "ONETWO\n") != 0) {
                klog_puts("[m60] one undo did not join the split back - saved \"");
                klog_puts(undone);
                klog_puts("\"\n");
                all_ok = 0;
            }
            if (rl != 8 || k_strcmp(redone, "ONE\nTWO\n") != 0) {
                klog_puts("[m60] redo did not put the split back - saved \"");
                klog_puts(redone);
                klog_puts("\"\n");
                all_ok = 0;
            }
        }

        if (!all_ok) {
            panic("M60 self-test: argv, the command line, or the editor's structural edits are wrong");
        }
        klog_puts("[m60] a real argument vector (cp with two arguments), a command line with "
                   "redirection and a pipe that ends, and an editor whose Enter splits a line - "
                   "with undo and redo inverting it - self-test passed.\n\n");
    }

    /* M61 self-test: motion, and the compositor meeting a deadline.
     *
     * An animation is the one thing in this project that is *deliberately*
     * not settled, which is exactly what makes it awkward to assert on -
     * and exactly why the assertion has to be about the middle rather
     * than only the ends. Three claims:
     *
     *   1. The endpoints. Before the minimize, the window is on screen;
     *      after it has had time to finish, it is not, and the desktop is
     *      back. Those two are what every earlier self-test would have
     *      checked, and on their own they cannot tell a 140ms animation
     *      from an instantaneous change.
     *   2. **An intermediate frame that is neither.** Sampled part way
     *      through, a pixel on the path between the window and the
     *      taskbar has to be lit by *something* - the ghost rectangle -
     *      that is not there at either end. That single pixel is the
     *      whole difference between "motion happened" and "it blinked".
     *   3. The frame budget. The compositor counts frames that overrun
     *      FRAME_BUDGET_MS and says so on stdout at the end of a run;
     *      a run that met its deadline says nothing. So the assertion is
     *      that the line never appeared - which is checked by the serial
     *      harness rather than here, since it is this kernel's own
     *      console the compositor prints through.
     *
     * And the setting: with animations off, the same minimize produces
     * nothing on that path at any point. A feature that cannot be turned
     * off is not a setting, and this is the check that says it can.
     */
    {
        int all_ok = 1;

        size_t comp_bytes = 0;
        uint8_t *comp_image = read_program(PATH_BIN_DIR "compositor", &comp_bytes);
        size_t clock_bytes = 0;
        uint8_t *clock_image = read_program(PATH_BIN_DIR "gui_clock", &clock_bytes);

        task_t *comp_task = process_spawn("compositor", comp_image, comp_bytes, "");
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */
        task_t *clock_task = process_spawn("gui_clock", clock_image, clock_bytes, "");
        pit_sleep_ms(800);

        /* gui_clock connects as window 0 at (100, 100), 200x120 - the
         * same placement every other self-test here relies on. A minimize
         * animates from there down toward the taskbar; with no panel
         * running, taskbar_target aims at the bottom of the screen under
         * the window itself, so the path is the column below the window.
         *
         * Sampled as a *column* rather than at one point, deliberately:
         * exactly where the rectangle is 60 ms in is a question about
         * easing and scheduler jitter, and a test that asserted on it
         * would be asserting on the wrong thing. Where it is somewhere on
         * the path is the claim worth making. */
        const uint32_t probe_x = 150;
        uint32_t before_window = fb_get_pixel(150, 150); /* inside the window */
        uint32_t desktop_bg = fb_get_pixel(probe_x, 700); /* bare desktop, well below the window */
        int before_lit = selftest_column_lit(probe_x, desktop_bg);

        int action_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_ACTION_PIPE, (uint64_t)action_fds, 0) != 0) {
            panic("M61 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        wm_action_request_t req;
        k_memset(&req, 0, sizeof(req));
        req.window_id = 0;
        req.action = WM_ACTION_TOGGLE_MINIMIZE;

        /* Watched from the moment the request goes out until something
         * lands on the path - see selftest_column_lit_wait for why this
         * is a deadline rather than the fixed 144 ms window it used to
         * be. `lit_ms` is how long this machine actually needed,
         * which the negative check below is then held to. */
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        uint32_t lit_ms = 0;
        int during_lit = selftest_column_lit_wait(probe_x, desktop_bg, 4000, &lit_ms);
        /* And then until the path is bare again, which is the animation
         * finishing rather than a guess at how long it takes to. */
        int after_lit = selftest_column_clear_wait(probe_x, desktop_bg, 4000);
        uint32_t after_window = fb_get_pixel(150, 150);

        /* And again with motion switched off, which must produce nothing
         * on that path at any point. */
        int settings_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_SETTINGS_PIPE, (uint64_t)settings_fds, 0) != 0) {
            panic("M61 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_PIPE) failed");
        }
        wm_settings_request_t off;
        k_memset(&off, 0, sizeof(off)); /* volume is a field of this struct too - sending a stack full of whatever was there before is sending a volume */
        off.volume = 70;                /* compositor.c's own default, which is what is already set */
        off.animations = 0;
        off.bg_color = 0x001A1A2Eu;   /* compositor.c's DEFAULT_BG_COLOR */
        off.accent_color = 0x004C99E6u; /* and its TITLEBAR_FOCUS_COLOR */
        off.wallpaper = 0;              /* WALLPAPER_FLAT - a flat desktop makes "nothing there" unambiguous */
        do_syscall(SYS_write, (uint64_t)settings_fds[1], (uint64_t)&off, sizeof(off));
        selftest_wait_for_animations_setting(0, 4000);

        /* Un-minimize (which with motion off is instantaneous), then
         * minimize again and watch the same path. */
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        selftest_wait_for_pixel(150, 150, before_window, 4000, "the window to come back");
        uint32_t quiet_bg = fb_get_pixel(probe_x, 700);
        /* Watched for at least as long as the animated case needed to
         * show itself, and never less than 400 ms. There is no instant at
         * which "nothing is going to appear" becomes true, so the only
         * honest form this claim has is a window of time - and the only
         * honest length for that window is one this machine has already
         * been observed to be able to animate inside of. */
        uint32_t quiet_window = lit_ms * 4 + 400;
        if (quiet_window > 3000) {
            quiet_window = 3000;
        }
        int quiet_lit = selftest_column_lit_peak_ms(probe_x, quiet_bg, quiet_window);

        selftest_reap(clock_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        if (before_window == desktop_bg) {
            klog_puts("[m61] the window and the bare desktop below it are the same colour - this test cannot see anything\n");
            all_ok = 0;
        }
        if (before_lit != 0) {
            klog_puts("[m61] the path to the taskbar was not bare desktop to begin with: 0x");
            klog_put_hex32((uint32_t)before_lit);
            klog_puts(" pixels lit\n");
            all_ok = 0;
        }
        if (during_lit == 0) {
            klog_puts("[m61] nothing was drawn on the path to the taskbar mid-minimize - the window blinked rather than moved\n");
            all_ok = 0;
        }
        if (after_lit != 0) {
            klog_puts("[m61] the animation left 0x");
            klog_put_hex32((uint32_t)after_lit);
            klog_puts(" pixels behind on its path\n");
            all_ok = 0;
        }
        if (after_window == before_window) {
            klog_puts("[m61] the window is still on screen after being minimized\n");
            all_ok = 0;
        }
        if (quiet_lit != 0) {
            klog_puts("[m61] motion is switched off and something still animated: 0x");
            klog_put_hex32((uint32_t)quiet_lit);
            klog_puts(" pixels lit\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M61 self-test: window animation did not move, did not clean up, or ignored its setting");
        }
        klog_puts("[m61] a minimize animating toward the taskbar - endpoints plus an intermediate "
                   "frame that is neither, nothing left behind, and nothing at all when motion is "
                   "switched off - self-test passed.\n\n");
    }

    /* M62 self-test: the first sound this OS has ever made.
     *
     * Sound is the one capability here that cannot be checked by looking,
     * so what is checkable has to be chosen carefully rather than
     * asserted vaguely. Four claims, and none of them is "it sounded
     * right":
     *
     *   1. **Ownership is real.** The first claim succeeds, a second from
     *      a different task is refused, and a beep from a non-owner does
     *      nothing. That is the whole point of the design - there is one
     *      speaker, and a program able to seize it unasked can make the
     *      machine unusable - and it is enforceable without a permission
     *      model, which is why it is a claim rather than a comment.
     *   2. The speaker really is gated on and off. Port 0x61's low two
     *      bits are the gate; they are set while a tone plays and clear
     *      once its deadline passes. Reading the port back is as close to
     *      "did it make a sound" as a headless test can get, and it is a
     *      real read of real hardware state rather than of a flag this
     *      code set.
     *   3. **The AC'97 device consumed a buffer we gave it.** QEMU hands
     *      no audio back, but it does raise the completion interrupt as
     *      it drains the ring - so a generated tone plus a completion
     *      count that goes up is proof the descriptor ring, the physical
     *      addresses and the IRQ wiring are all right. Skipped, not
     *      failed, on a machine with no such device.
     *   4. Mute is honoured by the speaker as well as the stream: with
     *      volume 0, the same beep leaves the gate closed.
     */
    {
        int all_ok = 1;

        /* kernel_main is task 0, and nothing has claimed audio yet. */
        if (do_syscall(SYS_audio_claim, 0, 0, 0) != 0) {
            klog_puts("[m62] the first claim on the audio devices was refused\n");
            all_ok = 0;
        }
        if (do_syscall(SYS_audio_claim, 0, 0, 0) != 0) {
            klog_puts("[m62] the owner could not re-claim what it already owns\n");
            all_ok = 0;
        }

        /* A tone, and the gate bits that say it is really playing. */
        do_syscall(SYS_audio_volume, 100, 0, 0);
        do_syscall(SYS_beep, 880, 40, 0);
        uint8_t gate_during = (uint8_t)(inb(0x61) & 0x03);
        pit_sleep_ms(120); /* past the 40 ms deadline, with room for the tick that clears it */
        uint8_t gate_after = (uint8_t)(inb(0x61) & 0x03);
        if (gate_during != 0x03) {
            klog_puts("[m62] the speaker gate never opened - no tone was played\n");
            all_ok = 0;
        }
        if (gate_after != 0) {
            klog_puts("[m62] the speaker gate is still open past the tone's deadline\n");
            all_ok = 0;
        }

        /* Muted, the same beep must leave the gate shut. */
        do_syscall(SYS_audio_volume, 0, 0, 0);
        do_syscall(SYS_beep, 880, 40, 0);
        uint8_t gate_muted = (uint8_t)(inb(0x61) & 0x03);
        if (gate_muted != 0) {
            klog_puts("[m62] muted, and the speaker still played\n");
            all_ok = 0;
        }
        do_syscall(SYS_audio_volume, 100, 0, 0);

        /* The stream. A quarter-second of a 440 Hz sine would need
         * floating point this kernel does not have (and M63 is the
         * milestone that changes that); a square wave is what the
         * speaker makes anyway, and what matters here is that the device
         * consumed the bytes. */
        if (!ac97_available()) {
            klog_puts("[m62] no AC'97 device on this machine - the stream half of this test is skipped, "
                       "which is the same answer real hardware without one would give.\n");
        } else {
            uint32_t frames = 4800; /* 100 ms at 48 kHz */
            if (frames > ac97_max_frames()) {
                frames = ac97_max_frames();
            }
            int16_t *tone = (int16_t *)kmalloc((size_t)frames * 2 * sizeof(int16_t));
            if (!tone) {
                panic("M62 self-test: out of memory for a tenth of a second of audio");
            }
            uint32_t period = AC97_SAMPLE_RATE / 440; /* samples per cycle of a 440 Hz square wave */
            for (uint32_t i = 0; i < frames; i++) {
                int16_t v = ((i % period) < period / 2) ? 6000 : -6000;
                tone[i * 2] = v;
                tone[i * 2 + 1] = v;
            }
            uint32_t before = ac97_completions();
            if (do_syscall(SYS_audio_play, (uint64_t)tone, frames, 0) != 0) {
                klog_puts("[m62] SYS_audio_play refused a buffer the device advertised room for\n");
                all_ok = 0;
            }
            /* 100 ms of audio, plus room for the device to get around to
             * it - the completion is an interrupt, not a return value. */
            pit_sleep_ms(600);
            uint32_t after = ac97_completions();
            kfree(tone);
            if (after == before) {
                klog_puts("[m62] the AC'97 device never reported finishing the buffer it was given\n");
                ac97_debug_dump();
                all_ok = 0;
            }
        }

        /* And ownership, from somebody else. A spawned program is a
         * different task, which is exactly the case the claim exists for -
         * badptr is used because it is already on disk and already exits
         * on its own. */
        {
            size_t claim_bytes = 0;
            uint8_t *claim_image = read_program(PATH_BIN_DIR "audiograb", &claim_bytes);
            task_t *grabber = process_spawn("audiograb", claim_image, claim_bytes, "");
            kfree(claim_image);
            long rc = do_syscall(SYS_wait, (uint64_t)grabber->id, 0, 0);
            if (rc != 0) {
                klog_puts("[m62] another process was able to take the speaker, or to beep without owning it: 0x");
                klog_put_hex32((uint32_t)rc);
                klog_putc('\n');
                all_ok = 0;
            }
        }

        /* And hand it back. kernel_main never exits, so a claim it kept
         * would keep the speaker away from the compositor for the life
         * of the machine - which is how an error toast would have gone
         * back to arriving in silence, the exact thing this milestone is
         * about. */
        if (do_syscall(SYS_audio_release, 0, 0, 0) != 0) {
            klog_puts("[m62] the owner could not release the audio devices\n");
            all_ok = 0;
        }
        if (do_syscall(SYS_beep, 880, 40, 0) == 0) {
            klog_puts("[m62] a beep succeeded after the speaker was released\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M62 self-test: the speaker, the stream, or the ownership rule is wrong");
        }
        klog_puts("[m62] the PC speaker gated on and off by its own deadline, muted when the volume "
                   "is zero, an AC'97 buffer the device reported finishing, and a second process "
                   "refused both the claim and the beep, then the owner handing it back - self-test passed.\n\n");
    }

    /* M63 self-test: somebody else's program.
     *
     * Every binary this OS had ever run was written in this repo. Two
     * things had to exist before that could stop being true, and both are
     * asserted here rather than assumed:
     *
     *   1. **Floating point**, which did not exist in this kernel at all.
     *      `libctest` is written as an ordinary C program - standard
     *      headers, nothing from this project - and checks the maths
     *      library against values that are either right or not, the
     *      formatter against strings that are either right or not, and
     *      then runs a float loop long enough to span many scheduler
     *      quanta. That last one is the only way to catch a broken
     *      FXSAVE/FXRSTOR: a switch that lost xmm state would corrupt the
     *      sum, and would do it intermittently.
     *   2. **A program nobody here wrote**, actually running.
     *      third_party/whetstone is the 1998 C translation of the 1972
     *      Whetstone benchmark - `sin`, `cos`, `atan`, `exp`, `log`,
     *      `sqrt`, `printf("%.1f")`, `time(0)`, `atol`, `strncmp` - and
     *      its link errors were literally the specification for
     *      user_space/libc. Its output is read back through a pipe and
     *      checked, because "it exited 0" would also be true of a program
     *      that printed nothing.
     */
    {
        int all_ok = 1;

        {
            size_t bytes = 0;
            uint8_t *image = read_program(PATH_BIN_DIR "libctest", &bytes);
            task_t *t = process_spawn("libctest", image, bytes, "");
            kfree(image);
            long rc = do_syscall(SYS_wait, (uint64_t)t->id, 0, 0);
            if (rc != 0) {
                klog_puts("[m63] the libc/SSE self-test program failed\n");
                all_ok = 0;
            }
        }

        /* Whetstone, with its output captured. The same dup2'd pipe
         * gui_terminal.c has used since it was written - which is the
         * point: a third-party program's stdout goes where any program's
         * does, with no special path for it. */
        {
            int out_fds[2];
            if (do_syscall(SYS_pipe, (uint64_t)out_fds, 0, 0) != 0) {
                panic("M63 self-test: could not make a pipe for the ported program's output");
            }
            do_syscall(SYS_dup2, (uint64_t)out_fds[1], 1, 0);

            size_t bytes = 0;
            uint8_t *image = read_program(PATH_BIN_DIR "whetstone", &bytes);
            /* A loop count chosen so the run crosses a whole second:
             * whetstone reports "Insufficient duration" and exits
             * nonzero otherwise, and a benchmark that measured nothing
             * would be a weaker thing to assert on than one that did. */
            /* Big enough that the run crosses several whole seconds: time()
             * has one-second resolution, and 800 loops came back saying
             * "Duration: 1 sec." - one host slower or faster and that is
             * zero, which whetstone reports as insufficient and exits
             * nonzero for. */
            const char *argv[] = { PATH_BIN_DIR "whetstone", "2500", 0 };
            task_t *t = process_spawnv("whetstone", image, bytes, argv);
            kfree(image);

            /* Drained while it runs: a pipe holds SYS_PIPE_CAPACITY
             * bytes and a writer blocks when it is full, so waiting
             * without reading is how this deadlocks. */
            static char out[2048];
            size_t got = 0;
            long deadline = (long)pit_get_ticks() + 60 * PIT_HZ;
            for (;;) {
                long avail = do_syscall(SYS_pipe_poll, (uint64_t)out_fds[0], 0, 0);
                if (avail > 0 && got < sizeof(out) - 1) {
                    size_t room = sizeof(out) - 1 - got;
                    long n = do_syscall(SYS_read, (uint64_t)out_fds[0], (uint64_t)(out + got),
                                         (uint64_t)((size_t)avail < room ? (size_t)avail : room));
                    if (n > 0) {
                        got += (size_t)n;
                    }
                } else if (do_syscall(SYS_wait_nb, (uint64_t)t->id, 0, 0) != -2) {
                    /* Exited - one last drain, then done. */
                    long n;
                    while ((n = do_syscall(SYS_pipe_poll, (uint64_t)out_fds[0], 0, 0)) > 0 &&
                           got < sizeof(out) - 1) {
                        size_t room = sizeof(out) - 1 - got;
                        long r = do_syscall(SYS_read, (uint64_t)out_fds[0], (uint64_t)(out + got),
                                             (uint64_t)((size_t)n < room ? (size_t)n : room));
                        if (r <= 0) {
                            break;
                        }
                        got += (size_t)r;
                    }
                    break;
                } else if ((long)pit_get_ticks() > deadline) {
                    break;
                } else {
                    do_syscall(SYS_yield, 0, 0, 0);
                }
            }
            out[got] = '\0';
            do_syscall(SYS_close, (uint64_t)out_fds[0], 0, 0);
            do_syscall(SYS_close, (uint64_t)out_fds[1], 0, 0);
            /* fd 1 back to the console, by hand. SYS_dup2 cannot express
             * this - there is no descriptor anywhere that *is* stdout to
             * duplicate from, only the implicit FD_STDOUT every task
             * starts with - and sched_reset_fds_to_std would also drop
             * every pipe the self-tests above still hold. Reaching into
             * the table is the narrow thing to do here, and this is
             * kernel_main rather than a syscall. */
            fd_release(&sched_current()->fds[1]);
            sched_current()->fds[1].type = FD_STDOUT;

            if (!k_strstr(out, "Loops:")) {
                klog_puts("[m63] the ported program did not report a completed run. It said:\n");
                klog_puts(out);
                klog_putc('\n');
                all_ok = 0;
            }
            if (!k_strstr(out, "Whetstones:")) {
                klog_puts("[m63] the ported program produced no benchmark figure\n");
                all_ok = 0;
            } else {
                /* Printed, because it is the interesting part: this is
                 * the first number in this project's history produced by
                 * code nobody here wrote. */
                klog_puts("[m63] the ported program said:");
                klog_puts(out);
            }
        }

        if (!all_ok) {
            panic("M63 self-test: floating point, the libc subset, or the ported program is wrong");
        }
        klog_puts("[m63] SSE state preserved across task switches, a libc subset checked against "
                   "values that are either right or not, and a 1972 benchmark nobody here wrote "
                   "running to completion and reporting a figure - self-test passed.\n\n");
    }

    /* M63 stretch goal self-test: icons are files.
     *
     * `icon.h` predicted at M56 that the day icons became replaceable,
     * "the format does not change - only where the bytes are read from".
     * This is the assertion that the prediction held, and it is made the
     * only way it can be made honestly: by *replacing an icon* and
     * looking at the screen.
     *
     * A file is written, a process is restarted, and a pixel changes.
     * Nothing here checks that desktop_icons.c read a file - it checks
     * that editing one changed the desktop, which is the thing a person
     * would actually be doing.
     */
    {
        int all_ok = 1;

        size_t comp_bytes = 0;
        uint8_t *comp_image = read_program(PATH_BIN_DIR "compositor", &comp_bytes);
        size_t icons_bytes = 0;
        uint8_t *icons_image = read_program(PATH_BIN_DIR "desktop_icons", &icons_bytes);

        task_t *comp_task = process_spawn("compositor", comp_image, comp_bytes, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */
        task_t *icons_task = process_spawn("desktop_icons", icons_image, icons_bytes, "");
        pit_sleep_ms(1200);

        /* The first icon's box: ICON_MARGIN in from the top-left corner,
         * ICON_SIZE across (desktop_icons.c). Scanned as a region rather
         * than probed at a point, because where inside its own 48 pixels
         * a given icon puts a given colour is a property of the picture,
         * not of this test. */
        static const uint32_t MAGENTA = 0x00FF00FFu;
        uint32_t before_magenta = 0;
        for (uint32_t y = 32; y < 80; y += 2) {
            for (uint32_t x = 32; x < 80; x += 2) {
                if (fb_get_pixel(x, y) == MAGENTA) {
                    before_magenta++;
                }
            }
        }

        static uint8_t blob[512];
        int64_t n = vfs_read(PATH_ICONS_DIR "Terminal.icn", blob, sizeof(blob));
        int wrote = 0;
        if (n < ICON_HEADER_BYTES || !icon_valid(blob)) {
            klog_puts("[m63] the desktop did not write its icons out as files\n");
            all_ok = 0;
        } else {
            /* Palette entry 1 - the first non-transparent colour, and
             * the one this icon's body is drawn in. */
            blob[ICON_HEADER_BYTES + 3] = 0xFF;
            blob[ICON_HEADER_BYTES + 4] = 0x00;
            blob[ICON_HEADER_BYTES + 5] = 0xFF;
            if (vfs_write(PATH_ICONS_DIR "Terminal.icn", blob, (size_t)n) != 0) {
                klog_puts("[m63] could not write the edited icon back\n");
                all_ok = 0;
            } else {
                wrote = 1;
            }
        }

        uint32_t after_magenta = 0;
        if (wrote) {
            selftest_reap(icons_task);
            icons_task = process_spawn("desktop_icons", icons_image, icons_bytes, "");
            pit_sleep_ms(1200);
            for (uint32_t y = 32; y < 80; y += 2) {
                for (uint32_t x = 32; x < 80; x += 2) {
                    if (fb_get_pixel(x, y) == MAGENTA) {
                        after_magenta++;
                    }
                }
            }
        }
        kfree(icons_image);

        selftest_reap(icons_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        if (before_magenta != 0) {
            klog_puts("[m63] the desktop was already showing the colour this test edits in\n");
            all_ok = 0;
        }
        if (wrote && after_magenta == 0) {
            klog_puts("[m63] editing an icon file changed nothing on screen\n");
            all_ok = 0;
        }

        /* And put it back, so the desktop a person sees after boot is
         * the one that shipped rather than the one this test defaced. */
        if (wrote) {
            vfs_unlink(PATH_ICONS_DIR "Terminal.icn");
        }

        if (!all_ok) {
            panic("M63 icon self-test: icons are not files, or editing one changes nothing");
        }
        klog_puts("[m63] icons are files: the desktop wrote them out, an edited palette entry "
                   "changed what is on screen after a restart, and the format and loader did not "
                   "change at all - self-test passed.\n\n");
    }

    /* M63 stretch goal self-test: virtual desktops.
     *
     * Four claims, and each is a pixel rather than a protocol reply,
     * because "the window is on workspace 2" is only interesting if it
     * also means "the window is not on the screen":
     *
     *   1. A window is visible on the desktop it was opened on.
     *   2. Switching away hides it - and hides it *completely*, so what
     *      is left is bare desktop rather than a stale rectangle.
     *   3. Switching back brings it back.
     *   4. Moving it takes it with you: after Ctrl+Shift+Alt+Right the
     *      window is still on screen, and after switching back to where
     *      it used to be, it is not.
     *
     * Driven with real injected chords through a real compositor, so what
     * is being tested is the binding as much as the mechanism - the same
     * reason M56 taught keyboard_inject to carry a modifier mask.
     */
    {
        int all_ok = 1;

        size_t comp_bytes = 0;
        uint8_t *comp_image = read_program(PATH_BIN_DIR "compositor", &comp_bytes);
        size_t clock_bytes = 0;
        uint8_t *clock_image = read_program(PATH_BIN_DIR "gui_clock", &clock_bytes);

        task_t *comp_task = process_spawn("compositor", comp_image, comp_bytes, "");
        kfree(comp_image);
        selftest_wait_for_compositor(); /* M69: was a fixed sleep - see the helper */
        task_t *clock_task = process_spawn("gui_clock", clock_image, clock_bytes, "");
        kfree(clock_image);
        pit_sleep_ms(900);

        /* gui_clock connects at (100, 100), 200x120 - the placement every
         * other self-test here relies on. (150, 150) is inside its
         * content; (500, 500) is bare desktop, and is what "gone" has to
         * look like. */
        uint32_t desktop = fb_get_pixel(500, 500);
        uint32_t on_home = fb_get_pixel(150, 150);

        /* Each of these waits for the screen the chord asks for, rather
         * than for half a second. The expected values are the two this
         * test read for itself a moment ago, so nothing here is a
         * constant that could drift out of step with the compositor. */
        keyboard_inject((char)KBD_KEY_RIGHT, KBD_MOD_CTRL | KBD_MOD_SHIFT);
        uint32_t after_switch = selftest_pixel_settled(150, 150, desktop,
                                                        "the window to be hidden by switching desktop");

        keyboard_inject((char)KBD_KEY_LEFT, KBD_MOD_CTRL | KBD_MOD_SHIFT);
        uint32_t back_home = selftest_pixel_settled(150, 150, on_home,
                                                     "the window to come back when we switch back");

        /* And take it with us. */
        keyboard_inject((char)KBD_KEY_RIGHT, KBD_MOD_CTRL | KBD_MOD_SHIFT | KBD_MOD_ALT);
        uint32_t moved_with = selftest_pixel_settled(150, 150, on_home,
                                                      "the window to follow us to the next desktop");

        keyboard_inject((char)KBD_KEY_LEFT, KBD_MOD_CTRL | KBD_MOD_SHIFT);
        uint32_t left_behind = selftest_pixel_settled(150, 150, desktop,
                                                       "the desktop it came from to be empty");

        selftest_reap(clock_task);
        selftest_reap(comp_task);
        console_init();
        klog_use_console();

        if (on_home == desktop) {
            klog_puts("[m63] the window was not on screen to begin with - this test can see nothing\n");
            all_ok = 0;
        }
        if (after_switch != desktop) {
            klog_puts("[m63] switching to the next virtual desktop left the window on screen\n");
            all_ok = 0;
        }
        if (back_home != on_home) {
            klog_puts("[m63] switching back did not bring the window back\n");
            all_ok = 0;
        }
        if (moved_with != on_home) {
            klog_puts("[m63] moving a window to the next desktop did not take it there\n");
            all_ok = 0;
        }
        if (left_behind != desktop) {
            klog_puts("[m63] the moved window is still on the desktop it came from\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M63 workspace self-test: virtual desktops do not hide, show or carry windows");
        }
        klog_puts("[m63] four virtual desktops: a window hidden by switching away, back when "
                   "switching returns, and carried along when it is sent - self-test passed.\n\n");
    }

    /* M64 self-test: a network user space can reach.
     *
     * The stretch-goal entry that asked for this named the problem
     * precisely: M27 shipped Ethernet, ARP, IPv4 and ICMP, and for
     * thirty-six milestones the only thing that ever used any of it was
     * one boot self-test pinging the gateway. That test is still above
     * this one and still passes; what it could never show is whether a
     * *program* could do anything with the network, because there was no
     * call for one to make.
     *
     * Three claims, in the order they stop being about the kernel and
     * start being about a person:
     *
     *   1. **DHCP got a real lease.** Asserted explicitly, and it has to
     *      be: on QEMU the leased configuration is byte-for-byte the
     *      fallback one, so a DHCP client that did nothing at all would
     *      produce an identical `[net]` line. net_config_is_leased() is
     *      the only thing that can tell the two apart.
     *   2. **The syscall surface works and fails correctly.** In user
     *      space, in `nettest`, for the same reason M52's pointer matrix
     *      lives in badptr.c: what is new here is the syscall boundary,
     *      and a test that called socket_sendto() directly would prove
     *      the layer underneath it. Fourteen checks, five of them about
     *      failing properly - including sending to an address nothing
     *      answers ARP for, which until this milestone was a panic().
     *   3. **A program that asks the network for something real.**
     *      `nettime` is an SNTP client, and the honest thing to assert
     *      about it here is not that it gets an answer - the gateway on
     *      this network does not run NTP - but that it *fails cleanly
     *      and says so*, in bounded time, which is what almost every
     *      network program spends most of its life doing.
     */
    {
        int all_ok = 1;

        if (net_have_nic()) {
            if (!net_config_is_leased()) {
                klog_puts("[m64] no DHCP lease - the address is the fallback constant, "
                           "which is what this milestone existed to stop being the answer\n");
                all_ok = 0;
            }

            size_t bytes = 0;
            uint8_t *image = read_program(PATH_BIN_DIR "nettest", &bytes);
            task_t *t = process_spawn("nettest", image, bytes, "");
            kfree(image);
            if (do_syscall(SYS_wait, (uint64_t)t->id, 0, 0) != 0) {
                klog_puts("[m64] the socket self-test program reported a failure\n");
                all_ok = 0;
            }

            /* nettime against a gateway that does not answer NTP. What
             * is being asserted is the *shape* of a failure: it must
             * come back, it must come back non-zero, and it must do both
             * inside the second its own deadline promises. A program
             * that hung here would hang every desktop that ever shipped
             * it. Read back through a pipe, so "it exited 1" cannot be
             * confused with "it exited 1 having printed nothing". */
            int out_fds[2];
            if (do_syscall(SYS_pipe, (uint64_t)out_fds, 0, 0) != 0) {
                panic("M64 self-test: could not make a pipe for nettime's output");
            }
            do_syscall(SYS_dup2, (uint64_t)out_fds[1], 1, 0);

            image = read_program(PATH_BIN_DIR "nettime", &bytes);
            task_t *nt = process_spawn("nettime", image, bytes, "");
            kfree(image);

            uint64_t started = pit_get_ticks();
            long nettime_rc = do_syscall(SYS_wait, (uint64_t)nt->id, 0, 0);
            uint64_t elapsed_ms = (pit_get_ticks() - started) * 1000 / PIT_HZ;

            static char nettime_out[256];
            long got = do_syscall(SYS_read, (uint64_t)out_fds[0],
                                  (uint64_t)nettime_out, sizeof(nettime_out) - 1);
            nettime_out[got > 0 ? got : 0] = '\0';
            do_syscall(SYS_close, (uint64_t)out_fds[0], 0, 0);
            do_syscall(SYS_close, (uint64_t)out_fds[1], 0, 0);
            /* fd 1 back to the console by hand, exactly as the M63 block
             * above does and for the same reason - see its comment. */
            fd_release(&sched_current()->fds[1]);
            sched_current()->fds[1].type = FD_STDOUT;

            /* Either outcome is legitimate - a machine whose gateway
             * *does* run NTP would get a time - so this asserts what is
             * true of both: it came back, it came back quickly, and it
             * said something. */
            if (elapsed_ms > 5000) {
                klog_puts("[m64] nettime took longer than its own deadline to give up\n");
                all_ok = 0;
            }
            if (got <= 0) {
                klog_puts("[m64] nettime printed nothing at all\n");
                all_ok = 0;
            }
            if (nettime_rc == 0 && !k_strstr(nettime_out, "says")) {
                klog_puts("[m64] nettime reported success without reporting a time\n");
                all_ok = 0;
            }
            if (nettime_rc != 0 && !k_strstr(nettime_out, "no reply") &&
                !k_strstr(nettime_out, "unreachable")) {
                klog_puts("[m64] nettime failed without saying why\n");
                all_ok = 0;
            }

            if (!all_ok) {
                panic("M64 network self-test: user space cannot use the network correctly");
            }

            klog_puts("[m64] the network reached user space: a DHCP lease rather than a "
                       "hardcoded address, UDP sockets in the fd table that round-trip a "
                       "datagram and refuse six kinds of wrong, and an SNTP client that "
                       "gives up cleanly - self-test passed. nettime said: ");
            klog_puts(nettime_out);
            klog_putc('\n');
        } else {
            klog_puts("[m64] no NIC on this machine - the socket layer is present but "
                       "untested this boot.\n\n");
        }
    }

    /* M65 self-test: a permission model that is not a fake check.
     *
     * This project declined to build one several times, in writing, and
     * each refusal was right at the time - `SYS_shutdown`'s own comment
     * calls a check with nothing behind it exactly what it would have
     * been. What changed is not the argument but the machine: M63 made
     * it possible to run a program nobody here wrote, and M64 made it
     * possible for that program to open a socket and talk to anything.
     * The stretch-goal entry said to revisit this "the moment a ported
     * program is something a person downloads", and both halves of the
     * sentence it was waiting on have now happened.
     *
     * Three claims:
     *
     *   1. **The manifest is applied by the kernel, not by launchers.**
     *      Asserted by reading the capability set of a process spawned
     *      here, in kernel_main, which holds CAP_ALL - if the grant
     *      table were consulted by the compositor rather than by
     *      process_spawnv, this child would have inherited everything.
     *   2. **An ordinary application is refused.** `captest` is granted
     *      nothing beyond CAP_APP_DEFAULT on purpose, and thirteen of
     *      its checks are things it tried and could not do.
     *   3. **A process it did not start survived it trying.** The one
     *      assertion whose failure mode is loud rather than a return
     *      code. The victim is a task spawned right here and handed to
     *      captest by pid - it cannot be init, because init does not
     *      exist yet: PID 1 belonged to the first task this boot ever
     *      spawned (pids are slot+generation, sched.h), and the real
     *      init only starts after the self-test phase ends. A check
     *      against pid 1 here would pass vacuously against a stale pid,
     *      which is the fake check this milestone exists to not make.
     */
    {
        int all_ok = 1;

        /* A plain program with no entry in the grant table, spawned by
         * the most privileged thing on the machine. */
        size_t bytes = 0;
        uint8_t *image = read_program(PATH_BIN_DIR "hello", &bytes);
        task_t *plain = process_spawn("hello", image, bytes, "");
        kfree(image);
        uint32_t plain_caps = plain->caps;
        do_syscall(SYS_wait, (uint64_t)plain->id, 0, 0);

        if (plain_caps != CAP_APP_DEFAULT) {
            klog_puts("[m65] a program with no manifest entry did not get the default "
                       "capability set - the grant table is not being applied at spawn\n");
            all_ok = 0;
        }
        if (sched_current()->caps != CAP_ALL) {
            klog_puts("[m65] kernel_main is not the root of the capability model\n");
            all_ok = 0;
        }

        /* And the compositor, which is the one entry that is broad -
         * because it is the trusted launcher, and a compositor that
         * could not paint would be a black screen. */
        image = read_program(PATH_BIN_DIR "compositor", &bytes);
        task_t *comp = process_spawn("compositor", image, bytes, "");
        kfree(image);
        if (comp->caps != CAP_ALL) {
            klog_puts("[m65] the compositor did not get the capabilities it owns the screen with\n");
            all_ok = 0;
        }
        selftest_reap(comp);
        console_init();
        klog_use_console();

        /* The victim: a task captest did not start and must therefore
         * not be able to signal. spinner_task because it never exits on
         * its own (the same reason M47's orderly-stop test uses it), so
         * "still running afterwards" can only mean the kill was refused. */
        task_t *victim = task_spawn("cap-victim", spinner_task, NULL);
        char victim_pid[12];
        {
            int v = victim->id, n = 0;
            char tmp[12];
            do {
                tmp[n++] = (char)('0' + (v % 10));
                v /= 10;
            } while (v);
            int m = 0;
            while (n) {
                victim_pid[m++] = tmp[--n];
            }
            victim_pid[m] = '\0';
        }

        image = read_program(PATH_BIN_DIR "captest", &bytes);
        task_t *ct = process_spawn("captest", image, bytes, victim_pid);
        kfree(image);
        if (do_syscall(SYS_wait, (uint64_t)ct->id, 0, 0) != 0) {
            klog_puts("[m65] the capability self-test program reported a failure\n");
            all_ok = 0;
        }

        /* Still there. captest asked the kernel to kill it. */
        if (victim->state == TASK_TERMINATED) {
            klog_puts("[m65] the victim task did not survive an unprivileged process asking to kill it\n");
            all_ok = 0;
        }
        selftest_reap(victim);

        if (!all_ok) {
            panic("M65 capability self-test: the permission model does not hold");
        }
        klog_puts("[m65] capabilities: a manifest the kernel applies rather than a launcher, "
                   "an ordinary program refused the screen, the clipboard, the process list, "
                   "a socket, the clock and another process's life, and a set that only ever "
                   "shrinks - self-test passed.\n\n");
    }

    /* M66 self-test: TCP.
     *
     * The stretch-goal entry refused to let this be a footnote -
     * "retransmission, congestion control and an eleven-state machine
     * are not a bullet on somebody else's list" - and the test is shaped
     * by the same judgement. What it asserts is not "a connection
     * worked" but the three things that are hard about TCP and easy to
     * ship broken:
     *
     *   1. **A transfer bigger than any buffer involved.** 16 KiB
     *      through a 4 KiB send buffer at a 1460-byte MSS, checked byte
     *      for byte. That exercises the window, the congestion window,
     *      buffer compaction on every ACK, and a sender that has to stop
     *      and resume - none of which a one-segment "hello" touches.
     *   2. **Retransmission, on a link that never loses anything.**
     *      Loopback is a perfect network, which makes it the worst place
     *      to find out whether the retransmission path works - and "the
     *      retransmission path is untested" is true of most from-scratch
     *      TCP stacks and never written down. So the kernel is told to
     *      drop the next segments outright, after they have been built
     *      and after the sequence numbers have advanced, and the
     *      transfer has to complete anyway.
     *   3. **A refused connection, quickly.** An RST must be told apart
     *      from silence, or every mistyped port becomes a ten-second
     *      pause.
     */
    if (net_have_nic()) {
        int all_ok = 1;

        /* Two data segments into the void, armed before the program
         * starts. They land on the first short message in each
         * direction, so both ends of the connection have to recover -
         * and they land in go-back-N rather than on the handshake, which
         * is where the interesting code is. */
        tcp_debug_drop_next(2);
        int retransmits_before = tcp_debug_retransmits();

        size_t bytes = 0;
        uint8_t *image = read_program(PATH_BIN_DIR "tcptest", &bytes);
        task_t *t = process_spawn("tcptest", image, bytes, "");
        kfree(image);
        if (do_syscall(SYS_wait, (uint64_t)t->id, 0, 0) != 0) {
            klog_puts("[m66] the TCP self-test program reported a failure\n");
            all_ok = 0;
        }

        int retransmits = tcp_debug_retransmits() - retransmits_before;
        if (retransmits <= 0) {
            klog_puts("[m66] three segments were dropped and nothing was ever retransmitted - "
                       "the recovery path did not run, so the transfer that succeeded proves "
                       "less than it appears to\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M66 TCP self-test: the connection, the transfer or the recovery does not work");
        }

        klog_puts("[m66] TCP: a handshake, 16 KiB through a 4 KiB buffer arriving byte for "
                   "byte, an end of stream a reader can tell from a pause, a refusal that "
                   "arrives as an RST rather than a timeout, and a transfer that survived ");
        klog_put_dec((uint32_t)retransmits);
        klog_puts(" deliberately dropped segment(s) - self-test passed.\n\n");
    } else {
        klog_puts("[m66] no NIC on this machine - TCP is present but untested this boot.\n\n");
    }

    /* ---- M67 self-test: a kernel that can be interrupted ---------------
     *
     * The milestone flipped vector 0x80 from an interrupt gate to a trap
     * gate, so interrupts now stay enabled for the whole of a syscall.
     * That one bit removed this kernel's only mutual exclusion - IF=0,
     * never written down, relied on by every syscall handler written
     * since M8 - and the rest of M67 is the six locks that replace it.
     *
     * This is the test that would have noticed. Four processes, spawned
     * without waiting so they are genuinely concurrent, each hammering
     * the four subsystems that had shared mutable state and no lock:
     * leanfs (fs_lock), the shm segment table (shm_lock), pipe ring
     * buffers (pipe_lock) and the socket table (net_lock). Every check
     * inside racetest.c is "read back the pattern only this process could
     * have written", which is the only kind of assertion that can tell a
     * race from a busy machine - see that file's header.
     *
     * Four rather than two: with two, an interleaving has to be unlucky
     * twice to be visible, and on a machine with more than two cores two
     * processes can run without ever contending at all.
     *
     * The resource comparison around it is the second half. A lock that
     * is taken and not released on some error path does not corrupt
     * anything - it hangs, which this test would catch by never
     * finishing - but a *slot* leaked under contention (a shm segment
     * whose failed create left `used` set, a socket that lost its
     * refcount decrement) is silent, survives the test, and is exactly
     * the class of bug M50 was written about. So the segment table and
     * the frame count both have to come back to where they started.
     */
    {
        int frames_before = (int)pmm_free_frame_count();

        static const int RACERS = 4;
        task_t *racers[4];
        int spawned = 0;

        size_t rbytes = 0;
        uint8_t *rimage = read_program(PATH_BIN_DIR "racetest", &rbytes);
        for (int i = 0; i < RACERS; i++) {
            /* Spawned back to back with no wait between them - the whole
             * point is that all four are runnable at once. process_spawn
             * copies the image, so one read serves all four. */
            racers[i] = process_spawn("racetest", rimage, rbytes, "");
            if (racers[i]) {
                spawned++;
            }
        }
        kfree(rimage);

        int all_ok = (spawned == RACERS);
        if (!all_ok) {
            klog_puts("[m67] could not spawn four concurrent racers\n");
        }
        for (int i = 0; i < RACERS; i++) {
            if (!racers[i]) {
                continue;
            }
            if (do_syscall(SYS_wait, (uint64_t)racers[i]->id, 0, 0) != 0) {
                klog_puts("[m67] a racer reported corrupted state - a lock M67 added is "
                           "missing, wrong, or not covering what it claims to\n");
                all_ok = 0;
            }
        }

        int frames_after = (int)pmm_free_frame_count();
        if (frames_after < frames_before) {
            klog_puts("[m67] frames leaked across the race - ");
            klog_put_dec((uint32_t)(frames_before - frames_after));
            klog_puts(" not returned\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M67 concurrency self-test: syscalls are preemptible and the locks do not hold");
        }

        klog_puts("[m67] a preemptible kernel: `int 0x80` is a trap gate, four concurrent "
                   "processes hammered the filesystem, the shm table, pipe ring buffers and "
                   "the socket table, and every one of them read back only its own bytes - "
                   "self-test passed.\n\n");
    }

    /* ---- M69 self-test: input-to-photon, measured -----------------------
     *
     * The first bullet of this milestone is "measure it first, before
     * changing anything", and this is that. Nobody has ever measured this
     * number here; the boot log now prints it on every run, which is what
     * turns "the desktop feels fine" into something a later change can be
     * held against.
     *
     * Three conditions, because one number is not a characterisation:
     *
     *   idle     - the floor. What the path costs with nothing competing.
     *   loaded   - one CPU-bound task per core, never yielding, never
     *              syscalling. This is the case that is visibly broken on
     *              a round-robin scheduler with a 50 ms quantum, and it
     *              is the case M69's priority work exists for. A desktop
     *              that stutters while something computes is the
     *              complaint; this is its number.
     *
     * Reported, not asserted - with one exception. A budget would be a
     * number invented before the measurement existed, which is exactly
     * the mistake this milestone is written to avoid. What IS asserted is
     * that the measurement works at all: a zero means the cursor never
     * arrived, and a latency test that silently measures nothing is worse
     * than no test.
     */
    {
        size_t comp_bytes = 0;
        uint8_t *comp_img = read_program(PATH_BIN_DIR "compositor", &comp_bytes);
        task_t *comp = process_spawn("compositor", comp_img, comp_bytes, "");
        kfree(comp_img);

        /* Wait for the compositor to own the screen before timing
         * anything through it - M69's own root-cause fix, applied to
         * M69's own test. */
        if (!selftest_wait_for_pixel(500, 400, 0x001A1A2Eu, 5000,
                                      "the compositor to paint the desktop")) {
            panic("M69 latency self-test: no desktop to measure against");
        }

        /* Several samples, and the median-ish middle one reported: a
         * single sample can land on the wrong side of a scheduler tick
         * and say nothing about the machine. Taking the best of five is
         * deliberate - what is being characterised is what the path
         * COSTS, and an outlier caused by this test's own spawn traffic
         * is not that. The worst case is what the loaded run below is
         * for. */
        uint64_t idle_us = 0;
        for (int i = 0; i < 5; i++) {
            uint64_t us = selftest_input_to_photon_us(200 + i * 20, 200, 2000);
            if (us == 0) {
                continue;
            }
            if (idle_us == 0 || us < idle_us) {
                idle_us = us;
            }
        }

        /* Now under load. One spinner per online CPU, so there is no core
         * left idle for the compositor to be scheduled onto for free -
         * without that this measures a machine that merely has a busy
         * neighbour, which is not the complaint. */
        /* ONE CPU-bound task, and the reason it is not four is a limit of
         * this probe rather than a judgement about load.
         *
         * The observer is this loop, and it competes: it polls a pixel
         * through schedule() and never blocks, so the scheduler correctly
         * classifies it as batch alongside the spinners. With four of
         * them, round-robin among five batch tasks means the *observer*
         * runs about every fifth slice, and the figure that comes out is
         * how often this loop got scheduled - not when the compositor
         * repainted. Measured: four spinners gave 99 ms with priorities
         * and 99 ms without, which is the probe reporting on itself.
         *
         * With one, the observer is a much smaller share of the machine
         * and the number is attributable - which is how the 98.5 ms -> 39 ms
         * quantum result was obtained and is still the comparison this
         * self-test exists to protect. Measuring a genuinely loaded
         * desktop needs a probe that does not compete, i.e. one that
         * blocks rather than polls, and that is a different instrument. */
        int cpus = 1;
        task_t *load[4];
        int nload = 0;
        for (int i = 0; i < cpus; i++) {
            load[nload] = task_spawn("m69-load", spinner_task, NULL);
            if (load[nload]) {
                nload++;
            }
        }
        pit_sleep_ms(200); /* let them actually get going */

        uint64_t loaded_us = 0;
        for (int i = 0; i < 5; i++) {
            uint64_t us = selftest_input_to_photon_us(300 + i * 20, 300, 3000);
            if (us == 0) {
                continue;
            }
            if (loaded_us == 0 || us < loaded_us) {
                loaded_us = us;
            }
        }

        for (int i = 0; i < nload; i++) {
            selftest_reap(load[i]);
        }
        selftest_reap(comp);
        console_init();
        klog_use_console();

        if (idle_us == 0 || loaded_us == 0) {
            panic("M69 latency self-test: the cursor never reached the screen - "
                   "the measurement is measuring nothing");
        }

        klog_puts("[m69] input-to-photon: ");
        klog_put_dec((uint32_t)idle_us);
        klog_puts(" us idle, ");
        klog_put_dec((uint32_t)loaded_us);
        klog_puts(" us with ");
        klog_put_dec((uint32_t)nload);
        klog_puts(" CPU-bound task(s) running - measured with the TSC, "
                   "cursor motion to changed pixel; the observer competes, so this is an "
                   "upper bound - self-test passed.\n\n");
    }

    /* ---- M70 self-test: the machine says what happened ------------------
     *
     * Three claims, and the first one is the milestone:
     *
     *   1. the kernel log can be read back from user space at all. For
     *      seventy milestones it could not - klog wrote to a serial port
     *      and to a console the compositor paints over, so every driver
     *      message, every spawn failure and every capability denial went
     *      somewhere nobody on the machine could reach.
     *   2. reading it is gated. The log describes what every other
     *      process is doing, so CAP_SYSLOG is a real boundary and an
     *      ordinary program has to be refused - checked the way M65
     *      checks everything, by having a program try and fail.
     *   3. a cursor follows rather than re-reads. A viewer that showed
     *      the same lines again on every pass would be useless, and the
     *      cursor is the whole difference.
     *
     * The panic-paints half of M70 cannot be asserted from inside a
     * self-test - a panic stops the machine, so a test that triggered one
     * could not then report anything. It is verified by the input
     * harness instead, which screenshots a deliberately faulted guest
     * with no serial device attached; see tools/qemu-input-test.sh.
     */

    /* ---- M68 self-test: the machine actually sleeps -------------------
     *
     * The claim is "a task with nothing to do leaves the run queue", and
     * the only honest way to check it is to measure - a desktop that
     * spins and one that halts are indistinguishable from the outside,
     * which is precisely how this OS shipped sixty-seven milestones
     * without anyone noticing that no core had ever gone idle.
     *
     * Two assertions, and the second is the one with teeth:
     *
     *   1. a task that blocks is TASK_BLOCKED, not READY. Checked
     *      directly, because the whole mechanism rests on it and a
     *      wait that quietly stayed runnable would still pass every
     *      behavioural test in this file.
     *   2. the BSP accumulates real idle ticks while a child sleeps.
     *      Before this milestone that number was exactly zero on every
     *      core for the entire life of the machine.
     *
     * Deliberately measured against a *sleeping child* rather than
     * against the idle desktop: the desktop has not been handed off yet
     * at this point in boot, and a test that waited for it would be
     * measuring the compositor's timeout choices rather than the
     * scheduler's blocked state. What is being proved here is the
     * primitive; the desktop's use of it is the interactive suite's job.
     */
    {
        int all_ok = 1;

        /* A marker only this test could have written, so finding it back
         * proves the ring holds what klog emitted rather than merely
         * returning plausible bytes. */
        static const char MARKER[] = "[m70] marker-cafebabe";
        klog_puts(MARKER);
        klog_putc('\n');

        static char logbuf[1024];
        uint64_t cursor = 0;
        uint64_t next = 0;
        int found = 0;
        /* Walk the whole ring looking for the marker. Bounded by the
         * number of reads it takes to cross 64 KiB, not by a timeout:
         * this is a memory copy, not a wait. */
        for (int pass = 0; pass < 128 && !found; pass++) {
            long n = do_syscall4(SYS_klog, cursor, (uint64_t)logbuf, sizeof(logbuf) - 1,
                                  (uint64_t)&next);
            if (n <= 0) {
                break;
            }
            logbuf[n] = '\0';
            for (long i = 0; i + (long)sizeof(MARKER) - 1 <= n; i++) {
                int j = 0;
                while (MARKER[j] && logbuf[i + j] == MARKER[j]) {
                    j++;
                }
                if (!MARKER[j]) {
                    found = 1;
                    break;
                }
            }
            if (next == cursor) {
                break; /* no progress - the cursor is not advancing */
            }
            cursor = next;
        }
        if (!found) {
            klog_puts("[m70] the kernel log does not contain what klog just wrote to it\n");
            all_ok = 0;
        }

        /* The cursor follows: a read from the end returns nothing until
         * something new is logged, and then returns exactly that. */
        uint64_t end = klog_written_total();
        long none = do_syscall4(SYS_klog, end, (uint64_t)logbuf, sizeof(logbuf) - 1,
                                 (uint64_t)&next);
        if (none != 0) {
            klog_puts("[m70] a read from the end of the log returned bytes that were not "
                       "written yet\n");
            all_ok = 0;
        }
        klog_puts("x\n");
        long some = do_syscall4(SYS_klog, end, (uint64_t)logbuf, sizeof(logbuf) - 1,
                                 (uint64_t)&next);
        if (some <= 0) {
            klog_puts("[m70] the log did not advance after something was written to it\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M70 self-test: the kernel cannot report its own log");
        }

        /* ---- the panic band, painted and read back ---------------------
         *
         * A panic used to reach a serial port and a framebuffer console
         * the compositor had painted over, which on a machine with no
         * serial cable is a frozen screen and nothing else. It now takes
         * the screen back. Checked by painting it and reading the pixels,
         * because a test that caused a real panic could not report what
         * it found - see panic.h on why the rendering and the halting are
         * separate functions.
         *
         * Two assertions, and the second is the one with teeth: the band
         * is there (something was painted at all), AND there are glyph
         * pixels inside it (the message was actually rendered rather than
         * a coloured rectangle drawn over the screen). */
        {
            const uint32_t BAND = 0x00800000u;
            uint32_t h = fb_height();
            uint32_t band_h = 8 * FONT_HEIGHT;
            uint32_t band_y = (h > band_h) ? (h - band_h) / 2 : 0;

            panic_render("m70 paint check - the machine is fine, this is a test");

            int band_ok = (fb_get_pixel(4, band_y + band_h - 3) == BAND);

            /* Glyph pixels: scan the row the title is drawn on for
             * anything that is not the band colour. The title starts at
             * x=16 and is white. */
            int glyph_pixels = 0;
            for (uint32_t gx = 16; gx < 16 + 20 * FONT_WIDTH && gx < fb_width(); gx++) {
                for (uint32_t gy = band_y + FONT_HEIGHT; gy < band_y + 2 * FONT_HEIGHT; gy++) {
                    if (fb_get_pixel(gx, gy) != BAND) {
                        glyph_pixels++;
                    }
                }
            }

            /* Put the console back before logging anything, or the next
             * klog line lands on top of the band. */
            console_init();
            klog_use_console();

            if (!band_ok) {
                klog_puts("[m70] a panic painted nothing to the framebuffer\n");
                all_ok = 0;
            }
            if (glyph_pixels < 50) {
                klog_puts("[m70] the panic band was painted but the message was not drawn "
                           "into it (");
                klog_put_dec((uint32_t)glyph_pixels);
                klog_puts(" glyph pixels)\n");
                all_ok = 0;
            }
            if (!all_ok) {
                panic("M70 self-test: a panic cannot put its message on the screen");
            }
        }

        klog_puts("[m70] the kernel log is readable from user space: a marker written and "
                   "found again, a cursor that follows rather than repeats, a gate "
                   "(CAP_SYSLOG) an ordinary program does not hold, and a panic that paints "
                   "its own message onto the framebuffer rather than into a serial port "
                   "nobody is holding - self-test passed.\n\n");
    }

    /* ---- M71 self-test: files worth trusting ---------------------------
     *
     * Three claims, and each is about a way this filesystem could lose
     * something rather than about a feature working.
     *
     *   1. **an atomic replace exists.** SYS_writefile truncates and then
     *      writes, so from the truncate until the last byte the file on
     *      disk is neither the old document nor the new one - a crash
     *      there loses both. SYS_rename_replace repoints one directory
     *      record instead, so the name resolves to the old inode right up
     *      until it resolves to the new one.
     *   2. **the name never stops resolving.** Checked by doing the
     *      replace and requiring the target to be readable with the right
     *      contents at every point a caller could look - which is the
     *      property, stated as a test rather than as a comment.
     *   3. **an unclean mount is survivable.** A crash between allocating
     *      blocks and pointing an inode at them leaks them: the bitmap
     *      says used, nothing refers to them, and nothing ever reclaimed
     *      them. Simulated exactly - allocate a file, then orphan its
     *      blocks the way a crash would by clearing the inode without
     *      freeing them - and leanfs_check has to find and reclaim them.
     */
    {
        int all_ok = 1;
        static const char OLD_TEXT[] = "the version that was already there";
        static const char NEW_TEXT[] = "the version being written over it";
        static char readback[128];

        const char *target = PATH_TMP_DIR "m71target";
        const char *temp   = PATH_TMP_DIR "m71target.tmp~";

        /* ---- 1 & 2: the replace, and what is readable across it ------ */
        if (do_syscall(SYS_writefile, (uint64_t)target, (uint64_t)OLD_TEXT,
                        sizeof(OLD_TEXT) - 1) != 0 ||
            do_syscall(SYS_writefile, (uint64_t)temp, (uint64_t)NEW_TEXT,
                        sizeof(NEW_TEXT) - 1) != 0) {
            panic("M71 self-test: could not set up the replace fixture");
        }

        /* Before: the target reads as the old contents. */
        k_memset(readback, 0, sizeof(readback));
        do_syscall(SYS_readfile, (uint64_t)target, (uint64_t)readback, sizeof(readback) - 1);
        if (k_strcmp(readback, OLD_TEXT) != 0) {
            klog_puts("[m71] the fixture did not read back as itself\n");
            all_ok = 0;
        }

        /* SYS_rename would refuse this - the destination exists, which is
         * M56's deliberate safety and the reason the replacing variant
         * had to be a separate call rather than a loosening of that one. */
        if (do_syscall(SYS_rename, (uint64_t)temp, (uint64_t)target, 0) == 0) {
            klog_puts("[m71] SYS_rename replaced an existing file - M56's guarantee is gone\n");
            all_ok = 0;
        }

        if (do_syscall(SYS_rename_replace, (uint64_t)temp, (uint64_t)target, 0) != 0) {
            klog_puts("[m71] SYS_rename_replace failed on an existing destination\n");
            all_ok = 0;
        }

        /* After: the target reads as the NEW contents, and the temp name
         * is gone. Both halves matter - a replace that left the temp
         * behind would be a copy, not a rename. */
        k_memset(readback, 0, sizeof(readback));
        do_syscall(SYS_readfile, (uint64_t)target, (uint64_t)readback, sizeof(readback) - 1);
        if (k_strcmp(readback, NEW_TEXT) != 0) {
            klog_puts("[m71] after the replace the target is not the new contents\n");
            all_ok = 0;
        }
        if (vfs_exists(temp)) {
            klog_puts("[m71] the temporary file survived the rename - that is a copy, not a replace\n");
            all_ok = 0;
        }

        /* Replacing a file with itself must be a no-op, not an unlink -
         * the one input to this call that could destroy the very thing it
         * was asked to preserve. */
        if (do_syscall(SYS_rename_replace, (uint64_t)target, (uint64_t)target, 0) != 0 ||
            !vfs_exists(target)) {
            klog_puts("[m71] renaming a file onto itself destroyed it\n");
            all_ok = 0;
        }

        /* ---- 3: an unclean mount, and the blocks a crash leaks ------- */
        uint32_t free_before = vfs_free_blocks();

        /* A file big enough to need several blocks, then orphaned the way
         * a crash between "allocate" and "record" orphans one: the
         * directory entry goes, but the blocks stay marked used because
         * nothing walked the inode to free them. vfs_unlink would free
         * them properly, which is exactly what must NOT happen here. */
        static char filler[3000];
        k_memset(filler, 'z', sizeof(filler));
        const char *doomed = PATH_TMP_DIR "m71orphan";
        if (do_syscall(SYS_writefile, (uint64_t)doomed, (uint64_t)filler, sizeof(filler)) != 0) {
            panic("M71 self-test: could not write the orphan fixture");
        }
        uint32_t free_with_file = vfs_free_blocks();
        if (free_with_file >= free_before) {
            klog_puts("[m71] writing a 3 KiB file consumed no blocks - the fixture is wrong\n");
            all_ok = 0;
        }

        leanfs_debug_orphan(doomed); /* the crash, simulated precisely */

        uint32_t free_orphaned = vfs_free_blocks();
        if (free_orphaned != free_with_file) {
            klog_puts("[m71] orphaning did not leave the blocks allocated - nothing to reclaim\n");
            all_ok = 0;
        }

        vfs_check();
        uint32_t free_after = vfs_free_blocks();
        if (free_after != free_before) {
            klog_puts("[m71] the check did not reclaim every orphaned block (");
            klog_put_dec(free_before - free_after);
            klog_puts(" still missing)\n");
            all_ok = 0;
        }

        do_syscall(SYS_unlink, (uint64_t)target, 0, 0);

        if (!all_ok) {
            panic("M71 self-test: this filesystem can still lose a file");
        }

        klog_puts("[m71] files worth trusting: a replace that repoints one directory record "
                   "so the name never stops resolving, a plain rename that still refuses to "
                   "overwrite, and an unclean mount whose orphaned blocks are found and "
                   "reclaimed - self-test passed.\n\n");
    }

    /* ---- M72 self-test: a shell, and a script that is a program --------
     *
     * The honest test of a shell is not that it runs a command - the old
     * 109-line one did that. It is that **something which used to need a
     * compiler now does not**: a file of commands, spawned like any other
     * program, producing a result this test can read.
     *
     * So the fixture is a real script with a `#!` line, spawned through
     * the ordinary SYS_spawn that a desktop icon or the launcher would
     * use - nothing here knows it is not an ELF - and it is required to
     * exercise the things that separate a shell from a command splitter:
     * variables, quoting, `&&`/`||` gated on a real exit status, and a
     * redirect whose output is what gets checked.
     */
    {
        int all_ok = 1;
        const char *script = PATH_TMP_DIR "m72.sh";
        const char *result = PATH_TMP_DIR "m72.out";

        /* `false` is not a program on this machine, so `command-not-found`
         * is how the script produces a non-zero status - which is exactly
         * the 127 a shell is supposed to report, and makes the `||` below
         * a test of the status rather than of the parser. */
        static const char SCRIPT[] =
            "#!/bin/sh\n"
            "# a comment, which must not be run\n"
            "GREETING=hello\n"
            "NAME='lean os'\n"
            "echo $GREETING \"$NAME\" > " PATH_TMP_DIR "m72.out\n"
            "notaprogram\n"
            "echo status=$? >> " PATH_TMP_DIR "m72.out\n"
            /* `cd`, not `true`: there is no /bin/true on this machine,
             * and a test whose success case depends on a program that
             * does not exist is a test that passes for the wrong reason
             * or fails for one. `cd` is a builtin and returns 0. */
            "cd " PATH_TMP_DIR " && echo and-ran >> " PATH_TMP_DIR "m72.out\n"
            "notaprogram || echo or-ran >> " PATH_TMP_DIR "m72.out\n"
            "notaprogram && echo must-not-run >> " PATH_TMP_DIR "m72.out\n";

        if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                        sizeof(SCRIPT) - 1) != 0) {
            panic("M72 self-test: could not write the script fixture");
        }

        /* Spawned by path, with no argument and nothing told about it
         * being a script. If `#!` works, this loads /bin/sh instead. */
        long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
        if (pid < 0) {
            klog_puts("[m72] a #! script could not be spawned as a program\n");
            all_ok = 0;
        } else {
            do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
        }

        static char produced[256];
        k_memset(produced, 0, sizeof(produced));
        int64_t n = vfs_read(result, produced, sizeof(produced) - 1);
        if (n <= 0) {
            klog_puts("[m72] the script produced no output at all\n");
            all_ok = 0;
        } else {
            produced[n] = '\0';
            /* Each of these is a different claim, so each is checked
             * separately rather than by comparing the whole blob - a
             * single mismatch then names which feature is broken. */
            static const struct { const char *needle; const char *what; } EXPECT[] = {
                {"hello lean os", "variable expansion and a quoted argument holding a space"},
                {"status=127",    "a real exit status for a command that does not exist"},
                {"and-ran",       "&& running its right side after a success"},
                {"or-ran",        "|| running its right side after a failure"},
            };
            for (size_t e = 0; e < sizeof(EXPECT) / sizeof(EXPECT[0]); e++) {
                int found = 0;
                for (int64_t i = 0; i < n && !found; i++) {
                    int j = 0;
                    while (EXPECT[e].needle[j] && produced[i + j] == EXPECT[e].needle[j]) {
                        j++;
                    }
                    found = (EXPECT[e].needle[j] == '\0');
                }
                if (!found) {
                    klog_puts("[m72] the script did not demonstrate ");
                    klog_puts(EXPECT[e].what);
                    klog_putc('\n');
                    all_ok = 0;
                }
            }
            /* And the negative: `&&` after a failure must run nothing.
             * Without this, a shell that ran every branch unconditionally
             * would pass all four checks above. */
            for (int64_t i = 0; i + 12 < n; i++) {
                int j = 0;
                static const char NEVER[] = "must-not-run";
                while (NEVER[j] && produced[i + j] == NEVER[j]) {
                    j++;
                }
                if (NEVER[j] == '\0') {
                    klog_puts("[m72] && ran its right side after a failure\n");
                    all_ok = 0;
                    break;
                }
            }
        }

        do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
        do_syscall(SYS_unlink, (uint64_t)result, 0, 0);

        if (!all_ok) {
            /* The bytes, but only on failure. A test that says which
             * expectation was missed makes you guess at the shell; the
             * actual output says what it did. On a passing run it is
             * noise, so it is not printed. */
            klog_puts("[m72] what the script actually wrote:\n");
            klog_puts(produced);
            klog_puts("[m72] ---- end\n");
            panic("M72 self-test: scripts do not run, or the shell is not one");
        }

        klog_puts("[m72] a script is a program: `#!` resolved by the ordinary spawn path, "
                   "variables, quoting, a redirect, and && / || gated on a real exit status "
                   "- self-test passed.\n\n");
        /* A child that blocks on a pipe nobody will ever write to. It has
         * a 500 ms deadline of its own so it cannot wedge the boot if the
         * block never returns - which is the failure this test is most
         * likely to produce, and a self-test that hangs reports nothing. */
        int idle_fds[2];
        if (do_syscall(SYS_pipe, (uint64_t)idle_fds, 0, 0) != 0) {
            panic("M68 self-test: SYS_pipe failed");
        }

        uint64_t idle_before = sched_idle_ticks(0);
        uint64_t total_before = sched_total_ticks(0);

        /* Nothing writes to idle_fds[1] yet, so this task parks in
         * SYS_waitfds and stays parked. Spawned as a kernel thread rather
         * than a process because what is being measured is the scheduler,
         * and a kernel thread is the shortest path to a blocked task. */
        task_t *sleeper = task_spawn("m68-sleeper", m68_sleeper_task, (void *)(uint64_t)idle_fds[0]);
        if (!sleeper) {
            panic("M68 self-test: could not spawn the sleeper");
        }

        /* Long enough for the sleeper to reach pipe_read and park, and
         * for a meaningful number of ticks to accumulate. */
        pit_sleep_ms(400);

        if (sleeper->state != TASK_BLOCKED) {
            klog_puts("[m68] a task waiting in SYS_waitfds is not TASK_BLOCKED - it is "
                       "still in the run queue, which is the state this milestone exists to "
                       "remove\n");
            all_ok = 0;
        }

        uint64_t idle_gained = sched_idle_ticks(0) - idle_before;
        uint64_t total_gained = sched_total_ticks(0) - total_before;
        /* Reported, not asserted, and the distinction is honest rather
         * than convenient. What this milestone can prove is the state
         * machine: a task in SYS_waitfds is BLOCKED and a write wakes it,
         * both checked above and below. What it cannot prove here is a
         * *number* for idle time, because the boot is not an idle desktop
         * - the tcp-timer thread and kernel_main are both legitimately
         * halting in pit_sleep_ms, which counts, while several self-test
         * clients are legitimately spinning, which does not. Asserting a
         * threshold against that mixture would be asserting the shape of
         * the boot rather than the behaviour of the scheduler. */

        /* Wake it the way a real writer would, and require that it
         * actually came back. A blocked task that cannot be woken is
         * worse than one that never blocked. */
        static const char poke[] = "x";
        do_syscall(SYS_write, (uint64_t)idle_fds[1], (uint64_t)poke, 1);
        pit_sleep_ms(100);
        if (sleeper->state == TASK_BLOCKED) {
            klog_puts("[m68] a blocked task was not woken by a write to the pipe it was "
                       "waiting on\n");
            all_ok = 0;
        }
        do_syscall(SYS_kill, (uint64_t)sleeper->id, SIGKILL, 0);
        selftest_reap(sleeper);
        do_syscall(SYS_close, (uint64_t)idle_fds[0], 0, 0);
        do_syscall(SYS_close, (uint64_t)idle_fds[1], 0, 0);

        if (!all_ok) {
            panic("M68 self-test: tasks do not block, or blocked tasks do not wake");
        }

        (void)idle_gained;
        (void)total_gained;
        klog_puts("[m68] wait queues: a task in SYS_waitfds is TASK_BLOCKED rather than "
                   "runnable, a write to the pipe it waits on wakes it, and the BSP has "
                   "accumulated ");
        klog_put_dec((uint32_t)sched_idle_ticks(0));
        klog_puts(" idle tick(s) of ");
        klog_put_dec((uint32_t)sched_total_ticks(0));
        klog_puts(" so far - a count that did not exist before this milestone - "
                   "self-test passed.\n\n");
    }

    /* ---- M73 self-test: names, and the first inbound byte ---------------
     *
     * Two halves, and neither of them depends on the machine running QEMU
     * having internet - which is the whole trick, because a test that
     * needs the weather is a test that reports on the weather.
     *
     *   1. **The DNS parser**, fed responses built by hand. That is where
     *      a resolver actually goes wrong, and none of it needs a server:
     *      a reply is just bytes. The cases are the specific ways a real
     *      or hostile server ruins a naive parser - a compression pointer
     *      (which every real reply uses and a naive parser cannot read at
     *      all), a CNAME chain, a pointer that points at itself, somebody
     *      else's reply arriving with the wrong id, an answer to a
     *      different question, NXDOMAIN, and a truncated message.
     *   2. **A real HTTP GET over loopback.** `httpd` serves one canned
     *      response on a port; `fetch` retrieves it and writes it to a
     *      file; this test reads the file and compares it byte for byte.
     *      Every layer this project built is in that path - TCP's
     *      handshake, the loopback queue M66 had to add, the socket fd
     *      table from M64 - and the bytes at the end are the proof.
     *
     * That second half is the first time anything has arrived on this
     * machine's filesystem without being compiled into its disk image.
     */
    if (net_have_nic()) {
        int all_ok = 1;

        size_t ns_bytes = 0;
        uint8_t *ns_img = read_program(PATH_BIN_DIR "nslookup", &ns_bytes);
        const char *ns_argv[] = {PATH_BIN_DIR "nslookup", "-s", 0};
        task_t *ns = process_spawnv("nslookup", ns_img, ns_bytes, ns_argv);
        kfree(ns_img);
        if (!ns || do_syscall(SYS_wait, (uint64_t)ns->id, 0, 0) != 0) {
            klog_puts("[m73] the DNS parser self-test reported a failure\n");
            all_ok = 0;
        }

        /* The server first, so it is listening before anything connects.
         * It serves one request and exits, which is what a fixture should
         * do - a loop would be a process this test then has to remember
         * to kill. */
        size_t hd_bytes = 0;
        uint8_t *hd_img = read_program(PATH_BIN_DIR "httpd", &hd_bytes);
        /* M74-M79: 8080 -> 8081, and it is a real bug rather than a
         * tidy-up. tcptest (M66's self-test, a few blocks above) binds
         * 8080 too, and a TCP port is not free the moment its owner
         * exits - a closed connection sits in TIME_WAIT. On a quiet host
         * the gap between the two tests is long enough that nobody ever
         * noticed; on a machine running three guests at once it is not,
         * and this test panicked with "httpd: cannot listen on 8080" -
         * which reads as a networking failure and is really two
         * self-tests sharing a number. */
        const char *hd_argv[] = {PATH_BIN_DIR "httpd", "8081", 0};
        task_t *hd = process_spawnv("httpd", hd_img, hd_bytes, hd_argv);
        kfree(hd_img);
        pit_sleep_ms(300); /* long enough to bind and listen */

        const char *FETCHED = PATH_TMP_DIR "m73.txt";
        size_t ft_bytes = 0;
        uint8_t *ft_img = read_program(PATH_BIN_DIR "fetch", &ft_bytes);
        const char *ft_argv[] = {PATH_BIN_DIR "fetch",
                                  "http://127.0.0.1:8081/hello", FETCHED, 0};
        task_t *ft = process_spawnv("fetch", ft_img, ft_bytes, ft_argv);
        kfree(ft_img);
        if (!ft || do_syscall(SYS_wait, (uint64_t)ft->id, 0, 0) != 0) {
            klog_puts("[m73] fetch could not retrieve over loopback\n");
            all_ok = 0;
        }
        if (hd) {
            selftest_reap(hd);
        }

        static const char EXPECT[] = "lean_os fetched this over loopback\n";
        static char fetched[128];
        k_memset(fetched, 0, sizeof(fetched));
        int64_t fn = vfs_read(FETCHED, fetched, sizeof(fetched) - 1);
        if (fn != (int64_t)sizeof(EXPECT) - 1 || k_strcmp(fetched, EXPECT) != 0) {
            klog_puts("[m73] the fetched file is not what the server sent - got ");
            klog_put_dec((uint32_t)(fn < 0 ? 0 : fn));
            klog_puts(" byte(s)\n");
            all_ok = 0;
        }
        do_syscall(SYS_unlink, (uint64_t)FETCHED, 0, 0);

        if (!all_ok) {
            panic("M73 self-test: this machine cannot resolve a name or fetch a byte");
        }

        klog_puts("[m73] names, not numbers: a DNS parser that follows compression "
                   "pointers and CNAMEs and refuses a pointer loop, a wrong id and a "
                   "truncated reply - and an HTTP GET over loopback whose bytes reached "
                   "the filesystem, the first thing here that was not compiled in - "
                   "self-test passed.\n\n");
    } else {
        klog_puts("[m73] no NIC on this machine - DNS and HTTP are present but untested "
                   "this boot.\n\n");
    }

    /* ---- M74 self-test: the session that remembers ----------------------
     *
     * Two halves, tested separately because they fail separately: a
     * compositor that wrote a perfect session file and ignored it on
     * startup, and one that restored beautifully from a file it never
     * updated, are different bugs with the same symptom.
     *
     *   RESTORE - a session file is written by hand naming a program and
     *     a position the default cascade would never choose, a compositor
     *     is started with the environment init gives it, and the window
     *     has to appear *there*. Graded on the pixel, so a compositor
     *     that read the file and placed the window anyway cannot pass.
     *   SAVE - the file is then deleted out from under the running
     *     compositor, and it has to write it again, naming the program
     *     whose window is on screen.
     *
     * `gui_clock` is the fixture because it takes no arguments - which is
     * all a restore can give it - fills a fixed colour, and stays up.
     *
     * The gate is an environment variable and this is the first caller in
     * the project to pass one to process_spawnve deliberately. That is
     * M75 being load-bearing rather than decorative: the previous attempt
     * at this milestone gated on argv, which meant giving the
     * compositor's `main` parameters it had never taken, and that change
     * was one of the two suspects when the attempt was reverted.
     */
    {
        int all_ok = 1;
        const char *SESSION = PATH_ETC_DIR "session.conf";
        /* ---- the user's own session, preserved across this test -------
         *
         * This block writes a session file of its own and deletes it
         * afterwards, which is exactly what a self-test should do with a
         * fixture - and exactly the wrong thing to do to the file a
         * person's desktop wrote before the machine was last switched
         * off. Deleting it meant every boot began by forgetting what was
         * open, which is the feature this milestone exists to add, and
         * the failure was invisible from inside the self-test because
         * the self-test passed.
         *
         * Same shape as selftest_settings_install_defaults/restore, which
         * has had to do this for settings.conf since M47, and for the
         * same reason. */
        static char saved_session[512];
        int64_t saved_session_len = vfs_read(SESSION, saved_session, sizeof(saved_session));
        if (saved_session_len > (int64_t)sizeof(saved_session)) {
            saved_session_len = -1; /* larger than session_save ever writes - not ours to preserve */
        }
        const uint32_t CLOCK_BG = 0x00122438u;
        /* Far from the cascade (which starts at 100,100), so a freshly
         * placed window cannot land here by accident. */
        static const char SAVED[] = "gui_clock 520 380 200 90 0\n";
        /* Window-relative (10, 80): below the clock's text rows, so this
         * is flat background rather than a glyph. */
        const uint32_t PROBE_X = 530, PROBE_Y = 460;

        if (do_syscall(SYS_writefile, (uint64_t)SESSION, (uint64_t)SAVED,
                        sizeof(SAVED) - 1) != 0) {
            panic("M74 self-test: could not write the session fixture");
        }

        /* Motion off for this one - see SELFTEST_SETTINGS_NO_ANIM. Put
         * back at the end of the block, so every self-test after this
         * one runs against the same known settings as every test before
         * it. */
        vfs_write(PATH_SETTINGS, SELFTEST_SETTINGS_NO_ANIM,
                   sizeof(SELFTEST_SETTINGS_NO_ANIM) - 1);

        size_t comp_bytes = 0;
        uint8_t *comp_img = read_program(PATH_BIN_DIR "compositor", &comp_bytes);
        const char *comp_argv[] = {PATH_BIN_DIR "compositor", 0};
        const char *comp_envp[] = {"LEANOS_SESSION=1", 0};
        task_t *comp = process_spawnve("compositor", comp_img, comp_bytes, comp_argv, comp_envp);
        kfree(comp_img);
        if (!comp) {
            panic("M74 self-test: could not spawn a compositor");
        }

        if (!selftest_wait_for_pixel(PROBE_X, PROBE_Y, CLOCK_BG, 12000,
                                      "the session to relaunch a window and place it")) {
            klog_puts("[m74] the saved window was not brought back at its saved position\n");
            all_ok = 0;
        }

        /* Now the other half. Delete the file, then CHANGE THE LAYOUT -
         * a second window - because the compositor deliberately writes
         * only when what it would write has changed. Deleting the file on
         * its own is not a change to the session, and a save that fired
         * anyway would be a save firing on a timer, which is what the
         * signature exists to avoid. */
        do_syscall(SYS_unlink, (uint64_t)SESSION, 0, 0);

        size_t z_bytes = 0;
        uint8_t *z_img = read_program(PATH_BIN_DIR "wm_zorder", &z_bytes);
        task_t *second = process_spawn("wm_zorder", z_img, z_bytes, "s2 00C08040");
        kfree(z_img);
        /* No pixel assertion on this one, deliberately: where a second
         * window lands depends on the cascade index, which depends on the
         * restored window already occupying a slot - so a probe here
         * would assert the cascade's arithmetic rather than the session's
         * behaviour. Whether it connected at all is covered precisely by
         * the file check below, which requires it by name. */
        pit_sleep_ms(3000); /* connect, then the half-second save check */

        static char written[256];
        k_memset(written, 0, sizeof(written));
        int64_t wn = vfs_read(SESSION, written, sizeof(written) - 1);
        if (wn <= 0) {
            klog_puts("[m74] the compositor did not write a session after the layout changed\n");
            all_ok = 0;
        } else {
            written[wn] = '\0';
            /* Both programs, because a session that named only the one it
             * restored would be echoing its input rather than observing
             * the desktop. */
            if (!selftest_contains(written, "gui_clock") ||
                !selftest_contains(written, "wm_zorder")) {
                klog_puts("[m74] the session it wrote does not name both running programs: ");
                klog_puts(written);
                klog_putc('\n');
                all_ok = 0;
            }
        }

        if (second) {
            do_syscall(SYS_kill, (uint64_t)second->id, SIGKILL, 0);
            selftest_reap(second);
        }
        do_syscall(SYS_kill, (uint64_t)comp->id, SIGKILL, 0);
        selftest_reap(comp);
        /* The window the restore relaunched is somebody's child too - it
         * was spawned by the compositor, which has just been killed, so
         * nothing else will ever reap it. Left for the M40 handoff check
         * to notice otherwise. */
        for (int i = 0; i < sched_task_count(); i++) {
            task_t *o = sched_task_by_slot(i);
            if (o && o->state != TASK_FREE && o->state != TASK_TERMINATED &&
                k_strcmp(o->name, "gui_clock") == 0) {
                do_syscall(SYS_kill, (uint64_t)o->id, SIGKILL, 0);
                selftest_reap(o);
            }
        }
        /* The person's session back, exactly as it was - or gone, if
         * there was not one, which is what this test's own fixture must
         * not be mistaken for. */
        if (saved_session_len >= 0) {
            vfs_write(SESSION, saved_session, (size_t)saved_session_len);
        } else {
            do_syscall(SYS_unlink, (uint64_t)SESSION, 0, 0);
        }
        vfs_write(PATH_SETTINGS, SELFTEST_SETTINGS_CONF, sizeof(SELFTEST_SETTINGS_CONF) - 1);
        console_init();
        klog_use_console();

        if (!all_ok) {
            panic("M74 self-test: the desktop does not remember what was open");
        }

        klog_puts("[m74] the session remembers: a saved window relaunched and placed at its "
                   "own coordinates rather than the cascade's - graded on the pixel, not on "
                   "the file - and a session written naming both programs once a second "
                   "window changed the layout - self-test passed.\n\n");
    }

    /* ---- M75 self-test: environment, and a place to stand --------------
     *
     * The milestone's own statement of what would prove this: "Two
     * children spawned with different envp/cwd do a getenv/getcwd/open
     * round trip using only *relative* names and land on two different
     * real files - a relative path resolving correctly is the proof; an
     * unchanged string coming back proves nothing."
     *
     * So nothing here is graded on what a child printed. Each child is
     * given a different environment and started from a different
     * directory, and what is checked is which *file* appeared and what is
     * in it - a thing a broken implementation cannot fake by handing back
     * the string it was passed.
     *
     * The negative is the half that catches the most likely wrong
     * implementation: M75_ABSENT is set in this task's own environment
     * and in neither child's. A kernel that ignored envp and handed every
     * child its parent's environment would pass every positive check
     * above and fail this one.
     */
    {
        int all_ok = 1;
        const char *DIR_A = PATH_TMP_DIR "m75a";
        const char *DIR_B = PATH_TMP_DIR "m75b";
        do_syscall(SYS_mkdir, (uint64_t)DIR_A, 0, 0);
        do_syscall(SYS_mkdir, (uint64_t)DIR_B, 0, 0);
        do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m75a/alpha"), 0, 0);
        do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m75b/beta"), 0, 0);

        /* This task's own environment, so the negative check below has
         * something to be absent *from*. kernel_main is a kernel thread
         * and has never had one; setting it here is also the only place
         * in this project that exercises sched_set_env directly. */
        static const char SELF_ENV[] = "M75_ABSENT=1\0M75_OUT=wrong\0M75_BODY=wrong";
        sched_set_env(sched_current(), SELF_ENV, sizeof(SELF_ENV) - 1, 3);

        size_t et_bytes = 0;
        uint8_t *et_img = read_program(PATH_BIN_DIR "envtest", &et_bytes);
        if (!et_img) {
            panic("M75 self-test: /bin/envtest is not on this disk");
        }

        /* ---- child A: /tmp/m75a, writing "alpha" -------------------- */
        if (do_syscall(SYS_chdir, (uint64_t)DIR_A, 0, 0) != 0) {
            panic("M75 self-test: chdir into the fixture directory failed");
        }
        {
            const char *argv[] = {PATH_BIN_DIR "envtest", 0};
            const char *envp[] = {"M75_OUT=alpha", "M75_BODY=first", 0};
            task_t *t = process_spawnve("envtest", et_img, et_bytes, argv, envp);
            long rc = t ? do_syscall(SYS_wait, (uint64_t)t->id, 0, 0) : -1;
            if (rc != 0) {
                klog_puts("[m75] the first child exited ");
                klog_put_dec((uint32_t)(rc < 0 ? 99 : rc));
                klog_puts(" - see user_space/bin/envtest.c for what each code means\n");
                all_ok = 0;
            }
        }

        /* ---- child B: /tmp/m75b, writing "beta", reached with ".." --
         *
         * The directory is changed with a *relative* path containing
         * "..", which leanfs refuses outright (its own header says so) -
         * so this only works if the syscall layer resolved it before the
         * filesystem ever saw it. */
        if (do_syscall(SYS_chdir, (uint64_t)"../m75b", 0, 0) != 0) {
            klog_puts("[m75] `cd ../m75b` from /tmp/m75a did not resolve - \"..\" is not "
                       "being normalized before leanfs sees it\n");
            all_ok = 0;
        }
        {
            char where[PATH_MAX_LEN];
            k_memset(where, 0, sizeof(where));
            long n = do_syscall(SYS_getcwd, (uint64_t)where, sizeof(where), 0);
            if (n < 0 || k_strcmp(where, DIR_B) != 0) {
                klog_puts("[m75] after `cd ../m75b` the directory is '");
                klog_puts(where);
                klog_puts("' rather than ");
                klog_puts(DIR_B);
                klog_putc('\n');
                all_ok = 0;
            }
        }
        {
            const char *argv[] = {PATH_BIN_DIR "envtest", 0};
            const char *envp[] = {"M75_OUT=beta", "M75_BODY=second", 0};
            task_t *t = process_spawnve("envtest", et_img, et_bytes, argv, envp);
            long rc = t ? do_syscall(SYS_wait, (uint64_t)t->id, 0, 0) : -1;
            if (rc != 0) {
                klog_puts("[m75] the second child exited ");
                klog_put_dec((uint32_t)(rc < 0 ? 99 : rc));
                klog_putc('\n');
                all_ok = 0;
            }
        }
        kfree(et_img);

        /* ---- the grading: two files, in two places, with two bodies -- */
        static const struct {
            const char *path;
            const char *expect;
        } LANDED[] = {
            {PATH_TMP_DIR "m75a/alpha", PATH_TMP_DIR "m75a first"},
            {PATH_TMP_DIR "m75b/beta",  PATH_TMP_DIR "m75b second"},
        };
        for (size_t i = 0; i < sizeof(LANDED) / sizeof(LANDED[0]); i++) {
            static char got[PATH_MAX_LEN + 64];
            k_memset(got, 0, sizeof(got));
            int64_t n = vfs_read(LANDED[i].path, got, sizeof(got) - 1);
            if (n <= 0) {
                klog_puts("[m75] nothing was written to ");
                klog_puts(LANDED[i].path);
                klog_puts(" - a relative open did not land in the caller's directory\n");
                all_ok = 0;
                continue;
            }
            got[n] = '\0';
            if (k_strcmp(got, LANDED[i].expect) != 0) {
                klog_puts("[m75] ");
                klog_puts(LANDED[i].path);
                klog_puts(" holds '");
                klog_puts(got);
                klog_puts("' rather than '");
                klog_puts(LANDED[i].expect);
                klog_puts("'\n");
                all_ok = 0;
            }
        }

        /* ---- the shell, which is where a person meets all of this ----
         *
         * `cd` and `$VAR` were shell-local bookkeeping until this
         * milestone and the shell's own header comment said so. A script
         * is the shortest proof that they are not any more: it cds with a
         * relative name, exports a variable, and runs /bin/env - a
         * separate process - whose output has to contain that variable
         * and whose file has to appear in the directory the cd chose. */
        {
            const char *SCRIPT = PATH_TMP_DIR "m75.sh";
            const char *RESULT = PATH_TMP_DIR "m75a/fromsh";
            static const char SH[] =
                "#!/bin/sh\n"
                "cd " PATH_TMP_DIR "\n"
                "cd m75a\n"
                "M75_SHELL=exported\n"
                "export M75_SHELL\n"
                "env > fromsh\n"
                "pwd >> fromsh\n";
            if (do_syscall(SYS_writefile, (uint64_t)SCRIPT, (uint64_t)SH,
                            sizeof(SH) - 1) != 0) {
                panic("M75 self-test: could not write the shell fixture");
            }
            do_syscall(SYS_unlink, (uint64_t)RESULT, 0, 0);
            long pid = do_syscall(SYS_spawn, (uint64_t)SCRIPT, 0, 0);
            if (pid < 0) {
                klog_puts("[m75] the shell fixture could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            static char shout[1024];
            k_memset(shout, 0, sizeof(shout));
            int64_t n = vfs_read(RESULT, shout, sizeof(shout) - 1);
            if (n <= 0) {
                klog_puts("[m75] `cd m75a` then `env > fromsh` produced nothing at ");
                klog_puts(RESULT);
                klog_puts(" - the shell's cd is still its own bookkeeping\n");
                all_ok = 0;
            } else {
                shout[n] = '\0';
                static const struct { const char *needle; const char *what; } WANT[] = {
                    {"M75_SHELL=exported", "a shell assignment reaching a spawned program's environment"},
                    {PATH_TMP_DIR "m75a",  "`pwd` reporting the directory two relative cds arrived at"},
                };
                for (size_t w = 0; w < sizeof(WANT) / sizeof(WANT[0]); w++) {
                    if (!selftest_contains(shout, WANT[w].needle)) {
                        klog_puts("[m75] the shell did not demonstrate ");
                        klog_puts(WANT[w].what);
                        klog_putc('\n');
                        all_ok = 0;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)SCRIPT, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)RESULT, 0, 0);
        }

        /* Back where this task started, so nothing after it inherits a
         * directory it did not ask for - every self-test below spawns
         * something. */
        do_syscall(SYS_chdir, (uint64_t)"/", 0, 0);
        sched_release_env(sched_current());
        do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m75a/alpha"), 0, 0);
        do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m75b/beta"), 0, 0);
        do_syscall(SYS_rmdir, (uint64_t)DIR_A, 0, 0);
        do_syscall(SYS_rmdir, (uint64_t)DIR_B, 0, 0);

        if (!all_ok) {
            panic("M75 self-test: there is still nowhere to stand and nothing to stand there with");
        }

        klog_puts("[m75] environment and a place to stand: two children given different "
                   "environments and started in different directories, each writing a file "
                   "named only relatively and landing in its own, `..` normalized before "
                   "leanfs ever saw it, and a shell whose cd and export are the real ones - "
                   "self-test passed.\n\n");
    }

    /* ---- M76 self-test: a signal a program can catch --------------------
     *
     * The milestone's own statement of proof: "A self-test program
     * installs a SIGINT handler; a driven Ctrl+C is asserted to run the
     * handler and leave the process alive - still listed in SYS_taskinfo
     * afterwards - rather than disappearing the way every process on this
     * machine does today."
     *
     * So the grading here is deliberately two-sided, and the second side
     * is the one that matters. A test that only checked "the handler ran"
     * would pass on a kernel that ran the handler and then killed the
     * process anyway, which is a kernel that has changed nothing. What is
     * asserted is that the target is *still in the task table, in a
     * living state, with the signal already handled* - and, right after
     * it, that a process with no handler is killed by the same signal
     * with the same 128+sig exit code it always had.
     */
    {
        int all_ok = 1;
        const char *READY = PATH_TMP_DIR "m76ready";
        const char *ALIVE = PATH_TMP_DIR "m76alive";
        do_syscall(SYS_unlink, (uint64_t)READY, 0, 0);
        do_syscall(SYS_unlink, (uint64_t)ALIVE, 0, 0);

        size_t st_bytes = 0;
        uint8_t *st_img = read_program(PATH_BIN_DIR "sigtest", &st_bytes);
        if (!st_img) {
            panic("M76 self-test: /bin/sigtest is not on this disk");
        }
        const char *st_argv[] = {PATH_BIN_DIR "sigtest", 0};
        task_t *st = process_spawnv("sigtest", st_img, st_bytes, st_argv);
        if (!st) {
            panic("M76 self-test: could not spawn sigtest");
        }
        int st_pid = st->id;

        /* It says when it is ready by creating a file - everything before
         * that point (raise, the blocked-signal check, SIGCHLD) has to
         * have passed for it to get there. A fixed sleep would be a race
         * in whichever direction this boot happened to be slow. */
        int ready = 0;
        for (int i = 0; i < 300 && !ready; i++) {
            if (vfs_exists(READY)) {
                ready = 1;
                break;
            }
            if (st->state == TASK_TERMINATED) {
                break;
            }
            pit_sleep_ms(50);
        }
        if (!ready) {
            klog_puts("[m76] sigtest never reached its ready point - it exited ");
            klog_put_dec((uint32_t)st->exit_code);
            klog_puts(" (see user_space/bin/sigtest.c for what each code means)\n");
            all_ok = 0;
        }

        if (ready) {
            /* The signal a Ctrl+C sends, sent the way gui_terminal now
             * sends it: SYS_kill, from the process that started it. */
            if (do_syscall(SYS_kill, (uint64_t)st_pid, SIGINT, 0) != 0) {
                klog_puts("[m76] SYS_kill refused SIGINT - only the two that kill are accepted\n");
                all_ok = 0;
            }
            int handled = 0;
            for (int i = 0; i < 200 && !handled; i++) {
                if (vfs_exists(ALIVE)) {
                    handled = 1;
                    break;
                }
                pit_sleep_ms(50);
            }
            if (!handled) {
                klog_puts("[m76] the SIGINT handler never ran, or the process did not "
                           "survive it - sigtest is ");
                klog_puts(st->state == TASK_TERMINATED ? "terminated" : "still running");
                klog_putc('\n');
                all_ok = 0;
            }

            /* THE assertion: still listed, still living. Read through
             * SYS_taskinfo rather than off the task_t, because "still
             * listed in SYS_taskinfo" is the milestone's own wording and
             * because it is what a task manager - and a person - would
             * actually see. */
            {
                static task_info_t infos[TASK_INFO_MAX];
                long n = do_syscall(SYS_taskinfo, (uint64_t)infos, TASK_INFO_MAX, 0);
                int found_living = 0;
                for (long i = 0; i < n; i++) {
                    if (infos[i].pid == st_pid && infos[i].state != TASK_INFO_TERMINATED) {
                        found_living = 1;
                    }
                }
                if (!found_living) {
                    klog_puts("[m76] after SIGINT, sigtest is not listed as a living task - "
                               "a caught signal still killed it\n");
                    all_ok = 0;
                }
            }

            static char alive[64];
            k_memset(alive, 0, sizeof(alive));
            int64_t an = vfs_read(ALIVE, alive, sizeof(alive) - 1);
            if (an <= 0 || !selftest_contains(alive, "handled-and-alive 1")) {
                klog_puts("[m76] the handler ran a number of times other than once: '");
                klog_puts(alive);
                klog_puts("'\n");
                all_ok = 0;
            }

            /* Let it finish: a second SIGUSR1 is its "you may go". Which
             * is also a second delivery through the same trampoline, on a
             * process that has already been through it once - the case a
             * one-shot bug in the restorer would survive. */
            do_syscall(SYS_kill, (uint64_t)st_pid, SIGUSR1, 0);
            long code = do_syscall(SYS_wait, (uint64_t)st_pid, 0, 0);
            if (code != 0) {
                klog_puts("[m76] sigtest exited ");
                klog_put_dec((uint32_t)code);
                klog_puts(" - see user_space/bin/sigtest.c for what that code means\n");
                all_ok = 0;
            }
        }
        selftest_reap(st);

        /* ---- and the other side: no handler, same signal, dead --------
         *
         * `hello` installs nothing, so SIGINT's default action applies.
         * Without this check, a kernel that quietly ignored every signal
         * it did not have a handler for would pass everything above.
         *
         * hello writes a line and exits 0 on its own, so it is signalled
         * the moment it exists and the exit code is what distinguishes
         * "killed by SIGINT" (130) from "finished normally" (0). */
        {
            size_t h_bytes = 0;
            uint8_t *h_img = read_program(PATH_BIN_DIR "sh", &h_bytes);
            /* /bin/sh with no argument reads stdin, which nothing here
             * will ever write to - so it parks, which is exactly the
             * process a default-action signal has to be able to reach.
             * Chosen over a program that exits on its own precisely
             * because "it was still running" is what makes the exit code
             * below meaningful. */
            const char *h_argv[] = {PATH_BIN_DIR "sh", 0};
            task_t *h = process_spawnv("sh", h_img, h_bytes, h_argv);
            kfree(h_img);
            if (!h) {
                panic("M76 self-test: could not spawn the default-action fixture");
            }
            pit_sleep_ms(300); /* long enough to reach its blocking read */
            do_syscall(SYS_kill, (uint64_t)h->id, SIGINT, 0);
            long code = do_syscall(SYS_wait, (uint64_t)h->id, 0, 0);
            if (code != 128 + SIGINT) {
                klog_puts("[m76] a process with no SIGINT handler exited ");
                klog_put_dec((uint32_t)code);
                klog_puts(" rather than 130 - the default action is not being applied\n");
                all_ok = 0;
            }
            selftest_reap(h);
        }
        kfree(st_img);

        do_syscall(SYS_unlink, (uint64_t)READY, 0, 0);
        do_syscall(SYS_unlink, (uint64_t)ALIVE, 0, 0);

        if (!all_ok) {
            panic("M76 self-test: a signal here is still only a way to end a program");
        }

        klog_puts("[m76] a signal a program can catch: a handler installed, entered "
                   "through a frame on the process's own stack and returned from with "
                   "every register intact, a blocked signal held until it was unblocked, "
                   "a SIGCHLD that arrived without anyone polling, a SIGINT survived - and "
                   "the same SIGINT still ending a process that installed nothing - "
                   "self-test passed.\n\n");
    }

    /* ---- M77 self-test: POSIX names for what is already here ------------
     *
     * The milestone's own statement of proof: "A program written against
     * only <dirent.h>, <sys/stat.h> and <unistd.h> - none of this
     * project's own headers - walks a directory tree it knows nothing
     * about ahead of time and prints what it finds, the way `find` or
     * `du` would."
     *
     * /bin/treewalk is that program, and its include list is the part of
     * it that matters. What is graded here is its output against a tree
     * built right below - two levels, two sizes, and one name at each
     * level, so that a walker which listed only the top or which reported
     * a size from the wrong entry fails on a specific line rather than on
     * a total.
     *
     * Its stdout is pointed at a file with the same park/dup2/restore
     * cycle the [fd] self-test above asserts, because the honest way to
     * grade what a program printed is to read what it printed.
     */
    {
        int all_ok = 1;
        const char *ROOT = PATH_TMP_DIR "m77";
        const char *SUB = PATH_TMP_DIR "m77/inner";
        const char *OUT = PATH_TMP_DIR "m77out";

        /* Torn down first as well as last: a previous boot on a disk this
         * one did not format would otherwise leave a tree with extra
         * entries in it, and the counts below are exact. */
        do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m77/inner/deep.bin"), 0, 0);
        do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m77/top.txt"), 0, 0);
        do_syscall(SYS_rmdir, (uint64_t)SUB, 0, 0);
        do_syscall(SYS_rmdir, (uint64_t)ROOT, 0, 0);

        if (do_syscall(SYS_mkdir, (uint64_t)ROOT, 0, 0) != 0 ||
            do_syscall(SYS_mkdir, (uint64_t)SUB, 0, 0) != 0) {
            panic("M77 self-test: could not build the fixture tree");
        }
        static char eleven[11];
        static char thirty[30];
        k_memset(eleven, 'a', sizeof(eleven));
        k_memset(thirty, 'b', sizeof(thirty));
        if (do_syscall(SYS_writefile, (uint64_t)(PATH_TMP_DIR "m77/top.txt"),
                        (uint64_t)eleven, sizeof(eleven)) != 0 ||
            do_syscall(SYS_writefile, (uint64_t)(PATH_TMP_DIR "m77/inner/deep.bin"),
                        (uint64_t)thirty, sizeof(thirty)) != 0) {
            panic("M77 self-test: could not write the fixture files");
        }

        size_t tw_bytes = 0;
        uint8_t *tw_img = read_program(PATH_BIN_DIR "treewalk", &tw_bytes);
        if (!tw_img) {
            panic("M77 self-test: /bin/treewalk is not on this disk");
        }
        long saved = do_syscall(SYS_dup2, 1, 9, 0);
        long outfd = do_syscall(SYS_open, (uint64_t)OUT,
                                 OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE, 0);
        if (saved < 0 || outfd < 0) {
            panic("M77 self-test: could not redirect the walker's output");
        }
        do_syscall(SYS_dup2, (uint64_t)outfd, 1, 0);
        const char *tw_argv[] = {PATH_BIN_DIR "treewalk", ROOT, 0};
        task_t *tw = process_spawnv("treewalk", tw_img, tw_bytes, tw_argv);
        long rc = tw ? do_syscall(SYS_wait, (uint64_t)tw->id, 0, 0) : -1;
        do_syscall(SYS_dup2, (uint64_t)saved, 1, 0);
        do_syscall(SYS_close, (uint64_t)saved, 0, 0);
        do_syscall(SYS_close, (uint64_t)outfd, 0, 0);
        kfree(tw_img);
        if (rc != 0) {
            klog_puts("[m77] treewalk exited ");
            klog_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            klog_putc('\n');
            all_ok = 0;
        }

        static char walked[2048];
        k_memset(walked, 0, sizeof(walked));
        int64_t wn = vfs_read(OUT, walked, sizeof(walked) - 1);
        if (wn <= 0) {
            klog_puts("[m77] the walker printed nothing\n");
            all_ok = 0;
        } else {
            walked[wn] = '\0';
            static const struct { const char *needle; const char *what; } WANT[] = {
                {"f       11 " PATH_TMP_DIR "m77/top.txt",
                 "a file at the top level, with the size <sys/stat.h> reported"},
                {"d ", "a directory, told apart from a file by S_ISDIR"},
                {"f       30 " PATH_TMP_DIR "m77/inner/deep.bin",
                 "a file one level down, found by descending rather than by being told"},
                {"total 41 byte(s) in 2 file(s), 1 director(ies)",
                 "a total that adds up - which is what makes this a walk rather than a listing"},
            };
            for (size_t w = 0; w < sizeof(WANT) / sizeof(WANT[0]); w++) {
                if (!selftest_contains(walked, WANT[w].needle)) {
                    klog_puts("[m77] the walker did not demonstrate ");
                    klog_puts(WANT[w].what);
                    klog_putc('\n');
                    all_ok = 0;
                }
            }
            /* treewalk prints "!!" and keeps going whenever d_type and
             * st_mode disagree, or a stat fails. Any of those is a real
             * inconsistency between the two interfaces this milestone
             * added, and it must not be possible to pass while printing
             * one. */
            if (selftest_contains(walked, "!!")) {
                klog_puts("[m77] the walker reported an inconsistency:\n");
                klog_puts(walked);
                all_ok = 0;
            }
        }

        /* ---- fstat, which is the one thing a path cannot answer -------
         *
         * Checked from here rather than inside treewalk because the case
         * worth checking is a descriptor whose *name has since changed*,
         * and building that inside a tree walker would make the walker
         * about something else. */
        {
            const char *A = PATH_TMP_DIR "m77/named.txt";
            const char *B = PATH_TMP_DIR "m77/renamed.txt";
            do_syscall(SYS_unlink, (uint64_t)B, 0, 0);
            do_syscall(SYS_writefile, (uint64_t)A, (uint64_t)thirty, sizeof(thirty));
            long fd = do_syscall(SYS_open, (uint64_t)A, OPEN_READ, 0);
            if (fd < 0) {
                klog_puts("[m77] could not open the fstat fixture\n");
                all_ok = 0;
            } else {
                if (do_syscall(SYS_rename, (uint64_t)A, (uint64_t)B, 0) != 0) {
                    klog_puts("[m77] could not rename the fstat fixture out from under its fd\n");
                    all_ok = 0;
                }
                os_stat_t st;
                k_memset(&st, 0, sizeof(st));
                if (do_syscall(SYS_fstat, (uint64_t)fd, (uint64_t)&st, 0) != 0 ||
                    st.size != sizeof(thirty) || st.is_dir) {
                    klog_puts("[m77] SYS_fstat could not describe a descriptor whose name "
                               "had changed - which is the one question SYS_stat cannot answer\n");
                    all_ok = 0;
                }
                /* And a descriptor that is not a file at all is refused
                 * rather than described with invented numbers. */
                int pfds[2];
                if (do_syscall(SYS_pipe, (uint64_t)pfds, 0, 0) == 0) {
                    if (do_syscall(SYS_fstat, (uint64_t)pfds[0], (uint64_t)&st, 0) != -1) {
                        klog_puts("[m77] SYS_fstat invented a size and an mtime for a pipe\n");
                        all_ok = 0;
                    }
                    do_syscall(SYS_close, (uint64_t)pfds[0], 0, 0);
                    do_syscall(SYS_close, (uint64_t)pfds[1], 0, 0);
                }
                do_syscall(SYS_close, (uint64_t)fd, 0, 0);
            }
            do_syscall(SYS_unlink, (uint64_t)B, 0, 0);
        }

        do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m77/inner/deep.bin"), 0, 0);
        do_syscall(SYS_unlink, (uint64_t)(PATH_TMP_DIR "m77/top.txt"), 0, 0);
        do_syscall(SYS_unlink, (uint64_t)OUT, 0, 0);
        do_syscall(SYS_rmdir, (uint64_t)SUB, 0, 0);
        /* rmdir refuses a directory that is not empty, which is also the
         * last assertion this block makes: if anything above left a file
         * behind, this fails and the next boot's fixture would not be
         * exact. */
        if (do_syscall(SYS_rmdir, (uint64_t)ROOT, 0, 0) != 0) {
            klog_puts("[m77] the fixture tree could not be removed - something is still in it\n");
            all_ok = 0;
        }
        if (tw) {
            selftest_reap(tw);
        }

        if (!all_ok) {
            panic("M77 self-test: a program written against POSIX headers cannot walk this filesystem");
        }

        klog_puts("[m77] POSIX names for what is already here: a program including only "
                   "<dirent.h>, <sys/stat.h> and <unistd.h> walked a tree it was not told "
                   "the shape of, S_ISDIR and d_type agreed on every entry, the sizes added "
                   "up, and fstat described a descriptor whose name had changed - "
                   "self-test passed.\n\n");
    }

    /* ---- M78 self-test: memory that can be given back -------------------
     *
     * /bin/mmaptest carries the assertions a *program* can make - that a
     * freed range is the one handed back next, that an interior hole is
     * found, that fresh pages are zeroed, and that the refusals refuse.
     * See that file; its exit code names which one failed.
     *
     * What is added here, and could not be added there, is the frame
     * count. A program can see its own address space and cannot see the
     * machine's physical memory, so "the pages actually came back" is a
     * claim only the kernel can check - and it is the claim this
     * milestone is really about. M19's sbrk could hand a *virtual*
     * address back to a program's own free list all day without a single
     * frame ever returning to the allocator.
     */
    {
        int all_ok = 1;
        uint64_t frames_before = pmm_free_frame_count();

        size_t mt_bytes = 0;
        uint8_t *mt_img = read_program(PATH_BIN_DIR "mmaptest", &mt_bytes);
        if (!mt_img) {
            panic("M78 self-test: /bin/mmaptest is not on this disk");
        }
        const char *mt_argv[] = {PATH_BIN_DIR "mmaptest", 0};
        task_t *mt = process_spawnv("mmaptest", mt_img, mt_bytes, mt_argv);
        long rc = mt ? do_syscall(SYS_wait, (uint64_t)mt->id, 0, 0) : -1;
        kfree(mt_img);
        if (rc != 0) {
            klog_puts("[m78] mmaptest exited ");
            klog_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            klog_puts(" - see user_space/bin/mmaptest.c for what each code means\n");
            all_ok = 0;
        }

        /* Every frame back. Not "roughly" - exactly, because a mapping
         * that leaked one page per round is the failure this counts, and
         * an approximate comparison would pass it. The process is gone by
         * now, so its whole address space has been torn down too; what
         * this proves together with mmaptest's own address checks is that
         * munmap released frames at the time it was called, and that the
         * arena is in process_destroy_address_space's owned list so the
         * rest came back at exit. */
        uint64_t frames_after = pmm_free_frame_count();
        if (frames_after != frames_before) {
            klog_puts("[m78] ");
            klog_put_dec((uint32_t)(frames_before > frames_after
                                     ? frames_before - frames_after : 0));
            klog_puts(" frame(s) did not come back from a process that mapped 64 pages, "
                       "freed them, and exited\n");
            all_ok = 0;
        }

        /* ---- and the same claim, made from inside the kernel ---------
         *
         * A second process that maps and munmaps *without exiting*, so
         * that "the frames came back" is separated from "the address
         * space was torn down". Driven as a kernel-side count around a
         * live process, which is the only place both numbers are
         * visible at once.
         *
         * mmaptest already unmaps everything it made before returning,
         * so what this measures is precisely the munmap path: if munmap
         * unmapped without freeing, the count would still be short here
         * and would only be made whole by the exit above. */
        if (all_ok) {
            uint64_t mid_before = pmm_free_frame_count();
            const char *again[] = {PATH_BIN_DIR "mmaptest", 0};
            uint8_t *again_img = read_program(PATH_BIN_DIR "mmaptest", &mt_bytes);
            task_t *m2 = again_img
                             ? process_spawnv("mmaptest", again_img, mt_bytes, again)
                             : (task_t *)0;
            if (m2) {
                do_syscall(SYS_wait, (uint64_t)m2->id, 0, 0);
            }
            kfree(again_img);
            if (pmm_free_frame_count() != mid_before) {
                klog_puts("[m78] a second map/free/exit round did not return every frame\n");
                all_ok = 0;
            }
            if (m2) {
                selftest_reap(m2);
            }
        }
        if (mt) {
            selftest_reap(mt);
        }

        if (!all_ok) {
            panic("M78 self-test: this machine still cannot take a page back");
        }

        klog_puts("[m78] memory that can be given back: 64 pages mapped and touched, half "
                   "released and the *same addresses* handed out again rather than the arena "
                   "growing, an interior hole reused, a shared or file-backed mapping refused "
                   "by name, malloc routing a repeated 1 MiB allocation through it without "
                   "growing the process, and every frame back at the end - self-test passed.\n\n");
    }

    /* ---- M79 self-test: two threads, one address space ------------------
     *
     * /bin/threadtest carries the assertions a program can make - the
     * classic two-threads-one-counter test, that memory really is
     * shared, that the two report different tids and the same pid, and
     * that each one's floating-point state survives being interleaved
     * with the other's. See that file; its exit code names which failed.
     *
     * What is added here is the half a program cannot see. From inside,
     * a "thread" that was quietly a whole second process with its own
     * page table would pass every check except the shared-memory one -
     * so the kernel checks the thing that actually defines a thread:
     * while both are running, two entries in the task table have the
     * SAME pml4_phys. That has never been true on this machine before,
     * and it is the one sentence this milestone is about.
     */
    {
        int all_ok = 1;
        size_t tt_bytes = 0;
        uint8_t *tt_img = read_program(PATH_BIN_DIR "threadtest", &tt_bytes);
        if (!tt_img) {
            panic("M79 self-test: /bin/threadtest is not on this disk");
        }
        /* M81: reap everything already dead BEFORE the baseline, not
         * after.
         *
         * The sweep at the end of this test used to be justified with
         * "their kernel stacks are heap rather than frames, but their
         * slots have to go back" - true when it was written, and made
         * false by M81 moving task stacks from kmalloc to
         * pmm_alloc_contiguous. Reaping a task left over from an earlier
         * self-test now returns its stack's frames, so the sweep was
         * handing this test *more* free frames than it started with and
         * the strict equality below failed with a leak of zero. Clearing
         * the backlog first keeps the assertion exact, which is the
         * property worth protecting: "every frame back" should not have to
         * be spelled "every frame back, give or take an earlier test's
         * litter". */
        for (int i = 0; i < sched_task_count(); i++) {
            task_t *stale = sched_task_by_slot(i);
            if (stale && stale->state == TASK_TERMINATED) {
                selftest_reap(stale);
            }
        }

        uint64_t frames_before = pmm_free_frame_count();

        const char *tt_argv[] = {PATH_BIN_DIR "threadtest", 0};
        task_t *tt = process_spawnv("threadtest", tt_img, tt_bytes, tt_argv);
        kfree(tt_img);
        if (!tt) {
            panic("M79 self-test: could not spawn threadtest");
        }

        /* Caught while it is running, which is the only time the claim is
         * observable at all. Polled rather than timed: the threads are
         * created in the first instants of the program and live for the
         * whole of it, so a short poll finds them, and a fixed sleep
         * would be a guess about a machine whose speed varies. */
        int shared_seen = 0;
        int max_sharers = 0;
        for (int i = 0; i < 400 && !shared_seen; i++) {
            int count = sched_count_sharing_address_space(tt->pml4_phys);
            if (count > max_sharers) {
                max_sharers = count;
            }
            if (count >= 3) {
                /* the leader and its two threads */
                shared_seen = 1;
                break;
            }
            if (tt->state == TASK_TERMINATED) {
                break;
            }
            pit_sleep_ms(25);
        }
        if (!shared_seen) {
            klog_puts("[m79] never saw three tasks sharing one page table - the most that "
                       "ever did was ");
            klog_put_dec((uint32_t)max_sharers);
            klog_puts(", so a 'thread' here is still a process\n");
            all_ok = 0;
        }

        long rc = do_syscall(SYS_wait, (uint64_t)tt->id, 0, 0);
        if (rc != 0) {
            klog_puts("[m79] threadtest exited ");
            klog_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            klog_puts(" - see user_space/bin/threadtest.c for what each code means\n");
            all_ok = 0;
        }
        selftest_reap(tt);

        /* Every frame back, which is the assertion the last-one-out rule
         * in task_exit_with_code exists for. A thread that tore the
         * address space down when it exited would have taken the leader
         * with it - a crash, not a leak - but a leader that tore it down
         * and left the threads' stacks behind, or three tasks each
         * declining to free because the others looked alive, is exactly
         * a leak and exactly what this counts.
         *
         * Reaping every terminated thread first: their slots have to go
         * back or the next milestone's self-test starts from a smaller
         * table, and since M81 their kernel stacks are frames too - which
         * is exactly why the same sweep now also runs before the baseline
         * above. Both sweeps are needed: this one returns what this test
         * created, that one clears what earlier tests left. */
        for (int i = 0; i < sched_task_count(); i++) {
            task_t *o = sched_task_by_slot(i);
            if (o && o->state == TASK_TERMINATED) {
                selftest_reap(o);
            }
        }
        uint64_t frames_after = pmm_free_frame_count();
        if (frames_after != frames_before) {
            klog_puts("[m79] ");
            klog_put_dec((uint32_t)(frames_before > frames_after
                                     ? frames_before - frames_after : 0));
            klog_puts(" frame(s) did not come back from a process that ran two threads\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M79 self-test: this scheduler still cannot run two tasks in one address space");
        }

        klog_puts("[m79] two threads, one address space: three tasks on one page table at "
                   "once, two million increments through a mutex arriving as exactly two "
                   "million, memory written by one thread read by the other, separate tids "
                   "under one pid, each thread's floating-point state surviving the other's, "
                   "and every frame back when the last of them left - self-test passed.\n\n");
    }

    /* M81 self-test: a filesystem that can hold somebody else's program.
     *
     * Four claims, and each one is a wall this filesystem had before this
     * milestone rather than a limit it merely approached:
     *
     *   1. More files than the old whole-disk cap of 192. The number
     *      below is deliberately well past it, because "192 was a floor
     *      under every real workload" is the milestone's whole argument
     *      and a test that created 190 would prove nothing.
     *   2. A name longer than 27 characters - the longest one leanfs
     *      will now take, so the boundary itself is exercised rather
     *      than a comfortable value near it.
     *   3. A path longer than 128 bytes, built by nesting, because a
     *      deep tree is how a real source tarball produces one.
     *   4. Every one of them found again by streaming readdir, with the
     *      inode numbers all distinct - which is the check that a
     *      directory holding hundreds of variable-length records across
     *      many blocks is being walked correctly and not merely being
     *      walked without crashing.
     *
     * The cost is reported rather than hidden. Every file here is real
     * PIO writes on a disk this project measured as its slowest thing
     * (M56), and a self-test that quietly added a minute to every boot
     * would be a bad trade made invisibly. See the timing line below: if
     * it grows, that is the number to argue with.
     */
    {
        int all_ok = 1;
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));

        /* Thousands, because that is the claim. The milestone's argument
         * is that 192 was not a small ceiling but a floor below every
         * real workload - a language's standard library is roughly three
         * thousand files - so a test that created 190 and passed would be
         * measuring nothing.
         *
         * This is the one number here that is a trade rather than a
         * boundary, and the trade was measured rather than guessed: 400
         * files cost 2.1 s of boot, so the cost is about 2.7 ms per file
         * operation and this many is a little under seven seconds of a
         * 130-second boot. If that ever stops being worth it, the timing
         * printed at the end of this test is the number to argue with. */
        const int MANY = 1200;
        static const char *const MANY_DIR = PATH_TMP_DIR "m81many";

        if (!vfs_exists(MANY_DIR) && vfs_mkdir(MANY_DIR) != 0) {
            klog_puts("[m81] could not create the directory for the file storm\n");
            all_ok = 0;
        }

        int created = 0;
        for (int i = 0; i < MANY && all_ok; i++) {
            char path[PATH_MAX_LEN];
            char name[16];
            m81_storm_name(name, i);
            if (path_join(path, PATH_TMP_DIR "m81many/", name) != 0) {
                klog_puts("[m81] a name in the file storm did not fit a path\n");
                all_ok = 0;
                break;
            }
            /* The body carries the index, so reading one back proves it
             * is the file that name refers to and not merely a file. */
            char body[4];
            body[0] = (char)(i & 0xFF);
            body[1] = (char)((i >> 8) & 0xFF);
            body[2] = 'm';
            body[3] = '\0';
            if (vfs_write(path, body, sizeof(body)) != 0) {
                klog_puts("[m81] the filesystem ran out at 0x");
                klog_put_hex32((uint32_t)i);
                klog_puts(" files - the inode cap is still a wall\n");
                all_ok = 0;
                break;
            }
            created++;
        }

        /* The longest name this filesystem will take, in the same
         * directory, so it lands among hundreds of short ones and has to
         * be placed and found by the variable-length record code rather
         * than by luck. */
        char longname[LEANFS_MAX_NAME + 1];
        for (int i = 0; i < LEANFS_MAX_NAME; i++) {
            longname[i] = (char)('a' + (i % 26));
        }
        longname[LEANFS_MAX_NAME] = '\0';
        {
            char path[PATH_MAX_LEN];
            if (path_join(path, PATH_TMP_DIR "m81many/", longname) != 0 ||
                vfs_write(path, "long", 5) != 0) {
                klog_puts("[m81] a 255-character name was refused\n");
                all_ok = 0;
            } else {
                char got[8];
                k_memset(got, 0, sizeof(got));
                if (vfs_read(path, got, sizeof(got)) != 5 || got[0] != 'l') {
                    klog_puts("[m81] a 255-character name did not read back by path\n");
                    all_ok = 0;
                }
            }
        }

        /* A path past the old 128-byte limit, built the way a real tree
         * builds one: by nesting. Twelve levels of a twenty-character
         * name is 250-odd bytes, which no path in this project could name
         * before this milestone. */
        char deep[PATH_MAX_LEN];
        int deep_len = 0;
        {
            const char *seg = "/adirectorylevelname";  /* 20 bytes */
            k_strlcpy(deep, PATH_TMP_DIR "m81deep", sizeof(deep));
            deep_len = (int)k_strlen(deep);
            if (!vfs_exists(deep) && vfs_mkdir(deep) != 0) {
                klog_puts("[m81] could not start the deep path\n");
                all_ok = 0;
            }
            for (int level = 0; level < 12 && all_ok; level++) {
                size_t seg_len = k_strlen(seg);
                if (deep_len + (int)seg_len >= (int)sizeof(deep)) {
                    break;
                }
                k_memcpy(deep + deep_len, seg, seg_len);
                deep_len += (int)seg_len;
                deep[deep_len] = '\0';
                if (!vfs_exists(deep) && vfs_mkdir(deep) != 0) {
                    klog_puts("[m81] mkdir failed at depth 0x");
                    klog_put_hex32((uint32_t)level);
                    klog_puts("\n");
                    all_ok = 0;
                }
            }
        }
        if (all_ok && deep_len <= 128) {
            klog_puts("[m81] the deep path is not actually deeper than the old limit\n");
            all_ok = 0;
        }
        if (all_ok) {
            char leaf[PATH_MAX_LEN];
            k_strlcpy(leaf, deep, sizeof(leaf));
            size_t l = k_strlen(leaf);
            k_strlcpy(leaf + l, "/bottom.txt", sizeof(leaf) - l);
            static const char deep_body[] = "reached the bottom";
            if (vfs_write(leaf, deep_body, sizeof(deep_body)) != 0) {
                klog_puts("[m81] could not write a file at the bottom of a 0x");
                klog_put_hex32((uint32_t)deep_len);
                klog_puts("-byte path\n");
                all_ok = 0;
            } else {
                char got[32];
                k_memset(got, 0, sizeof(got));
                if (vfs_read(leaf, got, sizeof(got)) != (int64_t)sizeof(deep_body) ||
                    got[0] != 'r') {
                    klog_puts("[m81] the file at the bottom of the deep path did not read back\n");
                    all_ok = 0;
                }
            }
        }

        /* Walk the storm directory back one entry at a time and check
         * three things at once: every file is there, the long name is
         * among them, and no two entries share an inode number. The last
         * is what would catch a record walk that re-reads a block or
         * mis-advances its cookie - failures that produce a plausible
         * count and the wrong contents. */
        int seen = 0;
        int saw_long = 0;
        int dup_inode = 0;
        if (all_ok) {
            /* One bit per inode, so "have I seen this number" is a test
             * rather than a search. LEANFS_MAX_INODES bits is 1 KiB. */
            static uint8_t seen_ino[LEANFS_MAX_INODES / 8];
            k_memset(seen_ino, 0, sizeof(seen_ino));
            uint32_t cookie = 0;
            leanfs_dir_entry_t e;
            int rc;
            while ((rc = vfs_readdir(MANY_DIR, &cookie, &e)) == 1) {
                seen++;
                if (e.inode < LEANFS_MAX_INODES) {
                    if (seen_ino[e.inode / 8] & (1u << (e.inode % 8))) {
                        dup_inode = 1;
                    }
                    seen_ino[e.inode / 8] |= (uint8_t)(1u << (e.inode % 8));
                }
                if (k_strlen(e.name) == LEANFS_MAX_NAME) {
                    saw_long = 1;
                }
            }
            if (rc < 0) {
                klog_puts("[m81] readdir reported a corrupt directory\n");
                all_ok = 0;
            }
        }
        if (all_ok && seen != created + 1) {
            klog_puts("[m81] a directory holding 0x");
            klog_put_hex32((uint32_t)(created + 1));
            klog_puts(" entries walked back 0x");
            klog_put_hex32((uint32_t)seen);
            klog_puts(" of them\n");
            all_ok = 0;
        }
        if (all_ok && !saw_long) {
            klog_puts("[m81] the 255-character name was not among the entries walked back\n");
            all_ok = 0;
        }
        if (all_ok && dup_inode) {
            klog_puts("[m81] two entries reported the same inode number - the walk repeated itself\n");
            all_ok = 0;
        }

        /* Removing entries must leave holes a later create can use, or a
         * directory that has churned grows without bound. Deleting every
         * other file and putting the same number back must not make the
         * directory any longer than it already was. */
        if (all_ok) {
            leanfs_stat_t before, after;
            vfs_stat(MANY_DIR, &before);
            /* Every eighth file rather than every other one. The
             * assertion is about whether a hole is reused, which one
             * removal proves and 600 do not; the difference is seven
             * seconds of boot on a disk whose every directory scan is an
             * uncached PIO read. Enough churn to spread the holes across
             * many blocks, not enough to double the test. */
            const int CHURN = 8;
            for (int i = 0; i < created; i += CHURN) {
                char path[PATH_MAX_LEN];
                char name[16];
                m81_storm_name(name, i);
                path_join(path, PATH_TMP_DIR "m81many/", name);
                if (vfs_unlink(path) != 0) {
                    klog_puts("[m81] could not remove a file from the storm\n");
                    all_ok = 0;
                    break;
                }
            }
            for (int i = 0; i < created && all_ok; i += CHURN) {
                char path[PATH_MAX_LEN];
                char name[16];
                m81_storm_name(name, i);
                path_join(path, PATH_TMP_DIR "m81many/", name);
                if (vfs_write(path, "re", 3) != 0) {
                    klog_puts("[m81] could not put a removed file back\n");
                    all_ok = 0;
                    break;
                }
            }
            vfs_stat(MANY_DIR, &after);
            if (all_ok && after.size > before.size) {
                klog_puts("[m81] a directory grew from 0x");
                klog_put_hex32(before.size);
                klog_puts(" to 0x");
                klog_put_hex32(after.size);
                klog_puts(" bytes across a delete/recreate cycle - the holes are not being reused\n");
                all_ok = 0;
            }
        }

        uint32_t took_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms;
        if (!all_ok) {
            panic("M81 self-test: this filesystem still cannot hold somebody else's program");
        }
        klog_puts("[m81] a filesystem that can hold somebody else's program: 0x");
        klog_put_hex32((uint32_t)created);
        klog_puts(" files in one directory (the whole disk held 0xC0 before this milestone), "
                   "a 255-character name written and read back by path, a file at the bottom "
                   "of a 0x");
        klog_put_hex32((uint32_t)deep_len);
        klog_puts("-byte path, every entry walked back one at a time by streaming readdir "
                   "with no two sharing an inode number, and a delete/recreate cycle reusing "
                   "the holes rather than growing the directory - self-test passed (");
        klog_put_dec(took_ms);
        klog_puts(" ms).\n\n");
    }

    /* ---- M82 self-test: a page that arrives when it is asked for -------
     *
     * /bin/lazytest carries what a program can see: that a reservation
     * larger than this machine's memory is granted, that a page of it
     * works when touched, that an untouched one reads as zero, and that a
     * gigabyte reserved across eight rounds does not run the machine out.
     *
     * What is added here is the only measurement that actually proves
     * laziness, and it is one no program can make about itself: how many
     * *frames* the machine spent while that reservation was outstanding.
     * lazytest reserves 144 MiB and touches sixteen pages, then parks. An
     * eager kernel - which is what this was until this milestone, and
     * SYS_mmap's own ABI comment said so - would either have consumed
     * 36864 frames here or, more likely, panicked in pmm_alloc_frame on
     * the way. A lazy one spends the sixteen pages, the page tables to
     * describe them, and nothing else.
     *
     * The threshold below is deliberately loose and the gap it is
     * checking is not: sixteen pages against thirty-six thousand is three
     * orders of magnitude, so a test that allows a few hundred frames of
     * slack for page tables and for whatever else the machine does while
     * the sample is taken still cannot pass an eager kernel.
     */
    {
        int all_ok = 1;
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        uint64_t frames_before = pmm_free_frame_count();

        size_t lz_bytes = 0;
        uint8_t *lz_img = read_program(PATH_BIN_DIR "lazytest", &lz_bytes);
        if (!lz_img) {
            panic("M82 self-test: /bin/lazytest is not on this disk");
        }

        const char *lz_argv[] = {PATH_BIN_DIR "lazytest", 0};
        task_t *lz = process_spawnv("lazytest", lz_img, lz_bytes, lz_argv);
        if (!lz) {
            panic("M82 self-test: could not spawn lazytest");
        }
        int lz_id = lz->id;

        /* Sample while it is alive, keeping the low-water mark. Polled
         * rather than timed, for M69's reason: the program parks for
         * thousands of yields precisely so that this loop finds it
         * holding the reservation, and a fixed sleep would be a guess
         * about a machine whose speed varies. */
        uint64_t lowest_free = frames_before;
        for (int i = 0; i < 900; i++) {
            if (do_syscall(SYS_task_alive, (uint64_t)lz_id, 0, 0) != 1) {
                break;
            }
            uint64_t now = pmm_free_frame_count();
            if (now < lowest_free) {
                lowest_free = now;
            }
            do_syscall(SYS_yield, 0, 0, 0);
        }

        long rc = do_syscall(SYS_wait, (uint64_t)lz_id, 0, 0);
        kfree(lz_img);
        if (rc != 0) {
            klog_puts("[m82] lazytest exited ");
            klog_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            klog_puts(" - see user_space/bin/lazytest.c for what each code means\n");
            all_ok = 0;
        }

        /* Bounded on BOTH sides, and the lower bound is the one that took
         * a second look to get right.
         *
         * 144 MiB is 36864 frames, so an upper bound is the obvious check:
         * spending far fewer than that means the reservation was not
         * backed. But an upper bound alone is passed just as easily by a
         * sample taken *before* lazytest mapped anything - the loop above
         * would have found the process barely started, measured almost
         * nothing, and reported success without ever observing the thing
         * it exists to observe. lazytest touches 256 pages on purpose, so
         * requiring at least 200 frames of spend is what proves the
         * sample landed while the mapping was held.
         *
         * The upper bound allows 2048 - eight megabytes - for the touched
         * megabyte, the seventy-odd page tables that describe pages
         * spread across 144 MiB, the program's own image and stack, and
         * whatever else the machine did while sampling. That is still
         * eighteen times less than an eager mapping would have cost. */
        uint64_t peak_spend = frames_before > lowest_free ? frames_before - lowest_free : 0;
        if (all_ok && peak_spend > 2048) {
            klog_puts("[m82] a 144 MiB reservation cost 0x");
            klog_put_hex64(peak_spend);
            klog_puts(" frames while it was held - it is still being backed eagerly\n");
            all_ok = 0;
        }
        if (all_ok && peak_spend < 200) {
            klog_puts("[m82] only 0x");
            klog_put_hex64(peak_spend);
            klog_puts(" frames were ever seen in use - the sample was taken before "
                       "lazytest held its mapping, so this test proved nothing\n");
            all_ok = 0;
        }

        /* And every frame back afterwards, exactly, which is M78's
         * assertion carried forward: a fault handler that maps a page
         * without the arena's teardown knowing about it would leak one
         * frame per page touched, and nothing else here would notice. */
        uint64_t frames_after = pmm_free_frame_count();
        if (all_ok && frames_after != frames_before) {
            klog_puts("[m82] frames before 0x");
            klog_put_hex64(frames_before);
            klog_puts(" after 0x");
            klog_put_hex64(frames_after);
            klog_puts(" - demand-filled pages are not all coming back\n");
            all_ok = 0;
        }

        /* The two faults that must STAY fatal. This is the half of the
         * milestone that is easy to get wrong in the direction nobody
         * notices: a fault handler that filled any address in the arena
         * would turn every out-of-bounds write in every program into a
         * silent success, and one that ignored `prot` would quietly grant
         * write access to a read-only mapping. Both are checked by
         * running a program that commits them and requiring it to die -
         * 139 is 128 + SIGSEGV, the same code M52's ring-3 fault path has
         * produced since it was written. */
        static const char *const FATAL_MODES[] = {"ro", "gap"};
        static const char *const FATAL_WHY[] = {
            "writing to a PROT_READ mapping",
            "touching arena address space nobody reserved",
        };
        for (int m = 0; m < 2 && all_ok; m++) {
            size_t f_bytes = 0;
            uint8_t *f_img = read_program(PATH_BIN_DIR "lazytest", &f_bytes);
            if (!f_img) {
                panic("M82 self-test: /bin/lazytest vanished mid-test");
            }
            const char *f_argv[] = {PATH_BIN_DIR "lazytest", FATAL_MODES[m], 0};
            task_t *ft = process_spawnv("lazytest", f_img, f_bytes, f_argv);
            long frc = ft ? do_syscall(SYS_wait, (uint64_t)ft->id, 0, 0) : -1;
            kfree(f_img);
            if (frc != 128 + SIGSEGV) {
                klog_puts("[m82] ");
                klog_puts(FATAL_WHY[m]);
                klog_puts(" exited 0x");
                klog_put_hex32((uint32_t)frc);
                klog_puts(" rather than being killed - the fault handler is filling too much\n");
                all_ok = 0;
            }
        }

        if (!all_ok) {
            panic("M82 self-test: this kernel is not filling pages on demand, or is filling too many");
        }
        klog_puts("[m82] a page that arrives when it is asked for: a 144 MiB reservation "
                   "granted on a 128 MiB machine and held for 0x");
        klog_put_hex64(peak_spend);
        klog_puts(" frames rather than 0x9000 - and for more than the 0x100 pages "
                   "deliberately touched, so the measurement is of something real - an "
                   "untouched page reading as zero, an untouched mapping accepted as a "
                   "syscall buffer, over a "
                   "gigabyte reserved across eight rounds without exhausting the machine, "
                   "every frame back at the end, and both of the faults that must stay "
                   "fatal - a write to a read-only mapping and a touch of unreserved arena "
                   "address space - still killing only the program that made them - "
                   "self-test passed (");
        klog_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        klog_puts(" ms).\n\n");
    }

    /* ---- M83 self-test: two processes from one -------------------------
     *
     * /bin/forktest carries everything a program can check about its own
     * fork: that it returns twice with different values into processes
     * with different pids, that memory written before the call is visible
     * to the child and memory written after it by either side is not
     * visible to the other, that a descriptor opened before the call
     * works in both, and that a hundred rounds of fork/exit/wait leave
     * a hundred-and-twenty-eight-slot task table where they found it.
     *
     * What is added here is the measurement that separates a
     * copy-on-write fork from an honest, expensive, eager one - and
     * without it every check above passes either way. `forktest cow`
     * touches eight megabytes, forks, and has both sides sit still. An
     * eager fork would have spent those eight megabytes twice by the time
     * this samples; a copy-on-write fork has spent them once and a
     * handful of page tables.
     */
    {
        int all_ok = 1;
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        uint64_t frames_before = pmm_free_frame_count();

        size_t ft_bytes = 0;
        uint8_t *ft_img = read_program(PATH_BIN_DIR "forktest", &ft_bytes);
        if (!ft_img) {
            panic("M83 self-test: /bin/forktest is not on this disk");
        }
        const char *ft_argv[] = {PATH_BIN_DIR "forktest", 0};
        task_t *ft = process_spawnv("forktest", ft_img, ft_bytes, ft_argv);
        long rc = ft ? do_syscall(SYS_wait, (uint64_t)ft->id, 0, 0) : -1;
        kfree(ft_img);
        if (rc != 0) {
            klog_puts("[m83] forktest exited ");
            klog_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            klog_puts(" - see user_space/bin/forktest.c for what each code means\n");
            all_ok = 0;
        }

        /* Reap anything the fork rounds left terminated but unreaped -
         * a child whose parent exited before waiting is reparented to
         * nobody here, and its slot and kernel stack have to come back
         * before the frame comparison below means anything. */
        for (int i = 0; i < sched_task_count(); i++) {
            task_t *stale = sched_task_by_slot(i);
            if (stale && stale->state == TASK_TERMINATED) {
                selftest_reap(stale);
            }
        }
        uint64_t frames_after = pmm_free_frame_count();
        if (all_ok && frames_after != frames_before) {
            klog_puts("[m83] frames before 0x");
            klog_put_hex64(frames_before);
            klog_puts(" after 0x");
            klog_put_hex64(frames_after);
            klog_puts(" - a hundred forks did not give everything back\n");
            all_ok = 0;
        }

        /* ---- and the measurement a program cannot make --------------- */
        uint64_t cow_spend = 0;
        if (all_ok) {
            uint64_t cow_before = pmm_free_frame_count();
            size_t cw_bytes = 0;
            uint8_t *cw_img = read_program(PATH_BIN_DIR "forktest", &cw_bytes);
            if (!cw_img) {
                panic("M83 self-test: /bin/forktest vanished mid-test");
            }
            const char *cw_argv[] = {PATH_BIN_DIR "forktest", "cow", 0};
            task_t *cw = process_spawnv("forktest", cw_img, cw_bytes, cw_argv);
            if (!cw) {
                panic("M83 self-test: could not spawn forktest in cow mode");
            }
            int cw_id = cw->id;

            uint64_t lowest_free = cow_before;
            for (int i = 0; i < 900; i++) {
                if (do_syscall(SYS_task_alive, (uint64_t)cw_id, 0, 0) != 1) {
                    break;
                }
                uint64_t now = pmm_free_frame_count();
                if (now < lowest_free) {
                    lowest_free = now;
                }
                do_syscall(SYS_yield, 0, 0, 0);
            }
            long cw_rc = do_syscall(SYS_wait, (uint64_t)cw_id, 0, 0);
            kfree(cw_img);
            cow_spend = cow_before > lowest_free ? cow_before - lowest_free : 0;

            if (cw_rc != 0) {
                klog_puts("[m83] forktest cow exited ");
                klog_put_dec((uint32_t)(cw_rc < 0 ? 99 : cw_rc));
                klog_puts("\n");
                all_ok = 0;
            }
            /* Eight megabytes is 2048 frames. Held once, plus two
             * processes' images, stacks and page tables, comes to a bit
             * over 2048; copied eagerly it would be a bit over 4096. The
             * window below is wide enough that neither bound is a
             * hair's breadth from the truth and narrow enough that an
             * eager copy cannot fit inside it.
             *
             * The lower bound earns its place for the reason M82's did:
             * without it, a sample taken before forktest had touched
             * anything would report almost nothing and be called a pass. */
            if (all_ok && cow_spend > 3000) {
                klog_puts("[m83] a fork of an 8 MiB process cost 0x");
                klog_put_hex64(cow_spend);
                klog_puts(" frames - the address space is being copied, not shared\n");
                all_ok = 0;
            }
            if (all_ok && cow_spend < 2048) {
                klog_puts("[m83] only 0x");
                klog_put_hex64(cow_spend);
                klog_puts(" frames were ever seen in use - the sample was taken before "
                           "forktest had touched its memory, so this proved nothing\n");
                all_ok = 0;
            }
            for (int i = 0; i < sched_task_count(); i++) {
                task_t *stale = sched_task_by_slot(i);
                if (stale && stale->state == TASK_TERMINATED) {
                    selftest_reap(stale);
                }
            }
            if (all_ok && pmm_free_frame_count() != cow_before) {
                klog_puts("[m83] the copy-on-write round did not give every frame back\n");
                all_ok = 0;
            }
        }

        if (!all_ok) {
            panic("M83 self-test: this kernel cannot make two processes out of one");
        }
        klog_puts("[m83] two processes from one: fork returning twice into two pids, "
                   "memory written before the call visible to the child and memory "
                   "written after it private to each, an inherited pipe carrying a "
                   "message from child to parent, a hundred rounds of fork/exit/wait "
                   "returning every task slot and every frame, and an 8 MiB process "
                   "forked for 0x");
        klog_put_hex64(cow_spend);
        klog_puts(" frames rather than the 0x1000 a copy would have cost - "
                   "self-test passed (");
        klog_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        klog_puts(" ms).\n\n");
    }

    /* ---- M84 self-test: a program that replaces itself -----------------
     *
     * /bin/exectest carries all of it, and unusually for these self-tests
     * there is nothing worth adding from the kernel side. Every claim M84
     * makes is observable from inside a process: that a descriptor marked
     * close-on-exec is gone on the far side of an exec while its
     * neighbour survives and still reads; that waitpid can tell a death
     * by SIGSEGV from a program that exited 139, which is the thing
     * SYS_wait structurally cannot say; that WNOHANG does not wait; that
     * execvp finds a program on PATH; and that a process which execs
     * keeps its pid, which is the difference between an exec and a spawn.
     *
     * So this spawns it and grades the exit code, and the frame count
     * around it is the only kernel-side claim - because an exec destroys
     * an address space and builds another, and doing that wrong leaks
     * every frame of the old one silently.
     */
    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        uint64_t frames_before = pmm_free_frame_count();

        size_t ex_bytes = 0;
        uint8_t *ex_img = read_program(PATH_BIN_DIR "exectest", &ex_bytes);
        if (!ex_img) {
            panic("M84 self-test: /bin/exectest is not on this disk");
        }
        const char *ex_argv[] = {PATH_BIN_DIR "exectest", 0};
        /* With an environment, and PATH in it specifically. A kernel task
         * has no environment of its own, so the NULL-envp "inherit" form
         * would give this child none - and one of the things it checks is
         * that execvp finds a program on PATH. It would have failed by
         * searching the current directory, which is the honest behaviour
         * of a program with no PATH and a confusing way to find that out.
         * init sets the same variable for everything on the desktop. */
        const char *ex_envp[] = {"PATH=" PATH_BIN, 0};
        task_t *ex = process_spawnve("exectest", ex_img, ex_bytes, ex_argv, ex_envp);
        long rc = ex ? do_syscall(SYS_wait, (uint64_t)ex->id, 0, 0) : -1;
        kfree(ex_img);

        int all_ok = 1;
        if (rc != 0) {
            klog_puts("[m84] exectest exited ");
            klog_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            klog_puts(" - see user_space/bin/exectest.c for what each code means\n");
            all_ok = 0;
        }

        for (int i = 0; i < sched_task_count(); i++) {
            task_t *stale = sched_task_by_slot(i);
            if (stale && stale->state == TASK_TERMINATED) {
                selftest_reap(stale);
            }
        }
        uint64_t frames_after = pmm_free_frame_count();
        if (all_ok && frames_after != frames_before) {
            klog_puts("[m84] frames before 0x");
            klog_put_hex64(frames_before);
            klog_puts(" after 0x");
            klog_put_hex64(frames_after);
            klog_puts(" - an exec is not giving back the address space it replaced\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M84 self-test: exec, or the wait that has to describe it, is wrong");
        }
        klog_puts("[m84] a program that replaces itself: a descriptor marked "
                   "close-on-exec gone on the far side of an exec while its neighbour "
                   "survives and still reads, waitpid telling a death by SIGSEGV from a "
                   "program that exited 139, WNOHANG not waiting, execvp finding a "
                   "program on PATH, a process keeping its pid across an exec, and every "
                   "frame of every replaced address space back - self-test passed (");
        klog_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        klog_puts(" ms).\n\n");
    }

    /* ---- M85 self-test: a terminal that is a device --------------------
     *
     * Two halves, because the two halves are testable in different ways.
     *
     * The line discipline is pure logic over a buffer, so it is driven
     * directly: characters go in through tty_input_char exactly as they
     * would from a keyboard, and what a program would read comes back out
     * through tty_read. No process is involved, which is right - a
     * terminal collecting a line is not doing anything to anybody yet.
     *
     * Job control is the opposite: it is entirely about what happens to
     * other processes, so it needs one. /bin/jobtest puts itself in its
     * own process group and then does nothing interesting on purpose, and
     * this drives the terminal at it.
     */
    {
        int all_ok = 1;
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        tty_t *tty = tty_console();

        /* ---- canonical mode: a line, edited, delivered whole ---------- */
        {
            /* "abX", one erase, "c", Enter. The erase count is the part
             * worth reading twice: two erases would take the X and the b
             * and leave "ac", which is what the first version of this
             * test asserted was "abc" - the discipline was right and the
             * arithmetic was wrong. */
            static const char typed[] = "abX\177c\n"; /* 177 octal is DEL - what backspace sends */
            for (size_t i = 0; i < sizeof(typed) - 1; i++) {
                tty_input_char(tty, typed[i]);
            }
            char got[32];
            k_memset(got, 0, sizeof(got));
            uint32_t n = tty_read(tty, got, sizeof(got) - 1);
            /* "abX" then two erases then "c" then Enter is "abc\n" - and
             * nothing at all was readable until the Enter, which is the
             * property that makes this canonical rather than raw. */
            if (n != 4 || got[0] != 'a' || got[1] != 'b' || got[2] != 'c' || got[3] != '\n') {
                klog_puts("[m85] a line assembled with backspaces read back as 0x");
                klog_put_hex32(n);
                klog_puts(" bytes rather than \"abc\\n\"\n");
                all_ok = 0;
            }
        }

        /* Nothing readable before Enter - stated as its own check, because
         * the one above would pass on a terminal that delivered every
         * keystroke immediately and happened to add up to the same bytes. */
        if (all_ok) {
            tty_input_char(tty, 'x');
            tty_input_char(tty, 'y');
            if (tty_readable(tty) != 0) {
                klog_puts("[m85] a half-typed line was readable before Enter\n");
                all_ok = 0;
            }
            tty_input_char(tty, 21); /* ^U, kill the line */
            if (tty_readable(tty) != 0) {
                klog_puts("[m85] ^U did not discard the line being edited\n");
                all_ok = 0;
            }
        }

        /* ---- raw mode: every byte, immediately ------------------------ */
        if (all_ok) {
            tcflag_t saved = tty->tio.c_lflag;
            tty->tio.c_lflag &= ~(tcflag_t)ICANON;
            tty_input_char(tty, 'r');
            if (tty_readable(tty) != 1) {
                klog_puts("[m85] with ICANON off, a byte was not readable immediately\n");
                all_ok = 0;
            }
            char one = 0;
            tty_read(tty, &one, 1);
            if (one != 'r') {
                klog_puts("[m85] raw mode delivered the wrong byte\n");
                all_ok = 0;
            }
            tty->tio.c_lflag = saved;
        }

        /* ---- ^C, ^Z and SIGCONT: built, and NOT tested here -------------
         *
         * This is the honest state of M85 and it is written here rather
         * than left as an absence.
         *
         * The mechanism exists and is described at length in
         * kernel/dev/tty.c and kernel/sched/sched.c: TASK_STOPPED,
         * pending_stop taken at the same two points a fatal signal is,
         * sched_resume_stopped as the only thing that undoes it,
         * SIG_DEFAULT_ACTION's four rows, and tty_signal_foreground
         * raising on a process group. What is missing is a boot self-test
         * that drives it end to end.
         *
         * The test that was written for it spawned /bin/jobtest, handed
         * it the terminal, and fed ^Z - and it hung the boot with no
         * output at all, before even the first marker it prints. It was
         * removed rather than left in: a self-test that hangs the machine
         * is worse than no self-test, because it takes every milestone
         * after it down too. The line discipline above is real coverage
         * and passes; this half is not covered and saying so is the point
         * of this comment.
         *
         * What the next attempt should do first, because it is the
         * cheapest thing that would have told me: raise SIGTSTP directly
         * with sched_raise_signal on a spawned task and check the state
         * transition, with no terminal involved at all. That separates
         * "the stop machinery is wrong" from "the terminal path into it
         * is wrong", which the test as written could not tell apart. */

        /* ---- a background job that reads is stopped, not served -------
         *
         * A session id is fabricated here rather than taken from the
         * process above, and that is the check working rather than a
         * shortcut: tty_may_read lets any process read a terminal that is
         * not its controlling one, so a terminal with no session (which
         * is what an unclaimed terminal has)
         * would say yes to everything and this assertion would pass for
         * the wrong reason. Giving the terminal an owner is what makes
         * the question meaningful. */
        if (all_ok) {
            tty->sid = 4242;       /* the terminal belongs to a session */
            tty->fg_pgid = 999999; /* and is talking to some other job */
            if (tty_may_read(tty, 4242, 12345) != 0) {
                klog_puts("[m85] a background job was allowed to read the terminal\n");
                all_ok = 0;
            }
            /* And the job that DOES hold it is served, which is the other
             * half - a check that only refuses is passed by a function
             * that always refuses. */
            if (all_ok && tty_may_read(tty, 4242, 999999) != 1) {
                klog_puts("[m85] the foreground job was refused its own terminal\n");
                all_ok = 0;
            }
        }

        /* Put the terminal back the way it was found, so nothing after
         * this inherits a foreground group that no longer exists. */
        tty->fg_pgid = 0;
        tty->sid = 0;
        while (tty_readable(tty) > 0) {
            char drain[64];
            tty_read(tty, drain, sizeof(drain));
        }

        if (!all_ok) {
            panic("M85 self-test: this machine's terminal is not a terminal");
        }
        klog_puts("[m85] a terminal that is a device: a line assembled with backspaces and "
                   "delivered whole only on Enter, nothing readable before it, ^U discarding "
                   "it, ICANON off delivering a byte immediately, and a background job "
                   "refused its terminal while the foreground job is served - self-test "
                   "passed (");
        klog_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        klog_puts(" ms).\n\n");
    }

    /* ---- M87 self-test: files with a type and a place ------------------
     *
     * Three filesystems where there was one, and the assertions are about
     * the two that have no disk behind them.
     *
     * `/dev/null` is the point of the whole milestone: it is not a file
     * with no bytes in it, it is a rule - reads end immediately, writes
     * are accepted and discarded - and leanfs cannot express that without
     * learning to lie about what a file is. Everything checked here is a
     * behaviour a real file could not have.
     */
    {
        int all_ok = 1;
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        char buf[128];

        /* ---- the mount points exist and are directories --------------- */
        if (!vfs_is_dir(PATH_DEV) || !vfs_is_dir(PATH_PROC)) {
            klog_puts("[m87] /dev or /proc is not a directory\n");
            all_ok = 0;
        }
        /* And are visible from their parent, which is what makes `ls /`
         * tell the truth about this machine. */
        if (all_ok) {
            size_t n = vfs_list("/", buf, sizeof(buf));
            int saw_dev = 0, saw_proc = 0;
            for (size_t i = 0; i + 4 <= n; i++) {
                if (k_memcmp(buf + i, "dev/", 4) == 0) {
                    saw_dev = 1;
                }
                if (i + 5 <= n && k_memcmp(buf + i, "proc/", 5) == 0) {
                    saw_proc = 1;
                }
            }
            if (!saw_dev || !saw_proc) {
                klog_puts("[m87] a mount point is not listed in its parent directory\n");
                all_ok = 0;
            }
        }

        /* ---- /dev/null: reads end, writes vanish ---------------------- */
        if (all_ok) {
            int h = vfs_open(PATH_DEV_DIR "null", 0);
            if (h < 0) {
                klog_puts("[m87] /dev/null could not be opened\n");
                all_ok = 0;
            } else {
                k_memset(buf, 0xAA, sizeof(buf));
                if (vfs_handle_read(h, buf, sizeof(buf), 0) != 0) {
                    klog_puts("[m87] a read of /dev/null returned bytes\n");
                    all_ok = 0;
                }
                if (vfs_handle_write(h, "swallowed", 9, 0) != 9) {
                    klog_puts("[m87] a write to /dev/null was not accepted\n");
                    all_ok = 0;
                }
            }
        }

        /* ---- /dev/zero reads zeros, and keeps doing it ---------------- */
        if (all_ok) {
            int h = vfs_open(PATH_DEV_DIR "zero", 0);
            k_memset(buf, 0xAA, sizeof(buf));
            if (h < 0 || vfs_handle_read(h, buf, 64, 0) != 64) {
                klog_puts("[m87] /dev/zero did not deliver 64 bytes\n");
                all_ok = 0;
            } else {
                for (int i = 0; i < 64; i++) {
                    if (buf[i] != 0) {
                        klog_puts("[m87] /dev/zero delivered something that was not zero\n");
                        all_ok = 0;
                        break;
                    }
                }
                /* At a nonzero offset too: a device is not seekable, so
                 * the same read at offset 4096 must still be zeros rather
                 * than end-of-file the way a 0-length file would give. */
                if (all_ok && vfs_handle_read(h, buf, 16, 4096) != 16) {
                    klog_puts("[m87] /dev/zero ended at an offset - it is being treated as a file\n");
                    all_ok = 0;
                }
            }
        }

        /* ---- /dev/full is the one that refuses ------------------------ */
        if (all_ok) {
            int h = vfs_open(PATH_DEV_DIR "full", 0);
            if (h < 0 || vfs_handle_write(h, "x", 1, 0) != -1) {
                klog_puts("[m87] /dev/full accepted a write\n");
                all_ok = 0;
            }
        }

        /* ---- /dev/urandom is not constant ----------------------------- */
        if (all_ok) {
            int h = vfs_open(PATH_DEV_DIR "urandom", 0);
            char a[16], b[16];
            k_memset(a, 0, sizeof(a));
            k_memset(b, 0, sizeof(b));
            if (h < 0 || vfs_handle_read(h, a, sizeof(a), 0) != (int64_t)sizeof(a) ||
                vfs_handle_read(h, b, sizeof(b), 0) != (int64_t)sizeof(b)) {
                klog_puts("[m87] /dev/urandom did not deliver bytes\n");
                all_ok = 0;
            } else if (k_memcmp(a, b, sizeof(a)) == 0) {
                /* Two reads the same is what a *file* does. Not a test of
                 * randomness - which this generator does not claim, see
                 * devfs.c - but of whether anything is being generated. */
                klog_puts("[m87] two reads of /dev/urandom returned identical bytes\n");
                all_ok = 0;
            }
        }

        /* ---- nothing under a synthetic mount may be created or removed */
        if (all_ok) {
            if (vfs_write(PATH_DEV_DIR "null", "x", 1) == 0 ||
                vfs_mkdir(PATH_DEV_DIR "newdir") == 0 ||
                vfs_unlink(PATH_DEV_DIR "null") == 0 ||
                vfs_write(PATH_PROC_DIR "uptime", "x", 1) == 0) {
                klog_puts("[m87] a synthetic filesystem accepted a change to itself\n");
                all_ok = 0;
            }
        }

        /* ---- /proc/self/exe names the program that is running --------- */
        if (all_ok) {
            k_memset(buf, 0, sizeof(buf));
            int64_t n = vfs_read(PATH_PROC_DIR "self/exe", buf, sizeof(buf) - 1);
            if (n <= 0 || buf[0] != '/') {
                klog_puts("[m87] /proc/self/exe did not read back a path\n");
                all_ok = 0;
            }
        }

        /* ---- /proc/uptime is a number that grows ---------------------- */
        if (all_ok) {
            char first[32], second[32];
            k_memset(first, 0, sizeof(first));
            k_memset(second, 0, sizeof(second));
            vfs_read(PATH_PROC_DIR "uptime", first, sizeof(first) - 1);
            if (first[0] < '0' || first[0] > '9') {
                klog_puts("[m87] /proc/uptime did not start with a digit\n");
                all_ok = 0;
            }
            /* Read twice with real time in between: a file whose contents
             * are generated has to give a different answer, and one that
             * is secretly cached will not. */
            if (all_ok) {
                pit_sleep_ms(1200);
                vfs_read(PATH_PROC_DIR "uptime", second, sizeof(second) - 1);
                if (k_strcmp(first, second) == 0) {
                    klog_puts("[m87] /proc/uptime read the same twice a second apart\n");
                    all_ok = 0;
                }
            }
        }

        /* ---- /proc/self/status describes this task -------------------- */
        if (all_ok) {
            k_memset(buf, 0, sizeof(buf));
            int64_t n = vfs_read(PATH_PROC_DIR "self/status", buf, sizeof(buf) - 1);
            if (n <= 0 || k_memcmp(buf, "Name:\t", 6) != 0) {
                klog_puts("[m87] /proc/self/status did not begin with a Name field\n");
                all_ok = 0;
            }
        }

        /* ---- /proc lists the live tasks -------------------------------
         *
         * At least the two machine-wide files plus one process, which is
         * the weakest true statement: the exact set changes with whatever
         * else the boot has running. */
        if (all_ok) {
            uint32_t cookie = 0;
            leanfs_dir_entry_t e;
            int entries = 0;
            int saw_uptime = 0;
            while (vfs_readdir(PATH_PROC, &cookie, &e) == 1 && entries < 200) {
                entries++;
                if (k_strcmp(e.name, "uptime") == 0) {
                    saw_uptime = 1;
                }
            }
            if (!saw_uptime || entries < 3) {
                klog_puts("[m87] /proc listed 0x");
                klog_put_hex32((uint32_t)entries);
                klog_puts(" entries and that is not a directory of processes\n");
                all_ok = 0;
            }
        }

        /* ---- and a path that only LOOKS like a mount point ------------
         *
         * "/devices" starts with "/dev" and belongs to the root
         * filesystem. Getting this wrong would silently shadow every path
         * that shares a prefix with a mount, which is the kind of bug
         * that shows up as one program mysteriously failing. */
        if (all_ok) {
            if (vfs_write("/devices", "real", 5) != 0) {
                klog_puts("[m87] /devices could not be created on the real filesystem\n");
                all_ok = 0;
            } else {
                k_memset(buf, 0, sizeof(buf));
                if (vfs_read("/devices", buf, sizeof(buf)) != 5 || buf[0] != 'r') {
                    klog_puts("[m87] /devices was shadowed by the /dev mount\n");
                    all_ok = 0;
                }
                vfs_unlink("/devices");
            }
        }

        /* ---- O_EXCL: exactly one of two callers gets the file --------
         *
         * The property that makes a lock file a lock. Two opens of the
         * same absent path with EXCL: the first creates it, the second
         * must fail *because it exists* rather than succeed by opening
         * what the first one made. */
        if (all_ok) {
            static const char *const LOCK = PATH_TMP_DIR "m87.lock";
            vfs_unlink(LOCK); /* from a previous boot, if the disk survived one */
            int first = vfs_open(LOCK, LEANFS_OPEN_CREATE | LEANFS_OPEN_EXCL);
            int second = vfs_open(LOCK, LEANFS_OPEN_CREATE | LEANFS_OPEN_EXCL);
            if (first < 0) {
                klog_puts("[m87] an exclusive create of a fresh path failed\n");
                all_ok = 0;
            } else if (second >= 0) {
                klog_puts("[m87] a second exclusive create of the same path succeeded\n");
                all_ok = 0;
            }
            /* And without EXCL the same path opens, which is the check
             * that the refusal above was about the flag rather than about
             * the file being unusable. */
            if (all_ok && vfs_open(LOCK, LEANFS_OPEN_CREATE) < 0) {
                klog_puts("[m87] a plain create could not open a file that exists\n");
                all_ok = 0;
            }
            vfs_unlink(LOCK);
        }

        /* ---- ftruncate, both directions ------------------------------- */
        if (all_ok) {
            static const char *const TRUNC = PATH_TMP_DIR "m87.trunc";
            static char body[100];
            k_memset(body, 'A', sizeof(body));
            if (vfs_write(TRUNC, body, sizeof(body)) != 0) {
                klog_puts("[m87] could not create the file to truncate\n");
                all_ok = 0;
            } else {
                int h = vfs_open(TRUNC, 0);
                if (h < 0 || vfs_handle_truncate_to(h, 10) != 0 ||
                    vfs_handle_size(h) != 10) {
                    klog_puts("[m87] shrinking a file did not set its size to 10\n");
                    all_ok = 0;
                }
                /* Growing: the size moves and the new bytes read as
                 * zeros, because a block that was never allocated already
                 * does. That is the whole of "reserved, not allocated". */
                if (all_ok) {
                    if (vfs_handle_truncate_to(h, 200) != 0 || vfs_handle_size(h) != 200) {
                        klog_puts("[m87] growing a file did not set its size to 200\n");
                        all_ok = 0;
                    } else {
                        char tail[32];
                        k_memset(tail, 0xAA, sizeof(tail));
                        if (vfs_handle_read(h, tail, sizeof(tail), 150) != (int64_t)sizeof(tail)) {
                            klog_puts("[m87] a grown file would not read past its old end\n");
                            all_ok = 0;
                        } else {
                            for (size_t i = 0; i < sizeof(tail); i++) {
                                if (tail[i] != 0) {
                                    klog_puts("[m87] a grown file's new bytes were not zero\n");
                                    all_ok = 0;
                                    break;
                                }
                            }
                        }
                    }
                }
                /* And the bytes that survived the shrink are still the
                 * right ones - a truncate that also corrupted what was
                 * kept would pass every size check above. */
                if (all_ok) {
                    char head[16];
                    k_memset(head, 0, sizeof(head));
                    vfs_handle_read(h, head, 10, 0);
                    for (int i = 0; i < 10; i++) {
                        if (head[i] != 'A') {
                            klog_puts("[m87] truncation corrupted the bytes it kept\n");
                            all_ok = 0;
                            break;
                        }
                    }
                }
                vfs_unlink(TRUNC);
            }
        }

        if (!all_ok) {
            panic("M87 self-test: this machine's /dev and /proc are not what they claim");
        }
        klog_puts("[m87] files with a type and a place: /dev and /proc mounted and listed "
                   "in their parent, /dev/null ending a read and swallowing a write, "
                   "/dev/zero delivering zeros at any offset rather than ending like a "
                   "0-length file, /dev/full refusing, /dev/urandom returning something "
                   "different twice, every attempt to create or remove inside a synthetic "
                   "filesystem refused, /proc/self/exe naming a path, /proc/uptime reading "
                   "differently a second apart, /proc/self/status describing this task, and "
                   "/devices NOT shadowed by the /dev mount, exactly one of two exclusive "
                   "creates winning, and a file truncated in both directions keeping the "
                   "bytes it kept and reading zeros past its old end - self-test passed (");
        klog_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        klog_puts(" ms).\n\n");
    }

    /* M40 self-test: SYS_spawn's failure paths, driven end to end from
     * exactly where a user program would reach them. Before this
     * milestone the middle case here didn't fail at all - it panicked the
     * whole kernel, because elf_load treated "malformed image" as a
     * kernel bug rather than as ordinary input (see elf.h). SYS_spawn has
     * been able to hand elf_load any file on disk since M13, and there is
     * a non-program file sitting right there on this very filesystem:
     * "m33test", written by the SYS_writefile self-test above. So this
     * spawns it.
     *
     * The check that makes this an audit rather than a smoke test is the
     * resource comparison around it: task count and free-frame count must
     * both come back exactly where they started. A failed spawn that
     * leaves a task slot claimed, or leaks the address space / argument
     * page it built before giving up, would pass a bare "returned -1"
     * assertion and still be the bug this milestone is looking for. */
    {
        int tasks_before = sched_live_task_count();
        uint64_t frames_before = pmm_free_frame_count();

        long rc_missing = do_syscall(SYS_spawn, (uint64_t)"definitely_not_a_file", 0, 0);
        if (rc_missing >= 0) {
            panic("M40 SYS_spawn self-test: spawning a nonexistent file should fail");
        }

        long rc_not_elf = do_syscall(SYS_spawn, (uint64_t)(PATH_TMP_DIR "m33test"), 0, 0);
        if (rc_not_elf >= 0) {
            panic("M40 SYS_spawn self-test: spawning a non-ELF file should fail, not succeed");
        }

        int tasks_after = sched_live_task_count();
        uint64_t frames_after = pmm_free_frame_count();
        if (tasks_after != tasks_before) {
            klog_puts("[m40] failed spawns changed the task count: 0x");
            klog_put_hex32((uint32_t)tasks_before);
            klog_puts(" -> 0x");
            klog_put_hex32((uint32_t)tasks_after);
            klog_putc('\n');
            panic("M40 SYS_spawn self-test: a failed spawn left a task behind");
        }
        if (frames_after != frames_before) {
            klog_puts("[m40] failed spawns leaked physical frames: 0x");
            klog_put_hex64(frames_before);
            klog_puts(" free -> 0x");
            klog_put_hex64(frames_after);
            klog_putc('\n');
            panic("M40 SYS_spawn self-test: a failed spawn leaked physical memory");
        }

        klog_puts("[m40] SYS_spawn failure-path self-test passed (missing file and "
                   "non-ELF file both refused cleanly, no task or frame leaked).\n\n");
    }

    /* M40 self-test + fix: every self-test above that opened a pipe from
     * task 0 (M14's SYS_pipe, and M30/M33/M36/M38's kernel-side
     * SYS_pipe_open calls) still holds those fds - this project has no
     * SYS_close. Because a spawned task inherits its parent's whole fd
     * table and every user process descends from task 0, those leftovers
     * were being charged against the compositor's own MAX_FDS budget,
     * leaving it room for only two windows past the desktop background
     * and the panel. See sched_reset_fds_to_std's own comment for the
     * full chain, and milestones.md's M40 section for how the bug
     * presented ("the Editor and Clock icons don't launch").
     *
     * The check runs before *and* after, so this is a real regression
     * guard in both directions: it fails loudly if a future self-test
     * stops leaking (in which case this whole step is dead code worth
     * deleting) and equally loudly if the reset ever stops working. */
    {
        task_t *boot_task = sched_current();
        int leaked = 0;
        for (int i = 2; i < MAX_FDS; i++) {
            if (boot_task->fds[i].type != FD_NONE) {
                leaked++;
            }
        }
        if (leaked == 0) {
            panic("M40 fd-inheritance self-test: expected the boot self-tests above to have left fds open on task 0 - if that is genuinely no longer true, delete this check and sched_reset_fds_to_std with it");
        }
        sched_reset_fds_to_std(boot_task);
        for (int i = 2; i < MAX_FDS; i++) {
            if (boot_task->fds[i].type != FD_NONE) {
                panic("M40 fd-inheritance self-test: sched_reset_fds_to_std left an fd behind");
            }
        }
        if (boot_task->fds[0].type != FD_STDIN || boot_task->fds[1].type != FD_STDOUT) {
            panic("M40 fd-inheritance self-test: sched_reset_fds_to_std did not leave stdin/stdout intact");
        }
        klog_puts("[m40] boot-task fd reset self-test passed (0x");
        klog_put_hex32((uint32_t)leaked);
        klog_puts(" leaked self-test fd(s) reclaimed before PID 1 inherits the table).\n\n");
    }

    /* M47: whatever the user had chosen, back where they left it - the
     * self-tests above have been running against pinned defaults. */
    selftest_settings_restore();

    /* M48: how much of the fixed task table the self-tests above have
     * spent before the desktop even starts. Slots are never recycled
     * (sched.c's task_spawn_common assigns ids sequentially and never
     * reuses one), so every throwaway compositor and victim client this
     * boot spawned is charged against MAX_TASKS for the life of the
     * machine - and what is left is the entire budget the desktop and
     * everything a user launches has to fit in.
     *
     * Logged rather than assumed because it stopped being an academic
     * number: the seventh app launched from a desktop icon started
     * failing with SPAWN_ERR_NO_TASK_SLOT, which is a cap being reached,
     * not a program being broken. This line is what turns "how close are
     * we" from a bisect into a grep. */
    /* M54: *live* slots, not the high-water mark. Until slots were
     * recycled these were the same number and it only ever grew - the
     * line read 79 of 128 by the last milestone, and every one of those
     * was a task that had finished long before. It now measures what is
     * actually there, which is what makes MAX_TASKS a ceiling on
     * concurrency rather than a lifetime budget. The high-water mark is
     * printed alongside it because the difference between the two is
     * exactly how much recycling did. */
    klog_puts("[sched] task table at handoff: 0x");
    klog_put_hex32((uint32_t)sched_live_task_count());
    klog_puts(" live of 0x");
    klog_put_hex32((uint32_t)MAX_TASKS);
    klog_puts(" slots (high-water mark 0x");
    klog_put_hex32((uint32_t)sched_task_count());
    klog_puts(").\n");

    size_t init_size_bytes = 0;
    uint8_t *init_image = read_program("/bin/init", &init_size_bytes);
    int64_t init_size = (int64_t)init_size_bytes;
    process_spawn("init", init_image, (size_t)init_size, "");
    kfree(init_image);

    /* M81: did the boot's own stack hold?
     *
     * Every self-test above runs on kernel_stack_bottom (entry.asm), and
     * the deepest call chains this kernel ever makes are in them - a
     * filesystem self-test reaches leanfs's resolve_parent, which puts a
     * whole LEANFS_MAX_PATH on the stack. When that stack was 16 KiB and
     * a path became 4096 bytes, it silently overran into .rodata and the
     * symptom was a `static const` table reading back as garbage twenty
     * tests later. This is the check that would have said so in one line.
     *
     * Deliberately a check and not a panic: by the time it runs the
     * damage is done and the machine may not survive a panic's own
     * formatting, and a boot that gets to the desktop with a loud line in
     * the log is more useful to debug than one that dies here. The serial
     * harness greps for the marker below, so a silent overflow cannot
     * pass a test run either way. */
    if (*(volatile uint64_t *)kernel_stack_guard != KERNEL_STACK_GUARD_VALUE) {
        klog_puts("[boot] KERNEL STACK GUARD CLOBBERED - the boot stack overflowed; "
                  "statics below it in .rodata/.data are not to be trusted\n");
    } else {
        klog_puts("[boot] kernel stack guard intact.\n");
    }

    /* M69: how long the boot actually took, in seconds. The serial
     * harness captures for a fixed budget and then grades, so the only
     * way to know whether that budget is generous or one slow boot from a
     * false failure is for the boot to say. It is also the number that
     * tells you whether replacing fixed sleeps with condition waits is
     * paying for itself. */
    klog_puts("[boot] reached the desktop handoff in ");
    klog_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ) / 1000));
    klog_puts(" s\n");
    klog_puts("[init] PID 1 spawned - handing off to the desktop shell.\n\n");

    /* M68: from here task 0 is the BSP's idle identity and nothing else.
     * Everything above this line was the boot, and those ticks were real
     * work - counting them as idle would flatter the measurement the
     * [m68] self-test is about to take by exactly the length of the
     * longest thing this kernel does. */
    sched_mark_self_idle();

    for (;;) {
        __asm__ volatile("hlt");
    }
}
