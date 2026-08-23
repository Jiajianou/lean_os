#include <stddef.h>
#include <stdint.h>

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
#include "lib/libk.h"
#include "mm/e820.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net/icmp.h"
#include "net/net.h"
#include "panic.h"
#include "proc/proc.h"
#include "sched/sched.h"
#include "signal.h"  /* system_api/include/signal.h */
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
    X(menu_bar)                      \
    X(desktop_icons)                 \
    X(gui_terminal)                  \
    X(text_editor)                   \
    X(file_manager)                  \
    X(settings)

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
 * ABI's second integer argument register. */
void kernel_main(uint32_t *e820_map, fb_boot_info_t *fb_info) {
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
    mouse_event_t last_ev = {0, 0, 0, 0};
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
    task_spawn(demo_task, "A");
    task_spawn(demo_task, "B");
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

    task_spawn(syscall_exit_task, NULL);
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
    task_t *producer = task_spawn(pipe_producer_task, test_pipe);
    task_t *consumer = task_spawn(pipe_consumer_task, test_pipe);
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
    task_t *spinner = task_spawn(spinner_task, NULL);
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
    task_t *quick_a = task_spawn(quick_task, NULL);
    task_t *quick_b = task_spawn(quick_task, NULL);
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
     * directly. */
    long self_pgid = do_syscall(SYS_getpgid, 0, 0, 0);
    long child_pgid = do_syscall(SYS_getpgid, (uint64_t)quick_a->id, 0, 0);
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
    for (size_t i = 0; i < EMBEDDED_PROGRAM_COUNT; i++) {
        const embedded_program_t *p = &embedded_programs[i];
        if (!vfs_exists(p->name)) {
            klog_puts("[fs] seeding disk with '");
            klog_puts(p->name);
            klog_puts("' (first boot only)...\n");
            size_t size = (size_t)(p->end - p->start);
            if (vfs_write(p->name, p->start, size) != 0) {
                panic("vfs_write: failed to seed a program onto disk");
            }
        }
    }
    klog_puts("[fs] all user programs present on disk.\n\n");

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
        if (vfs_write("fstest", fstest_buf, fstest_len) != 0) {
            panic("leanfs indirect-block self-test: vfs_write failed");
        }
        k_memset(fstest_readback, 0, fstest_len);
        int64_t fstest_size = vfs_read("fstest", fstest_readback, fstest_len);
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
        int64_t memtest_size = vfs_read("memtest", memtest_image, LEANFS_MAX_FILE_SIZE);
        if (memtest_size < 0) {
            panic("vfs_read(\"memtest\") failed - should exist, just seeded");
        }
        task_t *memtest_task = process_spawn(memtest_image, (size_t)memtest_size, "");
        kfree(memtest_image);
        long memtest_status = do_syscall(SYS_wait, (uint64_t)memtest_task->id, 0, 0);
        if (memtest_status != 0) {
            panic("memtest self-test: nonzero exit code - malloc or shm is broken");
        }
        klog_puts("[memtest] user-space malloc/free and cross-process shm self-tests passed.\n\n");
    }

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
        int64_t comp_size = vfs_read("compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t demo_size = vfs_read("wm_demo", demo_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || demo_size < 0) {
            panic("vfs_read: compositor/wm_demo missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn(comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        task_t *demo_task = process_spawn(demo_image, (size_t)demo_size, "");
        kfree(demo_image);

        long demo_status = do_syscall(SYS_wait, (uint64_t)demo_task->id, 0, 0);
        if (demo_status != 0) {
            panic("wm_demo self-test: nonzero exit code - window creation failed");
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
        do_syscall(SYS_kill, (uint64_t)comp_task->id, SIGKILL, 0);
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
        int64_t comp_size = vfs_read("compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t clock_size = vfs_read("gui_clock", clock_image, LEANFS_MAX_FILE_SIZE);
        int64_t paint_size = vfs_read("gui_paint", paint_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || clock_size < 0 || paint_size < 0) {
            panic("vfs_read: compositor/gui_clock/gui_paint missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn(comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200); /* let the compositor map the fb and open its request/response pipes before any client tries to connect */

        /* Spawned one at a time, each given room to finish its whole
         * connect handshake (several pipe round trips, each needing
         * multiple scheduler quanta) before the next one starts - so
         * which window ends up at index 0 vs 1 is deterministic instead
         * of a race between two tasks starting from the same instant. */
        task_t *clock_task = process_spawn(clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(500);

        task_t *paint_task = process_spawn(paint_image, (size_t)paint_size, "");
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

        do_syscall(SYS_kill, (uint64_t)paint_task->id, SIGKILL, 0);
        do_syscall(SYS_kill, (uint64_t)clock_task->id, SIGKILL, 0);
        do_syscall(SYS_kill, (uint64_t)comp_task->id, SIGKILL, 0);
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
        int64_t comp_size = vfs_read("compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t shell_size = vfs_read("desktop_shell", shell_image, LEANFS_MAX_FILE_SIZE);
        int64_t clock_size = vfs_read("gui_clock", clock_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || shell_size < 0 || clock_size < 0) {
            panic("vfs_read: compositor/desktop_shell/gui_clock missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn(comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

        task_t *shell_task = process_spawn(shell_image, (size_t)shell_size, "");
        kfree(shell_image);
        pit_sleep_ms(500); /* connects, lists files, draws its first frame */

        task_t *clock_task = process_spawn(clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(1000); /* connects (window 1, focused); desktop_shell's next periodic query picks it up */

        /* Panel docks at the bottom: y = 768 - PANEL_HEIGHT(32) = 736.
         * Running slot 0 is gui_clock's window (id 1, the only non-panel
         * window, focused), left-aligned at local (4,4) 96x24 - the
         * taskbar's only row now that the launcher is gone. Its label is
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
            {44, 742, 0x002E4A63u, "running slot 0 background (focused)"},
            {6,  749, 0x00FFFFFFu, "running slot 0 'C' glyph - on pixel (left stem)"},
            {9,  749, 0x002E4A63u, "running slot 0 'C' glyph - off pixel (bowl interior)"},
            {500, 738, 0x00181828u, "panel background (margin strip above the slot row, y=2 - never overdrawn by any slot regardless of window count)"},
            {500, 500, 0x001A1A2Eu, "desktop background color, above the panel"},
        };
        uint32_t got[sizeof(checks) / sizeof(checks[0])];
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            got[i] = fb_get_pixel(checks[i].x, checks[i].y);
        }

        do_syscall(SYS_kill, (uint64_t)clock_task->id, SIGKILL, 0);
        do_syscall(SYS_kill, (uint64_t)shell_task->id, SIGKILL, 0);
        do_syscall(SYS_kill, (uint64_t)comp_task->id, SIGKILL, 0);
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
        int64_t comp_size = vfs_read("compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t clock_size = vfs_read("gui_clock", clock_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || clock_size < 0) {
            panic("vfs_read: compositor/gui_clock missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn(comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

        task_t *clock_task = process_spawn(clock_image, (size_t)clock_size, "");
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

        do_syscall(SYS_kill, (uint64_t)comp_task->id, SIGKILL, 0);
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
        long wrc = do_syscall(SYS_writefile, (uint64_t)"m33test", (uint64_t)content, sizeof(content) - 1);
        if (wrc != 0) {
            panic("M33 self-test: SYS_writefile failed");
        }
        char readback[64];
        long n = do_syscall(SYS_readfile, (uint64_t)"m33test", (uint64_t)readback, sizeof(readback));
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
        int64_t comp_size = vfs_read("compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0) {
            panic("vfs_read: compositor missing - should exist, just seeded");
        }
        task_t *comp_task = process_spawn(comp_image, (size_t)comp_size, "");
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

        do_syscall(SYS_kill, (uint64_t)comp_task->id, SIGKILL, 0);
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
        int64_t comp_size = vfs_read("compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t editor_size = vfs_read("text_editor", editor_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || editor_size < 0) {
            panic("vfs_read: compositor/text_editor missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn(comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

        task_t *editor_task = process_spawn(editor_image, (size_t)editor_size, "");
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

        do_syscall(SYS_kill, (uint64_t)comp_task->id, SIGKILL, 0);
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
        int64_t comp_size = vfs_read("compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t clock_size = vfs_read("gui_clock", clock_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || clock_size < 0) {
            panic("vfs_read: compositor/gui_clock missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn(comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

        task_t *clock_task = process_spawn(clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(500); /* connects (window 0, auto-focused), draws its first frame */

        /* Shadow: blend(desktop_bg, black, 1/3) - fill_rect_shadow's own
         * SHADOW_NUM/SHADOW_DEN - computed against the same 0x001A1A2E
         * default every earlier self-test's own "desktop_bg" constant
         * already assumes (a fresh compositor instance, nothing in
         * settings.c reachable to have changed it yet). */
        uint32_t shadow_pixel = fb_get_pixel(304, 150);
        uint32_t expected_shadow = 0x0011111Eu;

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

        do_syscall(SYS_kill, (uint64_t)clock_task->id, SIGKILL, 0);
        do_syscall(SYS_kill, (uint64_t)comp_task->id, SIGKILL, 0);
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
    smp_init();

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
            probe_tasks[i] = task_spawn(smp_probe_task, (void *)smp_seen_cpu);
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
         * this point in boot, task 0 has plenty of *other* unreaped
         * children too (M20-M22's compositor/client processes, killed via
         * SIGKILL rather than let exit normally, since a real WM isn't
         * expected to exit on its own - see those self-tests' own
         * comments). SYS_wait(-1) would happily reap those too, but only
         * once each one's pending SIGKILL actually gets noticed - which
         * needs it to be scheduled again at all, something nothing has
         * done since the moment it was killed. That's fine for boot to
         * leave lazily unresolved (nothing ever depended on it, before or
         * after SMP), but this self-test only ever cared about its own 4
         * probes, so waiting on their specific pids is both correct and a
         * lot less to get through. */
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
    /* M41 self-test: the shared top menu bar, end to end at the protocol
     * level - a real menu_bar.c panel and a real text_editor.c client,
     * with this self-test standing in for the *user* rather than for
     * either of them. Three separate claims, checked with pixels and a
     * protocol round trip:
     *
     *   1. The bar really is docked at the screen's top edge, and the
     *      editor's own window really was placed clear of it. Before
     *      M41 nothing docked to the top at all, and the cascade
     *      placement started at y=100 unconditionally.
     *   2. The menu query round-trips: asking the compositor what the
     *      focused window's menus are comes back with the File menu
     *      text_editor.c declared, with its four items - which is the
     *      one genuinely new protocol this milestone adds.
     *   3. A menu command reaches the owning client and *does something*
     *      - here "Quit" (item 3), which text_editor's own
     *      run_file_menu_item turns into the same clean sys_exit(1) M36's
     *      self-test already established for an unmodified buffer. That
     *      it exits with its own code, rather than being killed, is what
     *      proves the pick arrived as an event the app acted on.
     *
     * Whether a *click at the right pixel* opens the dropdown goes
     * through M40's input harness (tools/qemu-input-test.sh), which is
     * exactly the split that milestone's whole point was to make
     * possible. */
    {
        uint8_t *comp_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *bar_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        uint8_t *editor_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
        if (!comp_image || !bar_image || !editor_image) {
            panic("out of memory reading compositor/menu_bar/text_editor back from disk");
        }
        int64_t comp_size = vfs_read("compositor", comp_image, LEANFS_MAX_FILE_SIZE);
        int64_t bar_size = vfs_read("menu_bar", bar_image, LEANFS_MAX_FILE_SIZE);
        int64_t editor_size = vfs_read("text_editor", editor_image, LEANFS_MAX_FILE_SIZE);
        if (comp_size < 0 || bar_size < 0 || editor_size < 0) {
            panic("vfs_read: compositor/menu_bar/text_editor missing - should exist, just seeded");
        }

        task_t *comp_task = process_spawn(comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        pit_sleep_ms(200);

        task_t *bar_task = process_spawn(bar_image, (size_t)bar_size, "");
        kfree(bar_image);
        pit_sleep_ms(400); /* connects as window 0 (the top panel), draws its first frame */

        task_t *editor_task = process_spawn(editor_image, (size_t)editor_size, "");
        kfree(editor_image);
        pit_sleep_ms(700); /* connects as window 1, declares its File menu, draws */

        /* (1) Pixels. menu_bar.c's BAR_BG fills the whole strip; (600, 8)
         * is inside it and clear of both the app name on the left and the
         * clock on the right, so it can only ever read the flat bar fill.
         * (600, 40) is just below the bar's 24px docked height, where the
         * compositor's own desktop background must show through.
         *
         * The clamp itself is checked by maximizing: content_top_limit
         * puts a maximized window's content at 24 + TITLEBAR_H(20) +
         * BORDER(2) = 46, so its titlebar occupies y:[26, 46) and its
         * outer border starts at exactly 24 - the bar's bottom edge,
         * touching it and not a pixel higher. Probing at y=30 for that
         * titlebar and at y=8 for the bar *after* maximizing checks both
         * halves at once: the window grew to fill everything below the
         * bar, and the bar is still on top of it. Before M41 a maximized
         * window started at y=22 and would have covered the strip
         * outright. */
        const uint32_t bar_bg = 0x001E2233u; /* menu_bar.c BAR_BG */
        uint32_t bar_pixel = fb_get_pixel(600, 8);
        uint32_t below_bar_pixel = fb_get_pixel(600, 40);

        int action_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_ACTION_PIPE, (uint64_t)action_fds, 0) != 0) {
            panic("M41 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        wm_action_request_t max_req;
        k_memset(&max_req, 0, sizeof(max_req));
        max_req.window_id = 1; /* the editor - the bar took window 0 */
        max_req.action = WM_ACTION_MAXIMIZE;
        do_syscall(SYS_write, (uint64_t)action_fds[1], (uint64_t)&max_req, sizeof(max_req));
        pit_sleep_ms(400);
        /* x=300: inside the maximized window's titlebar, past the
         * "Editor" title text (which ends around x:58) and well left of
         * the three titlebar buttons, which sit at the right edge of its
         * 640-wide buffer around x:590-640. */
        uint32_t maximized_titlebar = fb_get_pixel(300, 30);
        uint32_t bar_over_maximized = fb_get_pixel(600, 8);

        /* (2) The menu query, driven straight off WM_MENU_QUERY_PIPE the
         * same way M30/M33/M36 drive their own protocol pipes. */
        int mq_fds[2];
        int mqr_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_MENU_QUERY_PIPE, (uint64_t)mq_fds, 0) != 0 ||
            do_syscall(SYS_pipe_open, (uint64_t)WM_MENU_QUERY_RESP_PIPE, (uint64_t)mqr_fds, 0) != 0) {
            panic("M41 self-test: kernel-side SYS_pipe_open(WM_MENU_QUERY_*) failed");
        }
        /* The bar polls this same pair every 300ms, so a reply meant for
         * it could already be sitting in the response pipe - drain it, so
         * what gets read below is the answer to *this* question. */
        do_syscall(SYS_pipe_reset, (uint64_t)mqr_fds[0], 0, 0);
        uint8_t ping = 1;
        do_syscall(SYS_write, (uint64_t)mq_fds[1], (uint64_t)&ping, sizeof(ping));
        pit_sleep_ms(300);
        wm_menu_set_t menus;
        k_memset(&menus, 0, sizeof(menus));
        long menu_read = do_syscall(SYS_read, (uint64_t)mqr_fds[0], (uint64_t)&menus, sizeof(menus));

        /* (3) Pick File > Quit and let the editor act on it. */
        int cmd_fds[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WM_MENU_CMD_PIPE, (uint64_t)cmd_fds, 0) != 0) {
            panic("M41 self-test: kernel-side SYS_pipe_open(WM_MENU_CMD_PIPE) failed");
        }
        wm_menu_command_t cmd;
        cmd.window_id = menus.window_id;
        cmd.menu_index = 0;
        cmd.item_index = 3; /* "Quit" - text_editor.c's FILE_MENU_ITEMS[3] */
        do_syscall(SYS_write, (uint64_t)cmd_fds[1], (uint64_t)&cmd, sizeof(cmd));
        pit_sleep_ms(500);
        long editor_exit = do_syscall(SYS_wait, (uint64_t)editor_task->id, 0, 0);

        do_syscall(SYS_kill, (uint64_t)bar_task->id, SIGKILL, 0);
        do_syscall(SYS_wait, (uint64_t)bar_task->id, 0, 0);
        do_syscall(SYS_kill, (uint64_t)comp_task->id, SIGKILL, 0);
        console_init();
        klog_use_console();

        int all_ok = 1;
        if (bar_pixel != bar_bg) {
            klog_puts("[m41] pixel check failed: the top menu bar should fill the screen's top strip - expected 0x");
            klog_put_hex32(bar_bg);
            klog_puts(" got 0x");
            klog_put_hex32(bar_pixel);
            klog_putc('\n');
            all_ok = 0;
        }
        if (below_bar_pixel == bar_bg) {
            klog_puts("[m41] pixel check failed: the menu bar is taller than its 24px docked height\n");
            all_ok = 0;
        }
        if (maximized_titlebar != 0x004C99E6u) {
            klog_puts("[m41] pixel check failed: a maximized window's titlebar should start immediately below the 24px menu bar - expected the focused-titlebar accent 0x004C99E6 at (300, 30), got 0x");
            klog_put_hex32(maximized_titlebar);
            klog_putc('\n');
            all_ok = 0;
        }
        if (bar_over_maximized != bar_bg) {
            klog_puts("[m41] pixel check failed: the menu bar must stay on top of a maximized window - expected 0x");
            klog_put_hex32(bar_bg);
            klog_puts(" got 0x");
            klog_put_hex32(bar_over_maximized);
            klog_putc('\n');
            all_ok = 0;
        }
        if (menu_read != (long)sizeof(menus)) {
            klog_puts("[m41] menu query returned the wrong number of bytes\n");
            all_ok = 0;
        } else if (menus.window_id < 0 || menus.menu_count != 1 ||
                   k_strcmp(menus.menus[0].title, "File") != 0 ||
                   menus.menus[0].item_count != 4 ||
                   k_strcmp(menus.menus[0].items[3], "Quit") != 0) {
            klog_puts("[m41] menu query did not round-trip text_editor's declared File menu (count 0x");
            klog_put_hex32((uint32_t)menus.menu_count);
            klog_puts(", window 0x");
            klog_put_hex32((uint32_t)menus.window_id);
            klog_puts(")\n");
            all_ok = 0;
        }
        if (editor_exit != 1) {
            klog_puts("[m41] File > Quit did not reach text_editor as a WM_EVENT_MENU_COMMAND it acted on - expected its own sys_exit(1), got 0x");
            klog_put_hex32((uint32_t)editor_exit);
            klog_putc('\n');
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M41 menu-bar self-test: the shared top bar did not behave as expected");
        }
        klog_puts("[m41] top menu bar (top-docked panel, menu query round-trip, "
                   "menu command reaching its client) self-test passed (6/6 checks matched).\n\n");
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
        int tasks_before = sched_task_count();
        uint64_t frames_before = pmm_free_frame_count();

        long rc_missing = do_syscall(SYS_spawn, (uint64_t)"definitely_not_a_file", 0, 0);
        if (rc_missing >= 0) {
            panic("M40 SYS_spawn self-test: spawning a nonexistent file should fail");
        }

        long rc_not_elf = do_syscall(SYS_spawn, (uint64_t)"m33test", 0, 0);
        if (rc_not_elf >= 0) {
            panic("M40 SYS_spawn self-test: spawning a non-ELF file should fail, not succeed");
        }

        int tasks_after = sched_task_count();
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

    uint8_t *init_image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
    if (!init_image) {
        panic("out of memory reading init back from disk");
    }
    int64_t init_size = vfs_read("init", init_image, LEANFS_MAX_FILE_SIZE);
    if (init_size < 0) {
        panic("vfs_read(\"init\") failed - should exist, just seeded");
    }
    process_spawn(init_image, (size_t)init_size, "");
    kfree(init_image);

    klog_puts("[init] PID 1 spawned - handing off to the desktop shell.\n\n");

    for (;;) {
        __asm__ volatile("hlt");
    }
}
