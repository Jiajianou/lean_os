#include <stddef.h>
#include <stdint.h>

#include "acpi/acpi.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/smp.h"
#include "drivers/console.h"
#include "drivers/cursor.h"
#include "drivers/fb.h"
#include "drivers/font8x16.h" /* M39 self-test reads the glyph tables and the shared metric directly */
#include "drivers/keyboard.h"
#include "drivers/klog.h"
#include "drivers/mouse.h"
#include "drivers/pit.h"
#include "fs/leanfs.h"
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
    X(ls)                            \
    X(init)                          \
    X(shell)                         \
    X(memtest)                       \
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
    X(reboot)

#define DECLARE_EMBEDDED_PROGRAM(name) \
    extern const uint8_t name##_elf_start[]; \
    extern const uint8_t name##_elf_end[];
FOR_EACH_EMBEDDED_PROGRAM(DECLARE_EMBEDDED_PROGRAM)
#undef DECLARE_EMBEDDED_PROGRAM

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
static void selftest_reap(task_t *t) {
    do_syscall(SYS_kill, (uint64_t)t->id, SIGKILL, 0);
    do_syscall(SYS_wait, (uint64_t)t->id, 0, 0);
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
    "wallpaper=0x00000001\n"; /* WALLPAPER_GRADIENT, its wallpaper_id default */

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

    sched_init();
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
    if (!vfs_exists(PATH_HOME_DIR "readme.txt")) {
        static const char welcome[] =
            "Welcome to lean_os.\n"
            "\n"
            "This is /home - your files live here.\n"
            "Programs live in /bin, settings in /etc.\n"
            "\n"
            "Double-click a name in Files to open it,\n"
            "or .. to go up a directory.\n";
        if (vfs_write(PATH_HOME_DIR "readme.txt", welcome, sizeof(welcome) - 1) != 0) {
            panic("vfs_write: failed to seed " PATH_HOME_DIR "readme.txt");
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
        uint8_t *memtest_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!memtest_image) {
            panic("out of memory reading memtest back from disk");
        }
        int64_t memtest_size = vfs_read("/bin/memtest", memtest_image, LEANFS_MAX_FILE_SIZE);
        if (memtest_size < 0) {
            panic("vfs_read(\"memtest\") failed - should exist, just seeded");
        }
        task_t *memtest_task = process_spawn("memtest", memtest_image, (size_t)memtest_size, "");
        kfree(memtest_image);
        long memtest_status = do_syscall(SYS_wait, (uint64_t)memtest_task->id, 0, 0);
        if (memtest_status != 0) {
            panic("memtest self-test: nonzero exit code - malloc or shm is broken");
        }
        klog_puts("[memtest] user-space malloc/free and cross-process shm self-tests passed.\n\n");
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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *demo_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !demo_image) {
            panic("out of memory reading compositor/wm_demo back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t demo_size = vfs_read("/bin/wm_demo", demo_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || demo_size < 0) {
            panic("vfs_read: compositor/wm_demo missing - should exist, just seeded");
        }

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
        for (int spin = 0; spin < 300 && demo_status == 1; spin++) {
            pit_sleep_ms(10);
            demo_status = do_syscall(SYS_task_alive, (uint64_t)demo_task->id, 0, 0);
        }
        if (demo_status != 2) {
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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *clock_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *paint_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !clock_image || !paint_image) {
            panic("out of memory reading compositor/gui_clock/gui_paint back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t clock_size = vfs_read("/bin/gui_clock", clock_image, LEANFS_MAX_FILE_SIZE);
        int64_t paint_size = vfs_read("/bin/gui_paint", paint_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || clock_size < 0 || paint_size < 0) {
            panic("vfs_read: compositor/gui_clock/gui_paint missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200); /* let the compositor map the fb and open its request/response pipes before any client tries to connect */

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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *shell_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *clock_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !shell_image || !clock_image) {
            panic("out of memory reading compositor/desktop_shell/gui_clock back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t shell_size = vfs_read("/bin/desktop_shell", shell_image, LEANFS_MAX_FILE_SIZE);
        int64_t clock_size = vfs_read("/bin/gui_clock", clock_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || shell_size < 0 || clock_size < 0) {
            panic("vfs_read: compositor/desktop_shell/gui_clock missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *clock_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !clock_image) {
            panic("out of memory reading compositor/gui_clock back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t clock_size = vfs_read("/bin/gui_clock", clock_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || clock_size < 0) {
            panic("vfs_read: compositor/gui_clock missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

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
        pit_sleep_ms(300);
        uint32_t after_maximize = fb_get_pixel((uint32_t)home_x, (uint32_t)home_y);

        req.action = WM_ACTION_RESTORE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        pit_sleep_ms(300);
        uint32_t after_restore = fb_get_pixel((uint32_t)home_x, (uint32_t)home_y);

        req.action = WM_ACTION_TOGGLE_MINIMIZE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        pit_sleep_ms(300);
        uint32_t after_minimize = fb_get_pixel((uint32_t)home_x, (uint32_t)home_y);

        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req)); /* toggle back */
        pit_sleep_ms(300);
        uint32_t after_unminimize = fb_get_pixel((uint32_t)home_x, (uint32_t)home_y);

        req.action = WM_ACTION_CLOSE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        /* Signal delivery isn't instantaneous (checked at the target's
         * next syscall/tick - signal.h), and M29's reap_dead_clients only
         * runs once per compositor loop iteration after that - 300ms is
         * comfortably many iterations either way. */
        pit_sleep_ms(300);
        uint32_t after_close = fb_get_pixel((uint32_t)home_x, (uint32_t)home_y);
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
    }

    /* M33 self-test: settings.c's live desktop-background-color control,
     * driven directly over WM_SETTINGS_PIPE the same way M30's self-test
     * drives WM_ACTION_PIPE - no GUI client needed, the compositor alone
     * already redraws the desktop background every frame regardless of
     * whether anything is connected to it. */
    {
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image) {
            panic("out of memory reading compositor back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0) {
            panic("vfs_read: compositor missing - should exist, just seeded");
        }
        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(300);

        int settings_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_SETTINGS_PIPE, (uint64_t)settings_fds, 0) != 0) {
            panic("M33 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_PIPE) failed");
        }
        wm_settings_request_t req;
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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *editor_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !editor_image) {
            panic("out of memory reading compositor/text_editor back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t editor_size = vfs_read("/bin/text_editor", editor_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || editor_size < 0) {
            panic("vfs_read: compositor/text_editor missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

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
        pit_sleep_ms(400);
        uint32_t after_close = fb_get_pixel((uint32_t)probe_x, (uint32_t)probe_y);
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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *clock_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !clock_image) {
            panic("out of memory reading compositor/gui_clock back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t clock_size = vfs_read("/bin/gui_clock", clock_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || clock_size < 0) {
            panic("vfs_read: compositor/gui_clock missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

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
        uint32_t shadow_pixel = fb_get_pixel(304, 150);
        uint32_t expected_shadow = 0x000D0D17u;

        int settings_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_SETTINGS_PIPE, (uint64_t)settings_fds, 0) != 0) {
            panic("M38 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_PIPE) failed");
        }
        wm_settings_request_t req;
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
         * NET_GATEWAY_IP) - exercises the whole stack end to end (NIC
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
        icmp_send_echo_request(NET_GATEWAY_IP, ping_id, ping_seq, ping_payload, sizeof(ping_payload));

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
        klog_put_hex32(NET_GATEWAY_IP);
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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *shell_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *clock_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !shell_image || !clock_image) {
            panic("out of memory reading compositor/desktop_shell/gui_clock back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t shell_size = vfs_read("/bin/desktop_shell", shell_image, LEANFS_MAX_FILE_SIZE);
        int64_t clock_size = vfs_read("/bin/gui_clock", clock_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || shell_size < 0 || clock_size < 0) {
            panic("vfs_read: compositor/desktop_shell/gui_clock missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

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
        uint32_t tray_sep_px = fb_get_pixel(932, 750);

        int action_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_ACTION_PIPE, (uint64_t)action_fds, 0) != 0) {
            panic("M42 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        wm_action_request_t req;
        k_memset(&req, 0, sizeof(req));
        req.window_id = 1; /* the clock - the panel took window 0 */
        req.action = WM_ACTION_MAXIMIZE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        pit_sleep_ms(400);
        /* content_top_limit is TITLEBAR_H(20) + BORDER(2) = 22 now that no
         * top-docked bar exists any more, so a maximized window's content
         * starts at y=22 and its titlebar occupies y:[2, 22). x=100 is
         * inside that titlebar (gui_clock's buffer is 200 wide, and
         * maximize never grows a window past its own buffer), past the
         * "Clock" title text and well left of the three buttons. */
        uint32_t maximized_titlebar = fb_get_pixel(100, 12);
        uint32_t panel_over_maximized = fb_get_pixel(512, 738);

        /* The launcher: no window_id at all (see WM_ACTION_TOGGLE_LAUNCHER),
         * and the overlay is centered horizontally and a third of the way
         * down, so (512, 309) is inside it and clear of its own title
         * text - and is bare desktop background when it's closed. */
        k_memset(&req, 0, sizeof(req));
        req.window_id = -1;
        req.action = WM_ACTION_TOGGLE_LAUNCHER;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        pit_sleep_ms(400);
        uint32_t launcher_open_px = fb_get_pixel(512, 309);
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        pit_sleep_ms(400);
        uint32_t launcher_closed_px = fb_get_pixel(512, 309);

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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *shell_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *editor_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !shell_image || !editor_image) {
            panic("out of memory reading compositor/desktop_shell/text_editor back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t shell_size = vfs_read("/bin/desktop_shell", shell_image, LEANFS_MAX_FILE_SIZE);
        int64_t editor_size = vfs_read("/bin/text_editor", editor_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || shell_size < 0 || editor_size < 0) {
            panic("vfs_read: compositor/desktop_shell/text_editor missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);
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
        pit_sleep_ms(400);
        uint32_t right_titlebar = fb_get_pixel(700, 12);
        uint32_t right_left_half = fb_get_pixel(200, 12);

        req.action = WM_ACTION_SNAP_LEFT;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        pit_sleep_ms(400);
        uint32_t left_titlebar = fb_get_pixel(200, 12);
        uint32_t left_right_half = fb_get_pixel(700, 12);

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
        pit_sleep_ms(400);
        uint32_t launcher_bg = fb_get_pixel(700, 309);
        uint32_t launcher_selected_row = fb_get_pixel(700, 205);
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&req, sizeof(req));
        pit_sleep_ms(400);
        uint32_t launcher_closed = fb_get_pixel(700, 309);

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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *icons_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *shell_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !icons_image || !shell_image) {
            panic("out of memory reading compositor/desktop_icons/desktop_shell back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t icons_size = vfs_read("/bin/desktop_icons", icons_image, LEANFS_MAX_FILE_SIZE);
        int64_t shell_size = vfs_read("/bin/desktop_shell", shell_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || icons_size < 0 || shell_size < 0) {
            panic("vfs_read: compositor/desktop_icons/desktop_shell missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);
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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *stub_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !stub_image) {
            panic("out of memory reading compositor/wm_stubborn back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t stub_size = vfs_read("/bin/wm_stubborn", stub_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || stub_size < 0) {
            panic("vfs_read: compositor/wm_stubborn missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

        uint64_t frames_before_victim = pmm_free_frame_count();
        task_t *victim = process_spawn("wm_stubborn", stub_image, (size_t)stub_size, "");
        pit_sleep_ms(600); /* connects as window 0, focused, and fills its buffer */

        uint32_t victim_pixel = fb_get_pixel(200, 150);
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
        pit_sleep_ms(400); /* then for reap_dead_clients to notice and repaint */
        uint32_t after_kill_pixel = fb_get_pixel(200, 150);
        do_syscall(SYS_wait, (uint64_t)victim->id, 0, 0); /* now it can be reaped - see selftest_reap */
        uint64_t frames_after_kill = pmm_free_frame_count();

        /* (3) the reclaim. A second client connecting now must be handed
         * the slot the first one gave up, and must be able to draw
         * through it - which it can only do if the shm segment and the
         * event pipe behind that slot are both live again. */
        task_t *victim2 = process_spawn("wm_stubborn", stub_image, (size_t)stub_size, "");
        kfree(stub_image);
        pit_sleep_ms(700);
        uint32_t reused_slot_pixel = fb_get_pixel(200, 150);

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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *clock_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *stub_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !clock_image || !stub_image) {
            panic("out of memory reading compositor/gui_clock/wm_stubborn back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t clock_size = vfs_read("/bin/gui_clock", clock_image, LEANFS_MAX_FILE_SIZE);
        int64_t stub_size = vfs_read("/bin/wm_stubborn", stub_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || clock_size < 0 || stub_size < 0) {
            panic("vfs_read: compositor/gui_clock/wm_stubborn missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);
        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(700); /* connects as window 0, focused, and draws */

        uint32_t focused_corner = fb_get_pixel(246, 83);
        uint32_t focused_disc = fb_get_pixel(250, 90);
        uint32_t focused_glyph = fb_get_pixel(253, 90);
        /* (304, 100) is in the right-hand sliver of this window's own drop
         * shadow (the frame's right edge is x = 302, the shadow reaches
         * x = 308) and above where the second client's frame will land,
         * so the same point can be read before and after the focus change
         * - which is what pins down *both* shadow ratios rather than just
         * whichever one happens to be in effect. */
        uint32_t focused_shadow = fb_get_pixel(304, 100);
        /* The title itself is text, so counting its pixels is the honest
         * check - picking one glyph pixel by hand would be asserting on
         * the font's shape rather than on the color the title is drawn
         * in. "Clock" starts at x = 100 + TITLE_MARGIN(6). */
        int focused_bright = 0, focused_dim = 0;
        for (int32_t ty = 82; ty < 98; ty++) {
            for (int32_t tx = 106; tx < 150; tx++) {
                uint32_t c = fb_get_pixel(tx, ty);
                if (c == 0x00F0F0F0u) {
                    focused_bright++;
                } else if (c == 0x009AA4B0u) {
                    focused_dim++;
                }
            }
        }

        /* A second client connects and takes focus, so the clock's window
         * is now the unfocused one - without anything having touched the
         * clock, its window or the cursor. */
        task_t *stub_task = process_spawn("wm_stubborn", stub_image, (size_t)stub_size, "");
        kfree(stub_image);
        pit_sleep_ms(700);

        uint32_t unfocused_corner = fb_get_pixel(246, 83);
        uint32_t unfocused_disc = fb_get_pixel(250, 90);
        uint32_t unfocused_glyph = fb_get_pixel(253, 90);
        uint32_t unfocused_shadow = fb_get_pixel(304, 100);
        int unfocused_bright = 0, unfocused_dim = 0;
        for (int32_t ty = 82; ty < 98; ty++) {
            for (int32_t tx = 106; tx < 150; tx++) {
                uint32_t c = fb_get_pixel(tx, ty);
                if (c == 0x00F0F0F0u) {
                    unfocused_bright++;
                } else if (c == 0x009AA4B0u) {
                    unfocused_dim++;
                }
            }
        }

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

        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *icons_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !icons_image) {
            panic("out of memory reading compositor/desktop_icons back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t icons_size = vfs_read("/bin/desktop_icons", icons_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || icons_size < 0) {
            panic("vfs_read: compositor/desktop_icons missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        pit_sleep_ms(200);
        task_t *icons_task = process_spawn("desktop_icons", icons_image, (size_t)icons_size, "");
        pit_sleep_ms(700);
        uint32_t saved_pixel = fb_get_pixel(600, 400);
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
        pit_sleep_ms(200);
        icons_task = process_spawn("desktop_icons", icons_image, (size_t)icons_size, "");
        kfree(icons_image);
        pit_sleep_ms(700);
        uint32_t fallback_pixel = fb_get_pixel(600, 400);
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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image) {
            panic("out of memory reading compositor back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
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
        pit_sleep_ms(400);

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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image) {
            panic("out of memory reading compositor back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0) {
            panic("vfs_read: compositor missing - should exist, just seeded");
        }
        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(400);

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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *stub_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !stub_image) {
            panic("out of memory reading compositor/wm_stubborn back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t stub_size = vfs_read("/bin/wm_stubborn", stub_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || stub_size < 0) {
            panic("vfs_read: compositor/wm_stubborn missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(400);

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
        uint8_t *last_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!last_image) {
            panic("out of memory reading wm_stubborn back from disk");
        }
        int64_t last_size = vfs_read("/bin/wm_stubborn", last_image, LEANFS_MAX_FILE_SIZE);
        task_t *last_task = last_size < 0 ? (task_t *)0
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
            {"a trailing slash", PATH_BIN_DIR},
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
                   "back by path; the same name in two directories staying two files; eight "
                   "malformed or escaping paths refused; and " PATH_BIN " holding exactly the "
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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *z_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !z_image) {
            panic("out of memory reading compositor/wm_zorder back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t z_size = vfs_read("/bin/wm_zorder", z_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || z_size < 0) {
            panic("vfs_read: compositor/wm_zorder missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

        task_t *a_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zA 00A02020");
        pit_sleep_ms(500);
        task_t *b_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zB 002060C0");
        kfree(z_image);
        pit_sleep_ms(500);

        /* B connected second, so it is in front to begin with - which was
         * true before this milestone too, and is the baseline the raise
         * below has to change. */
        uint32_t overlap_before = fb_get_pixel(200, 250);

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
        pit_sleep_ms(300);

        uint32_t overlap_after_raise = fb_get_pixel(200, 250);
        uint32_t a_tick1 = fb_get_pixel(393, 293);
        uint32_t b_tick1 = fb_get_pixel(433, 333);

        /* The occlusion bug, stated as a test: a click in the region both
         * windows cover has to reach exactly the one in front. */
        mouse_inject(-4096, -4096, 0, 0);
        mouse_inject(300, 200, 0, 0);
        pit_sleep_ms(120);
        mouse_inject(0, 0, 1, 0);
        pit_sleep_ms(120);
        mouse_inject(0, 0, 0, 0);
        pit_sleep_ms(300);

        uint32_t a_tick2 = fb_get_pixel(379, 293);
        uint32_t b_tick_still = fb_get_pixel(433, 333);

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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *fault_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !fault_image) {
            panic("out of memory reading compositor/wm_faulter back from disk");
        }
        int64_t comp_size = vfs_read("/bin/compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t fault_size = vfs_read("/bin/wm_faulter", fault_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || fault_size < 0) {
            panic("vfs_read: compositor/wm_faulter missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(300);

        uint64_t frames_before = pmm_free_frame_count();
        task_t *victim = process_spawn("wm_faulter", fault_image, (size_t)fault_size, "");
        kfree(fault_image);
        pit_sleep_ms(500); /* connects and paints, comfortably inside its own ALIVE_MS before it faults */

        uint32_t painted = fb_get_pixel(150, 150);
        int alive_before_fault = (int)do_syscall(SYS_task_alive, (uint64_t)victim->id, 0, 0);
        uint64_t frames_with_victim = pmm_free_frame_count();

        /* Past its ALIVE_MS, plus room for the compositor's own reap loop
         * to notice and repaint. If the machine were going to stop, this
         * is where it would have. */
        pit_sleep_ms(1400);

        int alive_after_fault = (int)do_syscall(SYS_task_alive, (uint64_t)victim->id, 0, 0);
        long victim_exit = do_syscall(SYS_wait, (uint64_t)victim->id, 0, 0);
        uint32_t after_fault = fb_get_pixel(150, 150);
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
        uint8_t *bad_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!bad_image) {
            panic("out of memory reading badptr back from disk");
        }
        int64_t bad_size = vfs_read("/bin/badptr", bad_image, LEANFS_MAX_FILE_SIZE);
        if (bad_size < 0) {
            panic("vfs_read: badptr missing - should exist, just seeded");
        }
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
        uint8_t *hello_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!hello_image) {
            panic("out of memory reading hello back from disk");
        }
        int64_t hello_size = vfs_read(PATH_BIN_DIR "hello", hello_image, LEANFS_MAX_FILE_SIZE);
        if (hello_size < 0) {
            panic("vfs_read: hello missing - should exist, just seeded");
        }

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
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *z_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !z_image) {
            panic("out of memory reading compositor/wm_zorder back from disk");
        }
        int64_t comp_size = vfs_read(PATH_BIN_DIR "compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t z_size = vfs_read(PATH_BIN_DIR "wm_zorder", z_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || z_size < 0) {
            panic("vfs_read: compositor/wm_zorder missing - should exist, just seeded");
        }

        task_t *comp1 = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        pit_sleep_ms(300);
        task_t *a_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zA 00A02020");
        pit_sleep_ms(400);
        task_t *b_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zB 002060C0");
        pit_sleep_ms(600);

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
        /* Long enough for a reconnect that *lost* the race with this new
         * compositor clearing the well-known pipes - the client's first
         * request is discarded in that case and re-sent one
         * WM_CONNECT_TIMEOUT_MS later (wmclient.c), so anything shorter
         * than a couple of those intervals is a test that passes on
         * timing rather than on behavior. */
        pit_sleep_ms(2500);

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

    uint8_t *init_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
    if (!init_image) {
        panic("out of memory reading init back from disk");
    }
    int64_t init_size = vfs_read("/bin/init", init_image, LEANFS_MAX_FILE_SIZE);
    if (init_size < 0) {
        panic("vfs_read(\"init\") failed - should exist, just seeded");
    }
    process_spawn("init", init_image, (size_t)init_size, "");
    kfree(init_image);

    klog_puts("[init] PID 1 spawned - handing off to the desktop shell.\n\n");

    for (;;) {
        __asm__ volatile("hlt");
    }
}
