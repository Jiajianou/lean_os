#pragma once

#include <stddef.h>
#include <stdint.h>

void kernel_log_init(void);
void kernel_log_putc(char c);
void kernel_log_puts(const char *s);

uint64_t kernel_log_begin(void);
void kernel_log_end(uint64_t flags);

/* A message that is a crash report - the exception paths' register dumps.
   Like kernel_log_begin (nothing another processor writes through
   kernel_log_write lands inside it), but its bytes go to the devices as
   they are written rather than at kernel_log_end: a report is written by a
   machine that may reset, triple fault or fault again at any step of it
   (dump_regs on a bad stack), and whatever was written before that must
   already be on the wire and the screen. It is a section with interrupts
   off for as long as its devices take, which an ordinary message is not -
   that is the price of a crash report, and why a [perf] line is not one.
   It is also every other processor's kernel_log_write waiting, with ITS
   interrupts off, on the message lock for that long - so only a report a
   panic follows is one: the unhandled exception. A ring-3 fault or an int3
   is a plain message; if its dump faults again, the nested unhandled
   exception's report sends the half written ahead of it.
   A plain message nested inside a report goes out as written too, and so
   does what an outer plain message had written before a report began
   inside it. Ended with kernel_log_end, which - like the report's own
   writes - waits for another processor's batch only while that batch makes
   progress: past KERNEL_LOG_PATIENCE_US without any, the rest stays in the
   ring for the panic after the report. */
uint64_t kernel_log_begin_report(void);

/* What one write() to the console puts in the log in one piece. A write is
   staged through a buffer this size on the kernel stack and each piece goes
   out under one hold of the log's lock, so on any number of processors two
   programs' lines cannot be spliced into each other a character at a time -
   which is what a lock taken per byte did, on eight cores, to a battery
   marker. A write longer than this is cut after its last newline inside the
   piece when there is one, so it is whole LINES that stay together; a single
   line longer than this can still be split at the boundary. */
#define KERNEL_LOG_WRITE_CHUNK 256

/* M225: the most characters the log hands the serial port and the console
   in one interrupts-off section. What goes into the log goes into its ring
   (memory) under the lock, and the ring is sent to the devices afterwards,
   in order, this many at a time - the serial FIFO's depth, so the port is
   never waited for with interrupts off. It was everything a call wrote:
   up to KERNEL_LOG_WRITE_CHUNK characters of a 38400-baud port, ~67 ms. */
#define KERNEL_LOG_DRAIN_BATCH 16

/* ...and no more than this long, once the TSC is measured: a section stops
   after the character that took it past the budget (always one at least).
   Under hvf a serial character is a VM exit of several microseconds, and
   on the ThinkPad - no COM port - a character is a glyph on a 4K panel and
   now and then a scroll that redraws it. */
#define KERNEL_LOG_DRAIN_BUDGET_US 50

/* The most console cells (an 8x16 glyph or blank, 128 pixels) one section
   may paint: a batch of KERNEL_LOG_DRAIN_BATCH characters that are all tabs
   (eight cells each), or a piece of a scroll's repaint. Until M225's
   log-crash-path a newline that scrolled repainted the whole screen inside
   the section - every cell of it, 72,000 on the ThinkPad's 3840x2400. */
#define KERNEL_LOG_SECTION_CELLS (KERNEL_LOG_DRAIN_BATCH * 8)

/* Kernel memory only, all of it under one hold of the lock. */
void kernel_log_write(const char *s, size_t length);

/* The interrupts-off sections the log opened itself (found interrupts on
   and turned them off) since the last reset: how many, their total and
   longest in TSC cycles, and the most characters any one of them gave the
   devices. device_dropped counts characters the devices never got because
   the ring lapped them. What each device cost per character - the
   console's glyph on the framebuffer, the serial port's I/O - is counted as
   well, because a machine with no COM port (the ThinkPad) pays only the
   first and a VM under hvf pays mostly the second; console_on says whether
   the console was still drawing when read. The reset clears the window
   figures only: dropped and the device counts run from boot. */
typedef struct {
    uint64_t windows;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint32_t max_device_chars;
    uint64_t device_dropped;
    uint64_t console_chars;
    uint64_t console_cycles;
    uint64_t serial_chars;
    uint64_t serial_cycles;
    int console_on;
    /* Since boot, never reset - the console draws during the boot and has
       usually stopped by the time anything reads these: the longest section
       the log opened itself, the longest of those that painted on the
       console, the most console cells any section painted (whether it found
       interrupts on or not - that one is a shape, not a time), how many
       sections went to repainting after scrolls, and the longest of those. */
    uint64_t boot_max_cycles;
    uint64_t boot_max_console_cycles;
    uint32_t boot_max_console_cells;
    uint64_t boot_repaint_sections;
    uint64_t boot_max_repaint_cycles;
    /* All the repaint sections' cycles together, since boot. */
    uint64_t boot_repaint_cycles;
    /* How often a panic took the drain from a processor that had held it
       without progress for KERNEL_LOG_PATIENCE_US (kernel_log.c), after
       halting it. */
    uint64_t panic_takeovers;
} kernel_log_irq_off_t;

void kernel_log_irq_off_stats(kernel_log_irq_off_t *out, int reset);

/* Where kernel_log_write_from cuts a staged piece: after its last newline if
   more of the write follows it and it has one, otherwise all of it. */
size_t kernel_log_write_cut(const char *staged, size_t length, int more_follows);

/* Copies `length` bytes from `from` into the kernel buffer `to`, 0 or -1. */
typedef int (*kernel_log_copy_t)(char *to, const char *from, size_t length);

/* A write() to the console: `source` may be a user buffer, and is copied in
   by `copy_in` (a plain copy when 0) with interrupts on and no lock held, a
   KERNEL_LOG_WRITE_CHUNK at most at a time; each piece then goes out with
   kernel_log_write. Returns the bytes written, or -1 if the first copy
   failed - a later failure returns what was written before it. */
long kernel_log_write_from(const char *source, size_t length, kernel_log_copy_t copy_in);

void kernel_log_release_console(void);

void kernel_log_enter_panic(void);
void kernel_log_put_hex32(uint32_t value);
void kernel_log_put_dec(uint32_t value);
void kernel_log_put_dec_pad(uint32_t value, int width);
void kernel_log_put_hex64(uint64_t value);

typedef enum {
    KERNEL_LOG_DEBUG = 0,
    KERNEL_LOG_INFO,
    KERNEL_LOG_WARN,
    KERNEL_LOG_ERROR,
} kernel_log_level_t;

void kernel_log_log(kernel_log_level_t level, const char *s);
void kernel_log_log_hex64(kernel_log_level_t level, uint64_t value);

static inline void kernel_log_debug(const char *s) { kernel_log_log(KERNEL_LOG_DEBUG, s); }
static inline void kernel_log_info(const char *s) { kernel_log_log(KERNEL_LOG_INFO, s); }
static inline void kernel_log_warn(const char *s) { kernel_log_log(KERNEL_LOG_WARN, s); }
static inline void kernel_log_error(const char *s) { kernel_log_log(KERNEL_LOG_ERROR, s); }

void kernel_log_use_console(void);

size_t kernel_log_read(uint64_t from, char *out, size_t max, uint64_t *next);

uint64_t kernel_log_written_total(void);

int kernel_log_line_time(uint64_t offset, uint64_t *tsc);
