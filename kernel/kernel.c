#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/smp.h"
#include "drivers/console.h"
#include "drivers/cursor.h"
#include "drivers/fb.h"
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
#include "panic.h"
#include "proc/proc.h"
#include "sched/sched.h"
#include "signal.h"  /* system_api/include/signal.h */
#include "syscall.h" /* system_api/include/syscall.h */

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
    X(gui_terminal)

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
 * many e820_entry_t records — the layout stage2.asm builds at
 * E820_COUNT_ADDR and hands off in RDI.
 * fb_info: fb_boot_info_t describing the linear framebuffer stage2's
 * setup_vbe_mode set up (M16) - handed off in RSI, the System V ABI's
 * second integer argument register. */
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

    /* M16: bring up the linear framebuffer stage2's setup_vbe_mode set up
     * and describe in RSI (fb_info, this function's second argument) -
     * needs vmm live first, since fb_init maps the physical framebuffer
     * region in. */
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
    mouse_event_t last_ev = {0, 0, 0};
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
            {110, 115, 0x00122438u, "clock caption 'C' glyph - off pixel"},
            {112, 115, 0x00FFFFFFu, "clock caption 'C' glyph - on pixel"},
            /* gui_paint (window 1, focused): */
            {200, 130, 0x004C99E6u, "paint titlebar color (focused)"},
            {140, 190, 0x0088AA55u, "paint's own border frame color"},
            {150, 240, 0x00202020u, "paint canvas background color"},
            {150, 151, 0x00202020u, "paint caption 'P' glyph - off pixel"},
            {151, 151, 0x00FFFFFFu, "paint caption 'P' glyph - on pixel"},
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
     * proving it reads the real on-disk file list (SYS_listfiles) into
     * real launcher slots and reflects a real second window (gui_clock,
     * spawned here the same way M21's did) into a real running-window
     * slot via the M22 query protocol (system_api/include/wm.h's
     * WM_QUERY_PIPE). What this can't prove headlessly: that *clicking*
     * a launcher slot really spawns its program, or that clicking a
     * running-window slot really focuses/minimizes it - both need real
     * mouse input, verified manually via QEMU monitor injection the same
     * way M21's focus-follows-click and gui_paint strokes were; see
     * milestones.md's M22 entry for that verification's results. */
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
         * Launcher slot 0 is "hello" (first file ever seeded, M12/M13) at
         * local (4,4) 64x24; running slot 0 is gui_clock's window (id 1,
         * the only non-panel window, focused) right-aligned at
         * local (1024-4-64, 4) = (956,4). Coordinates below are absolute
         * (panel-local + the panel's own (0,736) origin) - see
         * milestones.md's M22 entry for the glyph-bitmap math behind the
         * on/off pixel picks, same method M21's already proved out. */
        static const struct { uint32_t x, y; uint32_t expected; const char *what; } checks[] = {
            {44, 742,  0x00334455u, "launcher slot 0 background"},
            {9,  749,  0x00334455u, "launcher slot 0 'h' glyph - off pixel"},
            {6,  749,  0x00FFFFFFu, "launcher slot 0 'h' glyph - on pixel"},
            {996, 742, 0x0055AA33u, "running slot 0 background (focused)"},
            {958, 749, 0x0055AA33u, "running slot 0 '#' glyph - off pixel"},
            {960, 749, 0x00FFFFFFu, "running slot 0 '#' glyph - on pixel"},
            {500, 738, 0x00181828u, "panel background (margin strip above the slot row, y=2 - never overdrawn by any slot regardless of file count)"},
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
        klog_puts("[wm22] desktop shell (panel + launcher + taskbar query) self-test passed "
                  "(8/8 pixel checks matched).\n\n");
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

    /* M13: hand off to init (PID 1), which spawns the shell - this is
     * where interactive use of the OS begins. kernel_main (task 0) never
     * "finishes" from here: it becomes the idle task, looping on `hlt`
     * so entry.asm's post-kernel_main `cli` (which would permanently
     * disable interrupts, freezing the scheduler for every other task)
     * is never reached. */
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
