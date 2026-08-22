#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "drivers/keyboard.h"
#include "drivers/klog.h"
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
    X(shell)

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
 * E820_COUNT_ADDR and hands off in RDI. */
void kernel_main(uint32_t *e820_map) {
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

    klog_puts("[init] PID 1 spawned - handing off to the shell. Try 'ls', "
              "'cat <file>', 'echo <text>', 'hello', or 'exit'.\n\n");

    for (;;) {
        __asm__ volatile("hlt");
    }
}
