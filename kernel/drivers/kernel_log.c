#include "kernel_log.h"

#include <stddef.h>
#include <stdint.h>

#include "architecture/x86_64/io.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"
#ifdef LEANOS_HOST_TEST
static uint64_t tsc_read(void) {
    return 0;
}
static uint64_t tsc_cycles_per_us(void) {
    return 0;
}
#else
#include "architecture/x86_64/timestamp_counter.h"
#endif
#include "console.h"
#include "library/spinlock.h"
#include "serial.h"

static int use_console = 0;

static int console_released = 0;

void kernel_log_init(void) {
    serial_init();
}

void kernel_log_use_console(void) {
    use_console = 1;
}

typedef struct {
    char text[256];
    size_t n;
} kernel_log_line_t;

static void kernel_log_line_add(kernel_log_line_t *line, const char *s) {
    while (*s && line->n < sizeof(line->text) - 1) {
        line->text[line->n++] = *s++;
    }
}

static void kernel_log_line_add_dec(kernel_log_line_t *line, uint64_t value) {
    char digits[20];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < (int)sizeof(digits));
    while (count > 0 && line->n < sizeof(line->text) - 1) {
        line->text[line->n++] = digits[--count];
    }
}

/* M225 (log-crash-path): the console's whole life is the boot, and the
   [logwrite] self-test that grades its shape comes long after it - on a
   screen where every self-test before it passes. So what the console cost
   is said once, here, when the compositor takes the screen: the most cells
   any interrupts-off section painted, the longest section that painted and
   the longest of all, in microseconds once the TSC is measured. One write,
   so it is one piece of the log. */
void kernel_log_release_console(void) {
    if (console_released) {
        return;
    }
    console_released = 1;
    if (!use_console) {
        return;
    }
    kernel_log_irq_off_t stats;
    kernel_log_irq_off_stats(&stats, 0);
    uint64_t per_us = tsc_cycles_per_us();
    kernel_log_line_t line;
    line.n = 0;
    kernel_log_line_add(&line, "[console] the compositor has the screen; the console drew ");
    kernel_log_line_add_dec(&line, stats.console_chars);
    kernel_log_line_add(&line, " characters, at most ");
    kernel_log_line_add_dec(&line, stats.boot_max_console_cells);
    kernel_log_line_add(&line, " cells in one section (ceiling ");
    kernel_log_line_add_dec(&line, KERNEL_LOG_SECTION_CELLS);
    kernel_log_line_add(&line, "), ");
    kernel_log_line_add_dec(&line, stats.boot_repaint_sections);
    kernel_log_line_add(&line, " repaint sections, the longest ");
    kernel_log_line_add_dec(&line, per_us ? stats.boot_max_repaint_cycles / per_us : 0);
    kernel_log_line_add(&line, " us, all of them ");
    kernel_log_line_add_dec(&line, per_us ? stats.boot_repaint_cycles / per_us : 0);
    kernel_log_line_add(&line, " us; the longest section that drew ");
    kernel_log_line_add_dec(&line, per_us ? stats.boot_max_console_cycles / per_us : 0);
    kernel_log_line_add(&line, " us, the longest of all ");
    kernel_log_line_add_dec(&line, per_us ? stats.boot_max_cycles / per_us : 0);
    kernel_log_line_add(&line, " us\n");
    kernel_log_write(line.text, line.n);
}

/* M225. The log is two things that used to be done in one breath: the ring
   (memory - the log a machine with no serial port leaves behind, and what
   [logwrite] reads back) and the devices (the serial port and, until the
   compositor takes the screen, the console). Both were written a byte at a
   time under kernel_log_lock with interrupts off, so that a write()'s bytes
   stayed together - and so one write() of 256 bytes held interrupts off for
   256 characters of serial port: at this kernel's 38400 baud on a machine
   with a real COM port, about 67 ms of lost timer ticks and of xHCI and NIC
   interrupts waiting, and under hvf a VM exit or two per character.

   Now a write goes into the ring in one hold of the lock - memory only -
   and the ring is DRAINED to the devices separately, in ring order, by one
   processor at a time, a few characters per interrupts-off section:

   - Whoever appends drains, after letting go of the lock, until the devices
     have everything up to the end of its own bytes. It waits for that with
     interrupts as its caller had them (on, for a program's write()), so a
     write still returns with its bytes on the wire as it always did - the
     last line before a hang is not left sitting in memory.
   - The drain goes a batch at a time: interrupts off, claim the drain
     (kernel_log_drain_owner), send at most KERNEL_LOG_DRAIN_BATCH characters
     - only as many as the serial FIFO takes without a wait, and no more
     than KERNEL_LOG_DRAIN_BUDGET_US worth - advance kernel_log_drained, let
     go, interrupts back. A busy port is let go of and waited for with
     interrupts on. Nothing is held across a point where interrupts are on,
     so a drainer preempted or killed between two batches (a tick can
     deliver a signal in kernel mode) leaves nothing stuck: the next waiter
     claims the next batch.
   - Order is the ring's, because kernel_log_drained only moves under the
     claim and only forward.
   - An NMI or exception taken DURING a batch, on the processor that owns
     it, appends and returns: waiting there would be waiting for itself. The
     owner takes one last look at the ring before it stops draining.

   - A crash report (kernel_log_begin_report) is the exception: it goes to
     the devices as it is written, inside its message, because the machine
     writing it may reset before kernel_log_end.
   - A console scroll paints nothing inside a section: it moves the grid,
     and the screen is repainted from it by the drain, at most
     KERNEL_LOG_SECTION_CELLS cells a section.

   A panic sends whatever was still waiting in the ring and then writes
   through, under the drain's claim - waited for while its holder makes
   progress, taken over when it has made none for
   KERNEL_LOG_PATIENCE_US (a processor the panic halted mid-batch).
   It takes no lock: the processor holding one may be the one that
   panicked. */
static spinlock_t kernel_log_lock;

static volatile int kernel_log_panicking;
static spinlock_t kernel_log_message_lock;
static volatile int kernel_log_message_owner = -1;
static volatile int kernel_log_message_depth;
/* The message depth at which this processor's crash report began, 0 when
   the message it is in is not one. Only the message lock's owner uses it. */
static volatile int kernel_log_message_report_depth;
/* Set when this processor's crash report gave up waiting for another
   processor's batch (kernel_log_drain, `bounded`): the rest of the report
   then waits for nobody. Cleared when the report ends. */
static volatile int kernel_log_message_report_stalled;

/* How long a panic, or a crash report, waits for the drain's owner to make
   progress - before the panic takes the drain over, or the report leaves
   its bytes in the ring for the panic that follows it. A batch is
   KERNEL_LOG_DRAIN_BATCH characters and a repaint section
   KERNEL_LOG_SECTION_CELLS cells, either well under a millisecond even on a
   38400-baud port; an owner that says nothing for this long is not coming
   back (smp_halt_other_cpus stopped it mid-batch, or it is the hang the
   panic is about) - or it is waiting for this processor: an NMI or fault
   taken inside its batch whose own report spins on the message lock a
   report here holds. Before the TSC is measured it is a count of waits
   instead. */
#define KERNEL_LOG_PATIENCE_US 20000u
#ifdef LEANOS_HOST_TEST
#define KERNEL_LOG_PATIENCE_SPINS (1u << 12)
#else
#define KERNEL_LOG_PATIENCE_SPINS (1u << 22)
#endif

/* The longest run of blank cells one piece of a scroll's repaint fills
   between two looks at the clock and at the drain's claim (a piece is one
   glyph or one such run - console_repaint_step): 4,096 pixels, a couple of
   microseconds of write-combining stores. */
#define KERNEL_LOG_REPAINT_STEP 32u

#ifdef LEANOS_HOST_TEST
/* The host test plays the other processors here: it is called wherever
   this one would spin waiting for them. */
void kernel_log_test_relax(void);
#define KERNEL_LOG_RELAX() kernel_log_test_relax()
static int kernel_log_flags_had_interrupts(uint64_t flags) {
    return flags != 0;
}
#else
/* A processor waiting here may have interrupts off and so cannot take a TLB
   shootdown's IPI - it answers the request itself, as spin_lock does. */
#define KERNEL_LOG_RELAX()                     \
    do {                                       \
        if (smp_shootdown_pending) {           \
            smp_tlb_service_pending();         \
        }                                      \
        __asm__ volatile("pause");             \
    } while (0)
static int kernel_log_flags_had_interrupts(uint64_t flags) {
    return (flags & (1u << 9)) != 0;
}
#endif

#define KERNEL_LOG_RING_SIZE 262144u

static char kernel_log_ring[KERNEL_LOG_RING_SIZE];
/* Whether each byte of the ring goes to the console as well as the serial
   port: kernel_log_log below the console's level does not. */
static uint8_t kernel_log_ring_console[KERNEL_LOG_RING_SIZE / 8u];
static uint64_t kernel_log_written;
/* How much of the ring the devices have had. Moved only by the drain's
   owner, read by anyone. */
static uint64_t kernel_log_drained;
static volatile int kernel_log_drain_owner = -1;
static uint64_t kernel_log_device_dropped;

/* M197. The stick log said the laptop took nine seconds to reach the desktop
   and could not say where: a line carried no time. Each line's first byte is
   remembered with the TSC it was written at, so the log a machine with no
   serial port leaves behind can be read as a timeline. */
#define KERNEL_LOG_LINE_RING 16384u

static uint64_t kernel_log_line_offset[KERNEL_LOG_LINE_RING];
static uint64_t kernel_log_line_tsc[KERNEL_LOG_LINE_RING];
static uint64_t kernel_log_lines;
static int kernel_log_mid_line;

/* The instrument for the interrupts-off windows the log opens itself. Only
   a section that found interrupts ON and turned them off is counted: a
   caller that already had them off is paying for a window of its own. */
static uint64_t irq_off_windows;
static uint64_t irq_off_total_cycles;
static uint64_t irq_off_max_cycles;
static uint32_t irq_off_max_device_chars;
/* Only the drain's owner (or a panic) sends to the devices, so these are
   written by one processor at a time; they are a measurement, read
   unlocked. */
static uint64_t device_console_chars;
static uint64_t device_console_cycles;
static uint64_t device_serial_chars;
static uint64_t device_serial_cycles;
/* Since boot (kernel_log_irq_off_t). */
static uint64_t boot_max_cycles;
static uint64_t boot_max_console_cycles;
static uint32_t boot_max_console_cells;
static uint64_t boot_repaint_sections;
static uint64_t boot_max_repaint_cycles;
static uint64_t repaint_total_cycles;
static uint64_t panic_takeovers;

static void kernel_log_raise_max64(uint64_t *max, uint64_t value) {
    uint64_t seen = __atomic_load_n(max, __ATOMIC_RELAXED);
    while (value > seen &&
           !__atomic_compare_exchange_n(max, &seen, value, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

/* `console_cells`: what the console painted in the section - counted
   whether or not the section found interrupts on, because it is a shape
   rather than a time. */
static void kernel_log_note_window(uint64_t flags, uint64_t cycles, uint32_t device_chars,
                                   uint32_t console_cells) {
    uint32_t cells_seen = __atomic_load_n(&boot_max_console_cells, __ATOMIC_RELAXED);
    while (console_cells > cells_seen &&
           !__atomic_compare_exchange_n(&boot_max_console_cells, &cells_seen, console_cells, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    if (!kernel_log_flags_had_interrupts(flags)) {
        return;
    }
    kernel_log_raise_max64(&boot_max_cycles, cycles);
    if (console_cells) {
        kernel_log_raise_max64(&boot_max_console_cycles, cycles);
    }
    __atomic_fetch_add(&irq_off_windows, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&irq_off_total_cycles, cycles, __ATOMIC_RELAXED);
    uint64_t seen = __atomic_load_n(&irq_off_max_cycles, __ATOMIC_RELAXED);
    while (cycles > seen &&
           !__atomic_compare_exchange_n(&irq_off_max_cycles, &seen, cycles, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    uint32_t chars = __atomic_load_n(&irq_off_max_device_chars, __ATOMIC_RELAXED);
    while (device_chars > chars &&
           !__atomic_compare_exchange_n(&irq_off_max_device_chars, &chars, device_chars, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

void kernel_log_irq_off_stats(kernel_log_irq_off_t *out, int reset) {
    if (out) {
        out->windows = __atomic_load_n(&irq_off_windows, __ATOMIC_RELAXED);
        out->total_cycles = __atomic_load_n(&irq_off_total_cycles, __ATOMIC_RELAXED);
        out->max_cycles = __atomic_load_n(&irq_off_max_cycles, __ATOMIC_RELAXED);
        out->max_device_chars = __atomic_load_n(&irq_off_max_device_chars, __ATOMIC_RELAXED);
        out->device_dropped = __atomic_load_n(&kernel_log_device_dropped, __ATOMIC_RELAXED);
        out->console_chars = device_console_chars;
        out->console_cycles = device_console_cycles;
        out->serial_chars = device_serial_chars;
        out->serial_cycles = device_serial_cycles;
        out->console_on = use_console && !console_released;
        out->boot_max_cycles = __atomic_load_n(&boot_max_cycles, __ATOMIC_RELAXED);
        out->boot_max_console_cycles = __atomic_load_n(&boot_max_console_cycles, __ATOMIC_RELAXED);
        out->boot_max_console_cells = __atomic_load_n(&boot_max_console_cells, __ATOMIC_RELAXED);
        out->boot_repaint_sections = __atomic_load_n(&boot_repaint_sections, __ATOMIC_RELAXED);
        out->boot_max_repaint_cycles = __atomic_load_n(&boot_max_repaint_cycles, __ATOMIC_RELAXED);
        out->boot_repaint_cycles = __atomic_load_n(&repaint_total_cycles, __ATOMIC_RELAXED);
        out->panic_takeovers = __atomic_load_n(&panic_takeovers, __ATOMIC_RELAXED);
    }
    if (reset) {
        __atomic_store_n(&irq_off_windows, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&irq_off_total_cycles, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&irq_off_max_cycles, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&irq_off_max_device_chars, 0, __ATOMIC_RELAXED);
    }
}

/* One byte into the ring. Memory only: under kernel_log_lock, or with no
   lock at all on a panicking machine. */
static void kernel_log_append_byte(char c, int also_console) {
    if (!kernel_log_mid_line) {
        uint64_t slot = kernel_log_lines % KERNEL_LOG_LINE_RING;
        kernel_log_line_offset[slot] = kernel_log_written;
        kernel_log_line_tsc[slot] = tsc_read();
        kernel_log_lines++;
    }
    kernel_log_mid_line = c != '\n';
    uint64_t at = kernel_log_written % KERNEL_LOG_RING_SIZE;
    kernel_log_ring[at] = c;
    uint8_t bit = (uint8_t)(1u << (at % 8u));
    if (also_console) {
        kernel_log_ring_console[at / 8u] |= bit;
    } else {
        kernel_log_ring_console[at / 8u] &= (uint8_t)~bit;
    }
    __atomic_store_n(&kernel_log_written, kernel_log_written + 1, __ATOMIC_RELEASE);
}

/* The ring's byte at `at` to the devices: into the serial FIFO without a
   wait when `fifo` (the drain asked for the room first), or waiting for the
   transmitter as a panic does. */
static void kernel_log_device_put(uint64_t at, int fifo) {
    uint64_t slot = at % KERNEL_LOG_RING_SIZE;
    char c = kernel_log_ring[slot];
    uint64_t t0 = tsc_read();
    if (((kernel_log_ring_console[slot / 8u] >> (slot % 8u)) & 1u) && use_console &&
        !console_released) {
        console_putc(c);
        uint64_t t1 = tsc_read();
        device_console_chars++;
        device_console_cycles += t1 - t0;
        t0 = t1;
    }
    if (fifo) {
        serial_tx_put(c);
    } else {
        serial_putc(c);
    }
    device_serial_chars++;
    device_serial_cycles += tsc_read() - t0;
}

static int kernel_log_console_drawing(void) {
    return use_console && !console_released;
}

/* Whether the screen still owes a scroll's repaint. Read by anyone as a
   hint; acted on only under the drain's claim. */
static int kernel_log_repaint_owed(void) {
    return kernel_log_console_drawing() && console_repaint_pending();
}

/* Counts every section the drain's owner finished and every character a
   panic sent: what "the owner is making progress" is read off. */
static volatile uint64_t kernel_log_drain_progress;

typedef struct {
    uint64_t seen;
    uint64_t since;
    uint32_t spins;
} kernel_log_patience_t;

static void kernel_log_patience_start(kernel_log_patience_t *patience) {
    patience->seen = __atomic_load_n(&kernel_log_drain_progress, __ATOMIC_ACQUIRE);
    patience->since = tsc_read();
    patience->spins = 0;
}

/* 1 once the drain's owner has made no progress for KERNEL_LOG_PATIENCE_US
   (or KERNEL_LOG_PATIENCE_SPINS looks, before the TSC is measured). */
static int kernel_log_patience_run_out(kernel_log_patience_t *patience) {
    uint64_t now = __atomic_load_n(&kernel_log_drain_progress, __ATOMIC_ACQUIRE);
    if (now != patience->seen) {
        patience->seen = now;
        patience->since = tsc_read();
        patience->spins = 0;
        return 0;
    }
    patience->spins++;
    uint64_t per_us = tsc_cycles_per_us();
    return per_us ? tsc_read() - patience->since > per_us * KERNEL_LOG_PATIENCE_US
                  : patience->spins > KERNEL_LOG_PATIENCE_SPINS;
}

/* kernel_log_drained only moves forward: a batch the panic took over from a
   processor it gave up on may still end, later, at a point behind it. */
static void kernel_log_drained_advance(uint64_t to) {
    uint64_t seen = __atomic_load_n(&kernel_log_drained, __ATOMIC_ACQUIRE);
    while (to > seen && !__atomic_compare_exchange_n(&kernel_log_drained, &seen, to, 0,
                                                     __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
    }
}

/* The end of `cpu`'s batch: the devices have had everything before `to`,
   and the drain is let go of - unless a panic took it over meanwhile, which
   then keeps it. */
static void kernel_log_drain_release(int cpu, uint64_t to) {
    kernel_log_drained_advance(to);
    __atomic_fetch_add(&kernel_log_drain_progress, 1, __ATOMIC_RELEASE);
    int owner = cpu;
    __atomic_compare_exchange_n(&kernel_log_drain_owner, &owner, -1, 0, __ATOMIC_RELEASE,
                                __ATOMIC_RELAXED);
}

/* Whether `cpu` still has the drain's claim. Looked at before every
   character and every repaint step of a batch: a panic takes the claim over
   from an owner that has made no progress for KERNEL_LOG_PATIENCE_US (and
   halts it first - kernel_log_panic_claim), and an owner that comes back
   anyway - one inside an NMI handler, which the halt's NMI waits behind -
   stops at the next character instead of racing the panic's flush for the
   console's cursor and grid for the rest of its batch. */
static int kernel_log_drain_still_ours(int cpu) {
    return __atomic_load_n(&kernel_log_drain_owner, __ATOMIC_ACQUIRE) == cpu;
}

/* A panicking machine's claim on the drain: 1 when this call took it (and
   kernel_log_panic_release gives it back), 0 when this processor already
   had it - it panicked, or an NMI or fault came, in the middle of its own
   batch or flush. Another processor's claim is waited for only while that
   processor makes progress: a panic must never wait on something it cannot
   get, and a processor halted mid-batch holds the claim for ever.

   Until M225's log-crash-path a panic flushed with no claim at all: a
   batch another processor was half way through raced it for the console's
   state (the cursor, the grid) and then stored ITS end in
   kernel_log_drained over the panic's larger one - and the next flush sent
   everything between the two again. */
static int kernel_log_panic_claim(void) {
    int cpu = smp_current_cpu();
    kernel_log_patience_t patience;
    kernel_log_patience_start(&patience);
    for (;;) {
        int owner = __atomic_load_n(&kernel_log_drain_owner, __ATOMIC_ACQUIRE);
        if (owner == cpu) {
            return 0;
        }
        if (owner == -1) {
            if (__atomic_compare_exchange_n(&kernel_log_drain_owner, &owner, cpu, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return 1;
            }
            continue;
        }
        if (kernel_log_patience_run_out(&patience)) {
            /* The owner is halted BEFORE its claim is taken. panic() halts
               the other processors only after its words are out, and an
               owner that was merely slow - a vCPU the host descheduled for
               longer than the patience - would otherwise come back in the
               middle of its batch and run console_putc beside the panic's
               flush. The halt is an NMI, taken before the next instruction
               it runs; one already inside an NMI handler takes it on the
               way out, and kernel_log_drain_still_ours stops that one at
               its next character. */
            smp_halt_other_cpus();
            if (__atomic_compare_exchange_n(&kernel_log_drain_owner, &owner, cpu, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                __atomic_fetch_add(&panic_takeovers, 1, __ATOMIC_RELAXED);
                return 1;
            }
            continue;
        }
        KERNEL_LOG_RELAX();
    }
}

static void kernel_log_panic_release(int took) {
    if (took) {
        int owner = smp_current_cpu();
        __atomic_compare_exchange_n(&kernel_log_drain_owner, &owner, -1, 0, __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED);
    }
}

/* Under the claim on a panicking machine: the rest of a scroll's repaint,
   then everything not yet on the devices, waiting for the transmitter. The
   devices' position is read again for every character, so what an NMI's
   flush sent from inside this one is not sent a second time. */
static void kernel_log_panic_flush(void) {
    if (kernel_log_repaint_owed()) {
        while (console_repaint_step(KERNEL_LOG_SECTION_CELLS)) {
        }
    }
    for (;;) {
        uint64_t from = __atomic_load_n(&kernel_log_drained, __ATOMIC_ACQUIRE);
        uint64_t to = __atomic_load_n(&kernel_log_written, __ATOMIC_ACQUIRE);
        if (from >= to) {
            return;
        }
        if (to - from > KERNEL_LOG_RING_SIZE) {
            from = to - KERNEL_LOG_RING_SIZE;
        }
        kernel_log_device_put(from, 0);
        kernel_log_drained_advance(from + 1);
        __atomic_fetch_add(&kernel_log_drain_progress, 1, __ATOMIC_RELEASE);
    }
}

/* A panicking machine's write: what was waiting, then these bytes into the
   ring and out, all under the drain's claim (kernel_log_panic_claim) - which
   also keeps two processors' panic output from being appended at once when
   the claim can be had. No lock: the ring's may be held by the processor
   that panicked. */
static void kernel_log_panic_append(const char *s, size_t length, int stop_at_nul,
                                    int also_console) {
    int took = kernel_log_panic_claim();
    kernel_log_panic_flush();
    for (size_t i = 0; i < length && !(stop_at_nul && !s[i]); i++) {
        kernel_log_append_byte(s[i], also_console);
    }
    kernel_log_panic_flush();
    kernel_log_panic_release(took);
}

/* Send the ring to the devices until they have had everything before
   `until` and the screen has had any repaint a scroll left it. Called
   holding no log lock, with interrupts as the CALLER had them; each batch
   turns them off for itself.

   `bounded`: called inside a crash report, or at its end - interrupts off.
   Another processor's batch is then waited for only while it makes
   progress (kernel_log_patience_run_out): that processor may be inside an
   NMI or fault of its own whose report is spinning on the message lock
   this one holds, or it may be the hang the report is about - and waiting
   for it would be waiting for ever, with the panic after the report never
   reached. Returns 1 when it gave up: the bytes are left in the ring, for
   the panic after the report, which takes the drain over (and the caller
   marks the report stalled, so the rest of it does not wait again). */
static int kernel_log_drain(uint64_t until, int bounded) {
    int last_look = 0;
    kernel_log_patience_t patience;
    kernel_log_patience_start(&patience);
    for (;;) {
        if (__atomic_load_n(&kernel_log_drained, __ATOMIC_ACQUIRE) >= until &&
            !kernel_log_repaint_owed()) {
            if (last_look) {
                return 0;
            }
            /* What an NMI or exception appended while this processor owned a
               batch was left for the owner - this is that owner's last look,
               at what is in the ring right now. Once, so a drainer does not
               chase everybody else's output for ever. */
            last_look = 1;
            until = __atomic_load_n(&kernel_log_written, __ATOMIC_ACQUIRE);
            continue;
        }
        uint64_t flags = irq_save_disable();
        uint64_t t0 = tsc_read();
        int cpu = smp_current_cpu();
        /* -1 after this means the drain is ours; anything else says whose
           it is (a lost race leaves the winner here). */
        int owner = __atomic_load_n(&kernel_log_drain_owner, __ATOMIC_ACQUIRE);
        if (owner == -1) {
            __atomic_compare_exchange_n(&kernel_log_drain_owner, &owner, cpu, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE);
        }
        if (owner != -1) {
            irq_restore(flags);
            if (owner == cpu) {
                return 0;
            }
            if (bounded && kernel_log_patience_run_out(&patience)) {
                return 1;
            }
            /* Somebody else's batch. Waited for on memory, not on the port:
               under hvf every look at the port's status is a VM exit, and
               seven processors looking at it while the eighth sent made
               each character cost 51 us instead of a few. */
            KERNEL_LOG_RELAX();
            continue;
        }
        uint64_t budget = tsc_cycles_per_us() * KERNEL_LOG_DRAIN_BUDGET_US;
        if (kernel_log_repaint_owed()) {
            /* The repaint a scroll left the screen comes before the
               characters after it, a piece per section: at most
               KERNEL_LOG_SECTION_CELLS cells, and no more than the budget
               after the first piece (a glyph, or a blank run of at most
               KERNEL_LOG_REPAINT_STEP cells in one fill; the clock is read
               between every two). It was the whole screen
               inside the newline - on the 4K panel 72,000 cells, nine
               million pixels, in one section. */
            uint64_t cells_before = console_cells_drawn();
            uint32_t cells = 0;
            while (cells < KERNEL_LOG_SECTION_CELLS) {
                if (cells > 0 && budget && tsc_read() - t0 > budget) {
                    break;
                }
                if (!kernel_log_drain_still_ours(cpu)) {
                    break;
                }
                uint32_t step = KERNEL_LOG_SECTION_CELLS - cells;
                if (step > KERNEL_LOG_REPAINT_STEP) {
                    step = KERNEL_LOG_REPAINT_STEP;
                }
                uint32_t painted = console_repaint_step(step);
                if (!painted) {
                    break;
                }
                cells += painted;
            }
            uint64_t t1 = tsc_read();
            device_console_cycles += t1 - t0;
            repaint_total_cycles += t1 - t0;
            boot_repaint_sections++;
            if (kernel_log_flags_had_interrupts(flags)) {
                kernel_log_raise_max64(&boot_max_repaint_cycles, t1 - t0);
            }
            kernel_log_drain_release(cpu, 0);
            kernel_log_note_window(flags, t1 - t0, 0,
                                   (uint32_t)(console_cells_drawn() - cells_before));
            irq_restore(flags);
            continue;
        }
        /* The port is asked once, by the owner. A busy one is let go of and
           waited for with interrupts as the caller had them - never inside
           the batch. */
        int room = serial_tx_room();
        if (room <= 0) {
            kernel_log_drain_release(cpu, 0);
            irq_restore(flags);
            while (serial_tx_room() <= 0) {
                KERNEL_LOG_RELAX();
            }
            continue;
        }
        uint64_t from = __atomic_load_n(&kernel_log_drained, __ATOMIC_ACQUIRE);
        uint64_t to = __atomic_load_n(&kernel_log_written, __ATOMIC_ACQUIRE);
        if (to - from > KERNEL_LOG_RING_SIZE) {
            /* The ring lapped the devices. Every writer waits for its own
               bytes, so only output nobody waits for (an NMI's, a message
               far longer than the ring) could do it - and then the devices
               skip what the ring no longer has rather than send what has
               replaced it. */
            __atomic_fetch_add(&kernel_log_device_dropped, to - from - KERNEL_LOG_RING_SIZE,
                               __ATOMIC_RELAXED);
            from = to - KERNEL_LOG_RING_SIZE;
        }
        /* At most a batch, at most what the FIFO takes, and - once the TSC
           has been measured - no longer than KERNEL_LOG_DRAIN_BUDGET_US
           after the first character: a device whose characters are slow
           (a VM's port, a console on a 4K panel) gets fewer per section
           rather than a longer one. A character that scrolled the console
           ends the batch, so the repaint it owes is the next section. */
        uint64_t cells_before = console_cells_drawn();
        uint32_t sent = 0;
        while (from < to && sent < KERNEL_LOG_DRAIN_BATCH && room > 0) {
            int need = kernel_log_ring[from % KERNEL_LOG_RING_SIZE] == '\n' ? 2 : 1;
            if (need > room && sent > 0) {
                break;
            }
            if (sent > 0 && budget && tsc_read() - t0 > budget) {
                break;
            }
            if (!kernel_log_drain_still_ours(cpu)) {
                break;
            }
            kernel_log_device_put(from, 1);
            room -= need;
            from++;
            sent++;
            if (kernel_log_repaint_owed()) {
                break;
            }
        }
        kernel_log_drain_release(cpu, from);
        kernel_log_note_window(flags, tsc_read() - t0, sent,
                               (uint32_t)(console_cells_drawn() - cells_before));
        irq_restore(flags);
    }
}

/* Interrupts are off. The message lock, or one more level of it when this
   processor already holds it. */
static void kernel_log_message_enter(void) {
    int cpu = smp_current_cpu();
    if (kernel_log_message_owner == cpu) {
        kernel_log_message_depth++;
        return;
    }
    spin_lock(&kernel_log_message_lock);
    kernel_log_message_owner = cpu;
    kernel_log_message_depth = 1;
}

/* Interrupts are off. 1 when that was the outermost level. */
static int kernel_log_message_leave(void) {
    if (kernel_log_message_depth == kernel_log_message_report_depth) {
        kernel_log_message_report_depth = 0;
        kernel_log_message_report_stalled = 0;
    }
    if (--kernel_log_message_depth == 0) {
        kernel_log_message_owner = -1;
        spin_unlock(&kernel_log_message_lock);
        return 1;
    }
    return 0;
}

uint64_t kernel_log_begin(void) {
    uint64_t flags = irq_save_disable();
    if (kernel_log_panicking) {
        return flags;
    }
    kernel_log_message_enter();
    return flags;
}

uint64_t kernel_log_begin_report(void) {
    uint64_t flags = kernel_log_begin();
    if (!kernel_log_panicking && !kernel_log_message_report_depth) {
        kernel_log_message_report_depth = kernel_log_message_depth;
    }
    return flags;
}

/* Whether this processor is inside a crash report. Interrupts are off. */
static int kernel_log_in_report(void) {
    return kernel_log_message_owner == smp_current_cpu() && kernel_log_message_report_depth != 0;
}

/* A plain message's bytes went into the ring as it was built; the devices
   get them here, once interrupts are back as the caller had them. A
   report's are there already - and its end waits for nobody without limit:
   the unhandled-exception report ends with interrupts off on its way to
   panic(), and an owner of the drain that never makes progress (the hang
   the report is about, a processor stopped mid-batch) would keep it from
   ever getting there. Bounded, as the report's own writes are; a report
   that already gave up does not wait again. What is left in the ring is
   the panic's to send - it takes the drain over. */
void kernel_log_end(uint64_t flags) {
    if (kernel_log_panicking) {
        irq_restore(flags);
        return;
    }
    int ending_report = kernel_log_in_report();
    int stalled = kernel_log_message_report_stalled;
    int outermost = kernel_log_message_leave();
    irq_restore(flags);
    if (!outermost) {
        return;
    }
    if (ending_report) {
        if (!stalled) {
            kernel_log_drain(__atomic_load_n(&kernel_log_written, __ATOMIC_ACQUIRE), 1);
        }
        return;
    }
    kernel_log_drain(__atomic_load_n(&kernel_log_written, __ATOMIC_ACQUIRE), 0);
}

/* Inside a crash report (interrupts off, the message lock held): to the
   devices now, unless the report already gave up on the drain's owner. */
static void kernel_log_report_drain(uint64_t until) {
    if (kernel_log_message_report_stalled) {
        return;
    }
    if (kernel_log_drain(until, 1)) {
        kernel_log_message_report_stalled = 1;
    }
}

void kernel_log_enter_panic(void) {
    kernel_log_panicking = 1;
}

/* `length` bytes of `s` - stopping at a NUL when `stop_at_nul` - into the
   ring in one hold of the lock, then to the devices, unless this processor
   is in the middle of a plain begin/end message, whose end sends them. In a
   crash report they go to the devices now. */
static void kernel_log_append(const char *s, size_t length, int stop_at_nul, int also_console) {
    uint64_t flags = irq_save_disable();
    if (kernel_log_panicking) {
        kernel_log_panic_append(s, length, stop_at_nul, also_console);
        irq_restore(flags);
        return;
    }
    uint64_t t0 = tsc_read();
    spin_lock(&kernel_log_lock);
    for (size_t i = 0; i < length && !(stop_at_nul && !s[i]); i++) {
        kernel_log_append_byte(s[i], also_console);
    }
    uint64_t until = kernel_log_written;
    spin_unlock(&kernel_log_lock);
    int in_message = kernel_log_message_owner == smp_current_cpu();
    int in_report = kernel_log_in_report();
    kernel_log_note_window(flags, tsc_read() - t0, 0, 0);
    irq_restore(flags);
    if (in_report) {
        kernel_log_report_drain(until);
    } else if (!in_message) {
        kernel_log_drain(until, 0);
    }
}

void kernel_log_putc(char c) {
    kernel_log_append(&c, 1, 0, 1);
}

void kernel_log_puts(const char *s) {
    kernel_log_append(s, (size_t)-1, 1, 1);
}

/* Every byte of `s` in ONE hold of the lock, so nothing another processor
   writes can land between two of them. Kernel memory only: a user buffer
   can fault, and a fault taken here would be taken with interrupts off and
   the log's lock held, which is every other processor's next log line
   spinning on a lock the faulting one may never let go of.
   kernel_log_write_from stages a user buffer into the kernel first.

   It takes the message lock as well, so a write cannot land inside a fault
   report or anything else M106 made a multi-call message - and it is a
   message of its own to them. Since M225 both locks are held for the copy
   into the ring and no longer: the devices get the bytes afterwards, a
   batch at a time, in ring order. */
void kernel_log_write(const char *s, size_t length) {
    uint64_t flags = irq_save_disable();
    if (kernel_log_panicking) {
        kernel_log_panic_append(s, length, 0, 1);
        irq_restore(flags);
        return;
    }
    /* From the moment interrupts went off: a wait for either lock is as
       much a window as the copy. */
    uint64_t t0 = tsc_read();
    kernel_log_message_enter();
    spin_lock(&kernel_log_lock);
    for (size_t i = 0; i < length; i++) {
        kernel_log_append_byte(s[i], 1);
    }
    uint64_t until = kernel_log_written;
    spin_unlock(&kernel_log_lock);
    int outermost = kernel_log_message_leave();
    int in_report = kernel_log_in_report();
    kernel_log_note_window(flags, tsc_read() - t0, 0, 0);
    irq_restore(flags);
    if (in_report) {
        kernel_log_report_drain(until);
    } else if (outermost) {
        kernel_log_drain(until, 0);
    }
}

size_t kernel_log_write_cut(const char *staged, size_t length, int more_follows) {
    if (!more_follows) {
        return length;
    }
    for (size_t i = length; i > 0; i--) {
        if (staged[i - 1] == '\n') {
            return i;
        }
    }
    return length;
}

static int kernel_log_copy_plain(char *to, const char *from, size_t length) {
    for (size_t i = 0; i < length; i++) {
        to[i] = from[i];
    }
    return 0;
}

long kernel_log_write_from(const char *source, size_t length, kernel_log_copy_t copy_in) {
    if (!copy_in) {
        copy_in = kernel_log_copy_plain;
    }
    size_t done = 0;
    while (done < length) {
        char staged[KERNEL_LOG_WRITE_CHUNK];
        size_t chunk = length - done;
        if (chunk > KERNEL_LOG_WRITE_CHUNK) {
            chunk = KERNEL_LOG_WRITE_CHUNK;
        }
        if (copy_in(staged, source + done, chunk) != 0) {
            return done ? (long)done : -1;
        }
        size_t take = kernel_log_write_cut(staged, chunk, done + chunk < length);
        kernel_log_write(staged, take);
        done += take;
    }
    return (long)done;
}

static char hex_digit(uint8_t nibble) {
    return (char)(nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10));
}

void kernel_log_put_dec_pad(uint32_t value, int width) {
    char buffer[10];
    int n = 0;
    do {
        buffer[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value && n < (int)sizeof(buffer));
    for (int pad = n; pad < width; pad++) {
        kernel_log_putc('0');
    }
    while (n > 0) {
        kernel_log_putc(buffer[--n]);
    }
}

void kernel_log_put_dec(uint32_t value) {
    kernel_log_put_dec_pad(value, 1);
}

void kernel_log_put_hex32(uint32_t value) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        kernel_log_putc(hex_digit((value >> shift) & 0xF));
    }
}

void kernel_log_put_hex64(uint64_t value) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        kernel_log_putc(hex_digit((value >> shift) & 0xF));
    }
}

static kernel_log_level_t current_level = KERNEL_LOG_INFO;

void kernel_log_log(kernel_log_level_t level, const char *s) {
    kernel_log_append(s, (size_t)-1, 1, level >= current_level);
}

void kernel_log_log_hex64(kernel_log_level_t level, uint64_t value) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        char c = hex_digit((value >> shift) & 0xF);
        kernel_log_append(&c, 1, 0, level >= current_level);
    }
}

size_t kernel_log_read(uint64_t from, char *out, size_t max, uint64_t *next) {
    uint64_t flags = irq_save_disable();
    spin_lock(&kernel_log_lock);

    uint64_t total = kernel_log_written;
    uint64_t oldest = total > KERNEL_LOG_RING_SIZE ? total - KERNEL_LOG_RING_SIZE : 0;
    if (from < oldest) {
        from = oldest;
    }
    size_t n = 0;
    while (from + n < total && n < max) {
        out[n] = kernel_log_ring[(from + n) % KERNEL_LOG_RING_SIZE];
        n++;
    }
    if (next) {
        *next = from + n;
    }

    spin_unlock(&kernel_log_lock);
    irq_restore(flags);
    return n;
}

uint64_t kernel_log_written_total(void) {
    uint64_t flags = irq_save_disable();
    spin_lock(&kernel_log_lock);
    uint64_t v = kernel_log_written;
    spin_unlock(&kernel_log_lock);
    irq_restore(flags);
    return v;
}

int kernel_log_line_time(uint64_t offset, uint64_t *tsc) {
    uint64_t flags = irq_save_disable();
    spin_lock(&kernel_log_lock);
    uint64_t oldest = kernel_log_lines > KERNEL_LOG_LINE_RING ? kernel_log_lines - KERNEL_LOG_LINE_RING : 0;
    uint64_t low = oldest;
    uint64_t high = kernel_log_lines;
    int found = 0;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2u;
        uint64_t at = kernel_log_line_offset[middle % KERNEL_LOG_LINE_RING];
        if (at == offset) {
            *tsc = kernel_log_line_tsc[middle % KERNEL_LOG_LINE_RING];
            found = 1;
            break;
        }
        if (at < offset) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    spin_unlock(&kernel_log_lock);
    irq_restore(flags);
    return found;
}
