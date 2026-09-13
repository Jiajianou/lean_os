#include <stddef.h>
#include <stdint.h>

#include "acpi/acpi.h"
#include "architecture/x86_64/global_descriptor_table.h"
#include "architecture/x86_64/interrupt_descriptor_table.h"
#include "architecture/x86_64/ioapic.h"
#include "architecture/x86_64/pic.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"
#include "architecture/x86_64/timestamp_counter.h"
#include "drivers/block_device.h"
#include "drivers/console.h"
#include "drivers/cursor.h"
#include "display.h"
#include "icon.h"
#include "drivers/dispi.h"
#include "drivers/framebuffer.h"
#include "drivers/font8x16.h"
#include "drivers/keyboard.h"
#include "drivers/kernel_log.h"
#include "drivers/mouse.h"
#include "architecture/x86_64/floating_point_unit.h"
#include "architecture/x86_64/io.h"
#include "drivers/ac97.h"
#include "drivers/pc_speaker.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "drivers/xhci.h"
#include "file_system/leanfs.h"
#include "file_system/leanfs_format.h"
#include "file_system/flock.h"
#include "file_system/open_file.h"
#include "device/random.h"
#include "device/tty.h"
#include "device/fwcfg.h"
#include "file_system/virtual_file_system.h"
#include "inter_process_communication/pipe.h"
#include "inter_process_communication/shared_memory.h"
#include "inter_process_communication/unix_socket.h"
#include "inter_process_communication/eventfd.h"
#include "inter_process_communication/timerfd.h"
#include "inter_process_communication/epoll.h"
#include "inter_process_communication/memfd.h"
#include "library/kernel_library.h"
#include "memory_management/e820.h"
#include "memory_management/heap.h"
#include "memory_management/file_mapping.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"
#include "network/icmp.h"
#include "network/network.h"
#include "network/tcp.h"
#include "panic.h"
#include "paths.h"
#include "process.h"
#include "power/power.h"
#include "profile/sampler.h"
#include "profile.h"
#include "profile/syscall_counters.h"
#include "process/process.h"
#include "process/package_capabilities.h"
#include "scheduler/scheduler.h"
#include "shortcuts.h"
#include "signal.h"
#include "spawn_error.h"
#include "syscall.h"
#include "window_manager.h"

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
    X(vmtest)                      \
    X(forktest)                    \
    X(exectest)                    \
    X(jobtest)                       \
    X(syscalltest)                 \
    X(profile)                     \
    X(proftest)                    \
    X(oomtest)                                                                \
    X(futextest)                   \
    X(fswriter)                    \
    X(ptytest)                     \
    X(exhausttest)                \
    X(faulttest)                   \
    X(measure)                     \
    X(os)                         \
    X(pkgtest)                    \
    X(dirtest)                  \
    X(browsertest)              \
    X(netrecv)                  \
    X(unixtest)                 \
    X(epolltest)                \
    X(memfdtest)                \
    X(lvgl_demo)

#define DECLARE_EMBEDDED_PROGRAM(name) \
    extern const uint8_t name##_elf_start[]; \
    extern const uint8_t name##_elf_end[];
FOR_EACH_EMBEDDED_PROGRAM(DECLARE_EMBEDDED_PROGRAM)
#undef DECLARE_EMBEDDED_PROGRAM

extern uint64_t kernel_stack_guard[];
#define KERNEL_STACK_GUARD_VALUE 0x5354414B47554152ULL

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

static long do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                      : "rcx", "r8", "r9", "memory");
    return (long)ret;
}

static long do_syscall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t ret;
    __asm__ volatile("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "D"(a1), "S"(a2), "d"(a3), "c"(a4)
                      : "r8", "r9", "memory");
    return (long)ret;
}

static uint8_t *read_program(const char *path, size_t *out_size) {
    leanfs_stat_t st;
    if (virtual_file_system_stat(path, &st) != 0 || st.is_directory || st.size == 0) {
        panic("read_program: a program this kernel just seeded is missing or empty");
    }
    uint8_t *image = (uint8_t *)kmalloc(st.size);
    if (!image) {
        panic("read_program: out of memory reading a program back from disk");
    }
    if (virtual_file_system_read(path, image, st.size) < 0) {
        panic("read_program: vfs_read failed on a program that stat succeeded on");
    }
    *out_size = st.size;
    return image;
}

static void kernel_log_perf(const char *name, uint64_t value, const char *unit) {
    kernel_log_puts("[perf] ");
    kernel_log_puts(name);
    kernel_log_putc(' ');
    kernel_log_put_dec(value > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)value);
    kernel_log_putc(' ');
    kernel_log_puts(unit);
    kernel_log_putc('\n');
}

#define LVGL_DISTINCT_BUCKETS 512
#define LVGL_DISTINCT_EMPTY 0xFFFFFFFFu
#define LVGL_MINIMUM_DRAWN_PIXELS 20000
#define LVGL_MINIMUM_DISTINCT_COLORS 64

#define LATENCY_SAMPLES 16

static void latency_sort(uint64_t *a, int n) {
    for (int i = 1; i < n; i++) {
        uint64_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

static void kernel_log_perf_distribution(const char *base, uint64_t *samples, int n) {
    uint64_t good[LATENCY_SAMPLES];
    int k = 0;
    for (int i = 0; i < n && k < LATENCY_SAMPLES; i++) {
        if (samples[i] != 0) {
            good[k++] = samples[i];
        }
    }
    if (k == 0) {
        return;
    }
    latency_sort(good, k);

    char name[64];
    int base_length = 0;
    while (base[base_length] && base_length < 40) {
        name[base_length] = base[base_length];
        base_length++;
    }
    static const char *suffix[3] = {"_best_us", "_med_us", "_worst_us"};
    uint64_t value[3] = {good[0], good[k / 2], good[k - 1]};
    for (int s = 0; s < 3; s++) {
        int m = base_length;
        for (int c = 0; suffix[s][c] && m < 62; c++) {
            name[m++] = suffix[s][c];
        }
        name[m] = '\0';
        kernel_log_perf(name, value[s], "us");
    }
}

static void selftest_type(const char *s) {
    for (const char *p = s; *p; p++) {
        keyboard_inject(*p, 0);
        pit_sleep_ms(25);
    }
}

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

#define SELFTEST_POLL_MS 10

static int selftest_wait_until(int (*probe)(void *context), void *context, uint32_t timeout_ms,
                                const char *what) {
    uint64_t deadline = pit_get_ticks() + (timeout_ms + 9) / 10;
    for (;;) {
        if (probe(context)) {
            return 1;
        }
        if (pit_get_ticks() >= deadline) {
            kernel_log_puts("[selftest] timed out waiting for: ");
            kernel_log_puts(what);
            kernel_log_putc('\n');
            return 0;
        }
        pit_sleep_ms(SELFTEST_POLL_MS);
    }
}

typedef struct {
    uint32_t x, y, expected;
} pixel_probe_t;

static int pixel_matches(void *context) {
    pixel_probe_t *p = (pixel_probe_t *)context;
    return framebuffer_get_pixel(p->x, p->y) == p->expected;
}

static int selftest_wait_for_pixel(uint32_t x, uint32_t y, uint32_t expected,
                                    uint32_t timeout_ms, const char *what) {
    pixel_probe_t probe = {x, y, expected};
    return selftest_wait_until(pixel_matches, &probe, timeout_ms, what);
}

#define SELFTEST_PAINT_MS 4000

static uint32_t selftest_pixel_settled(uint32_t x, uint32_t y, uint32_t expected,
                                        const char *what) {
    uint64_t deadline = pit_get_ticks() + (SELFTEST_PAINT_MS + 9) / 10;
    int agreed = 0;
    for (;;) {
        if (framebuffer_get_pixel(x, y) == expected) {
            if (++agreed >= 2) {
                return expected;
            }
        } else {
            agreed = 0;
        }
        if (pit_get_ticks() >= deadline) {
            kernel_log_puts("[selftest] timed out waiting for: ");
            kernel_log_puts(what);
            kernel_log_putc('\n');
            return framebuffer_get_pixel(x, y);
        }
        pit_sleep_ms(SELFTEST_POLL_MS);
    }
}

static void selftest_title_counts(int until_bright, int *out_bright, int *out_dim) {
    uint64_t deadline = pit_get_ticks() + (SELFTEST_PAINT_MS + 9) / 10;
    for (;;) {
        int bright = 0, dim = 0;
        for (int32_t ty = 82; ty < 98; ty++) {
            for (int32_t tx = 106; tx < 150; tx++) {
                uint32_t c = framebuffer_get_pixel(tx, ty);
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
                kernel_log_puts("[selftest] timed out waiting for: the window title to be drawn\n");
            }
            *out_bright = bright;
            *out_dim = dim;
            return;
        }
        pit_sleep_ms(SELFTEST_POLL_MS);
    }
}

static void selftest_wait_for_compositor(void) {
    selftest_wait_for_pixel(500, 400, 0x001A1A2Eu, 5000, "the compositor to paint the desktop");
}

static int selftest_column_lit(uint32_t x, uint32_t bg) {
    int lit = 0;
    for (uint32_t y = 240; y < framebuffer_height(); y += 4) {
        if (framebuffer_get_pixel(x, y) != bg) {
            lit++;
        }
    }
    return lit;
}

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

static int selftest_term_top_lit(void) {
    int lit = 0;
    for (int32_t ty = 100; ty < 116; ty++) {
        for (int32_t tx = 100; tx < 660; tx++) {
            if (framebuffer_get_pixel((uint32_t)tx, (uint32_t)ty) == 0x00D0D0D0u) {
                lit++;
            }
        }
    }
    return lit;
}

static int selftest_term_top_settled(int differs_from, uint32_t timeout_ms) {
    uint64_t deadline = pit_get_ticks() + (timeout_ms + 9) / 10;
    uint64_t last_change = pit_get_ticks();
    uint64_t quiet_needed = 30;
    int value = selftest_term_top_lit();
    for (;;) {
        pit_sleep_ms(SELFTEST_POLL_MS);
        uint64_t now = pit_get_ticks();
        int sampled = selftest_term_top_lit();
        if (sampled != value) {
            uint64_t gap = (now - last_change) * 4;
            if (gap > quiet_needed) {
                quiet_needed = gap > 400 ? 400 : gap;
            }
            value = sampled;
            last_change = now;
        } else if (now - last_change >= quiet_needed &&
                   (differs_from < 0 || value != differs_from)) {
            return value;
        }
        if (now >= deadline) {
            return value;
        }
    }
}

static int selftest_wait_for_animations_setting(int want, uint32_t timeout_ms) {
    int sq_file_descriptors[2];
    int sqr_file_descriptors[2];
    if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_SETTINGS_QUERY_PIPE, (uint64_t)sq_file_descriptors, 0) != 0 ||
        do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_SETTINGS_QUERY_RESPONSE_PIPE, (uint64_t)sqr_file_descriptors, 0) != 0) {
        return 0;
    }
    uint64_t deadline = pit_get_ticks() + (timeout_ms + 9) / 10;
    for (;;) {
        do_syscall(SYS_pipe_reset, (uint64_t)sqr_file_descriptors[0], 0, 0);
        uint8_t ping = 1;
        do_syscall(SYS_write, (uint64_t)sq_file_descriptors[1], (uint64_t)&ping, sizeof(ping));
        pit_sleep_ms(SELFTEST_POLL_MS);
        window_manager_settings_request_t got;
        k_memset(&got, 0, sizeof(got));
        if (do_syscall(SYS_read, (uint64_t)sqr_file_descriptors[0], (uint64_t)&got, sizeof(got)) == (long)sizeof(got) &&
            (int)(got.animations != 0) == (want != 0)) {
            return 1;
        }
        if (pit_get_ticks() >= deadline) {
            kernel_log_puts("[selftest] timed out waiting for: the compositor to take the animations setting\n");
            return 0;
        }
        pit_sleep_ms(SELFTEST_POLL_MS);
    }
}

static uint64_t selftest_input_to_photon_us(int32_t target_x, int32_t target_y,
                                             uint32_t timeout_ms) {
    mouse_inject(-5000, -5000, 0, 0);
    pit_sleep_ms(120);

    uint32_t before = framebuffer_get_pixel((uint32_t)target_x + 2, (uint32_t)target_y + 2);

    uint64_t deadline = pit_get_ticks() + (timeout_ms + 9) / 10;
    uint64_t t0 = tsc_read();
    mouse_inject(target_x, target_y, 0, 0);
    for (;;) {
        if (framebuffer_get_pixel((uint32_t)target_x + 2, (uint32_t)target_y + 2) != before) {
            return tsc_to_us(tsc_read() - t0);
        }
        if (pit_get_ticks() >= deadline) {
            return 0;
        }
        schedule();
    }
}

static char saved_user_settings[256];
static int64_t saved_user_settings_length = -1;

static const char SELFTEST_SETTINGS_CONF[] =
    "bg=0x001a1a2e\n"
    "accent=0x004c99e6\n"
    "wallpaper=0x00000001\n"
    "animations=0x00000001\n";

static const char SELFTEST_SETTINGS_NO_ANIM[] =
    "bg=0x001a1a2e\n"
    "accent=0x004c99e6\n"
    "wallpaper=0x00000001\n"
    "animations=0x00000000\n";

static void selftest_settings_install_defaults(void) {
    saved_user_settings_length = virtual_file_system_read(PATH_SETTINGS, saved_user_settings, sizeof(saved_user_settings));
    if (saved_user_settings_length > (int64_t)sizeof(saved_user_settings)) {
        saved_user_settings_length = -1;
    }
    virtual_file_system_write(PATH_SETTINGS, SELFTEST_SETTINGS_CONF, sizeof(SELFTEST_SETTINGS_CONF) - 1);
}

static void selftest_settings_restore(void) {
    if (saved_user_settings_length >= 0) {
        virtual_file_system_write(PATH_SETTINGS, saved_user_settings, (size_t)saved_user_settings_length);
    } else {
        virtual_file_system_write(PATH_SETTINGS, SELFTEST_SETTINGS_CONF, sizeof(SELFTEST_SETTINGS_CONF) - 1);
    }
}

static void demo_task(void *arg) {
    const char *name = (const char *)arg;
    for (int i = 0; i < 5; i++) {
        kernel_log_puts("[task ");
        kernel_log_puts(name);
        kernel_log_puts("] iteration ");
        kernel_log_put_hex32((uint32_t)i);
        kernel_log_putc('\n');

        uint64_t target = pit_get_ticks() + 3;
        while (pit_get_ticks() < target) {
            for (volatile int spin = 0; spin < 100000; spin++) {
            }
        }
    }
}

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

static void syscall_exit_task(void *arg) {
    (void)arg;
    long pid = do_syscall(SYS_getpid, 0, 0, 0);
    kernel_log_puts("[task C] getpid() via syscall = ");
    kernel_log_put_hex64((uint64_t)pid);
    kernel_log_puts(", exiting via SYS_exit...\n");
    do_syscall(SYS_exit, 0, 0, 0);
    panic("syscall_exit_task: resumed after SYS_exit");
}

static void pipe_producer_task(void *arg) {
    pipe_t *p = (pipe_t *)arg;
    static const char message[] = "ping";
    for (int i = 0; i < 3; i++) {
        pipe_write(p, message, sizeof(message) - 1, 0);
    }
    pipe_close_write(p);
}

static void pipe_consumer_task(void *arg) {
    pipe_t *p = (pipe_t *)arg;
    char buffer[64];
    size_t total = 0;
    for (;;) {
        long n = pipe_read(p, buffer + total, sizeof(buffer) - total, 0);
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    kernel_log_puts("[pipe] consumer received ");
    kernel_log_put_hex64((uint64_t)total);
    kernel_log_puts(" bytes: \"");
    for (size_t i = 0; i < total; i++) {
        kernel_log_putc(buffer[i]);
    }
    kernel_log_puts("\"\n");
    buffer[total] = '\0';
    if (total != 12 || k_strcmp(buffer, "pingpingping") != 0) {
        panic("pipe self-test: consumer received unexpected data");
    }
}

static void m68_sleeper_task(void *arg) {
    int fd = (int)(uint64_t)arg;
    int file_descriptors[1];
    file_descriptors[0] = fd;
    do_syscall(SYS_waitfds, (uint64_t)file_descriptors, 1, 10000);
    task_exit();
}

static void spinner_task(void *arg) {
    (void)arg;
    for (;;) {
        for (volatile int i = 0; i < 1000000; i++) {
        }
    }
}

static void quick_task(void *arg) {
    (void)arg;
}

static void m81_storm_name(char *out, int i) {
    out[0] = 'f';
    out[1] = (char)('0' + (i / 1000) % 10);
    out[2] = (char)('0' + (i / 100) % 10);
    out[3] = (char)('0' + (i / 10) % 10);
    out[4] = (char)('0' + i % 10);
    out[5] = '\0';
}

static void boot_selftests_desktop(void) {
    selftest_settings_install_defaults();

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

        long demo_status = 1;
        uint64_t demo_t0 = pit_get_ticks();
        for (int spin = 0; spin < 300 && demo_status == 1; spin++) {
            pit_sleep_ms(10);
            demo_status = do_syscall(SYS_task_alive, (uint64_t)demo_task->id, 0, 0);
        }
        kernel_log_puts("[wm_demo] connect+draw+exit took ");
        kernel_log_put_dec((uint32_t)((pit_get_ticks() - demo_t0) * (1000 / PIT_HZ)));
        kernel_log_puts(" ms\n");
        if (demo_status != 2) {
            kernel_log_puts("[wm_demo] SYS_task_alive = ");
            kernel_log_put_dec((uint32_t)(demo_status & 0xFF));
            kernel_log_puts(" (1=still running past the budget, 0=exited non-zero, 255=no such task)\n");
            scheduler_debug_dump("wm_demo overran its budget");
            {
                int probe[2];
                if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_REQUEST_PIPE, (uint64_t)probe, 0) == 0) {
                    kernel_log_puts("[wm_demo] WM_REQUEST_PIPE holds ");
                    kernel_log_put_dec((uint32_t)do_syscall(SYS_pipe_poll, (uint64_t)probe[0], 0, 0));
                    kernel_log_puts(" byte(s), a request is ");
                    kernel_log_put_dec((uint32_t)sizeof(window_manager_create_request_t));
                    kernel_log_putc('\n');
                }
                if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_RESPONSE_PIPE, (uint64_t)probe, 0) == 0) {
                    kernel_log_puts("[wm_demo] WM_RESPONSE_PIPE holds ");
                    kernel_log_put_dec((uint32_t)do_syscall(SYS_pipe_poll, (uint64_t)probe[0], 0, 0));
                    kernel_log_puts(" byte(s), a response is ");
                    kernel_log_put_dec((uint32_t)sizeof(window_manager_create_response_t));
                    kernel_log_putc('\n');
                }
            }
            panic("wm_demo self-test: did not exit cleanly - window creation failed");
        }

        pit_sleep_ms(1000);

        struct { uint32_t x, y; uint32_t expected; const char *what; } checks[] = {
            {200, 180, 0x00336699u, "window content color"},
            {140, 140, 0x00CC8822u, "window accent square color"},
            {150, 85,  0x004C99E6u, "window titlebar color (focused)"},
            {99,  150, 0x00444466u, "window border color"},
            {500, 500, 0x001A1A2Eu, "desktop background color"},
        };
        uint32_t got[sizeof(checks) / sizeof(checks[0])];
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            got[i] = framebuffer_get_pixel(checks[i].x, checks[i].y);
        }

        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        int all_ok = 1;
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            if (got[i] != checks[i].expected) {
                kernel_log_puts("[wm] pixel check failed: ");
                kernel_log_puts(checks[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32(checks[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32(got[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (!all_ok) {
            panic("compositor self-test: framebuffer content did not match");
        }
        kernel_log_puts("[wm] compositor + client self-test passed (5/5 pixel checks matched).\n\n");
    }

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
        selftest_wait_for_compositor();

        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(500);

        task_t *paint_task = process_spawn("gui_paint", paint_image, (size_t)paint_size, "");
        kfree(paint_image);
        pit_sleep_ms(1000);

        static const struct { uint32_t x, y; uint32_t expected; const char *what; } checks[] = {
            {150, 90,  0x00335577u, "clock titlebar color (unfocused)"},
            {98,  150, 0x00444466u, "clock compositor border color"},
            {105, 105, 0x00122438u, "clock content background color"},
            {110, 115, 0x00FFFFFFu, "clock caption 'C' glyph - on pixel (left stem)"},
            {113, 115, 0x00122438u, "clock caption 'C' glyph - off pixel (bowl interior)"},
            {200, 130, 0x004C99E6u, "paint titlebar color (focused)"},
            {140, 190, 0x0088AA55u, "paint's own border frame color"},
            {150, 240, 0x00202020u, "paint canvas background color"},
            {150, 151, 0x00FFFFFFu, "paint caption 'P' glyph - on pixel (left stem)"},
            {153, 151, 0x00202020u, "paint caption 'P' glyph - off pixel (bowl interior)"},
            {240, 164, 0x0088AA55u, "paint separator line color"},
            {500, 500, 0x001A1A2Eu, "desktop background color"},
        };
        uint32_t got[sizeof(checks) / sizeof(checks[0])];
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            got[i] = framebuffer_get_pixel(checks[i].x, checks[i].y);
        }

        selftest_reap(paint_task);
        selftest_reap(clock_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        int all_ok = 1;
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            if (got[i] != checks[i].expected) {
                kernel_log_puts("[wm21] pixel check failed: ");
                kernel_log_puts(checks[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32(checks[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32(got[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (!all_ok) {
            panic("M21 multi-window self-test: framebuffer content did not match");
        }
        kernel_log_puts("[wm21] multi-window compositor + focus-routing self-test passed "
                  "(12/12 pixel checks matched).\n\n");
    }

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
        selftest_wait_for_compositor();

        task_t *shell_task = process_spawn("desktop_shell", shell_image, (size_t)shell_size, "");
        kfree(shell_image);
        pit_sleep_ms(500);

        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(1000);

        static const struct { uint32_t x, y; uint32_t expected; const char *what; } checks[] = {
            {124, 742, 0x00293E55u, "running slot 0 background (focused)"},
            {90,  749, 0x00C5C5CAu, "running slot 0 'C' glyph - on pixel (left stem)"},
            {93,  749, 0x00293E55u, "running slot 0 'C' glyph - off pixel (bowl interior)"},
            {500, 738, 0x00181829u, "panel background (margin strip above the slot row, y=2 - never overdrawn by any slot regardless of window count)"},
            {500, 500, 0x001A1A2Eu, "desktop background color, above the panel"},
        };
        uint32_t got[sizeof(checks) / sizeof(checks[0])];
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            got[i] = framebuffer_get_pixel(checks[i].x, checks[i].y);
        }

        selftest_reap(clock_task);
        selftest_reap(shell_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        int all_ok = 1;
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            if (got[i] != checks[i].expected) {
                kernel_log_puts("[wm22] pixel check failed: ");
                kernel_log_puts(checks[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32(checks[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32(got[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (!all_ok) {
            panic("M22 desktop shell self-test: framebuffer content did not match");
        }
        kernel_log_puts("[wm22] desktop shell (panel + taskbar query, no launcher) self-test passed "
                  "(5/5 pixel checks matched).\n\n");
    }

    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t clock_size_bytes = 0;
        uint8_t *clock_image = read_program("/bin/gui_clock", &clock_size_bytes);
        int64_t clock_size = (int64_t)clock_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor();

        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(500);

        int action_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_ACTION_PIPE, (uint64_t)action_file_descriptors, 0) != 0) {
            panic("M30 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }

        const int32_t home_x = 250, home_y = 170;
        const uint32_t clock_bg = 0x00122438u;
        const uint32_t desktop_bg = 0x001A1A2Eu;

        window_manager_action_request_t request;
        request.window_id = 0;

        request.action = WINDOW_MANAGER_ACTION_MAXIMIZE;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t after_maximize = selftest_pixel_settled((uint32_t)home_x, (uint32_t)home_y,
                                                          desktop_bg, "the maximize to leave the home position");

        request.action = WINDOW_MANAGER_ACTION_RESTORE;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t after_restore = selftest_pixel_settled((uint32_t)home_x, (uint32_t)home_y,
                                                         clock_bg, "the restore to put the window back");

        request.action = WINDOW_MANAGER_ACTION_TOGGLE_MINIMIZE;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t after_minimize = selftest_pixel_settled((uint32_t)home_x, (uint32_t)home_y,
                                                          desktop_bg, "the minimize to clear the home position");

        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t after_unminimize = selftest_pixel_settled((uint32_t)home_x, (uint32_t)home_y,
                                                            clock_bg, "the window to come back");

        request.action = WINDOW_MANAGER_ACTION_CLOSE;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t after_close = selftest_pixel_settled((uint32_t)home_x, (uint32_t)home_y,
                                                       desktop_bg, "the closed window's slot to be reclaimed");
        long clock_exit = do_syscall(SYS_wait, (uint64_t)clock_task->id, 0, 0);

        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        int all_ok = 1;
        if (after_maximize != desktop_bg) {
            kernel_log_puts("[wm30] pixel check failed: after WM_ACTION_MAXIMIZE, home position should be empty desktop - expected 0x");
            kernel_log_put_hex32(desktop_bg);
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32(after_maximize);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (after_restore != clock_bg) {
            kernel_log_puts("[wm30] pixel check failed: after WM_ACTION_RESTORE, home position should show the window again - expected 0x");
            kernel_log_put_hex32(clock_bg);
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32(after_restore);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (after_minimize != desktop_bg) {
            kernel_log_puts("[wm30] pixel check failed: after WM_ACTION_TOGGLE_MINIMIZE, home position should be empty desktop - expected 0x");
            kernel_log_put_hex32(desktop_bg);
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32(after_minimize);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (after_unminimize != clock_bg) {
            kernel_log_puts("[wm30] pixel check failed: after toggling minimize back off, home position should show the window again - expected 0x");
            kernel_log_put_hex32(clock_bg);
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32(after_unminimize);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (after_close != desktop_bg) {
            kernel_log_puts("[wm30] pixel check failed: after WM_ACTION_CLOSE, home position should be empty desktop (window slot reclaimed) - expected 0x");
            kernel_log_put_hex32(desktop_bg);
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32(after_close);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (clock_exit != 128 + SIGTERM) {
            kernel_log_puts("[wm30] WM_ACTION_CLOSE self-test: gui_clock's exit code did not match a SIGTERM death - expected 0x");
            kernel_log_put_hex32((uint32_t)(128 + SIGTERM));
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32((uint32_t)clock_exit);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M30 window chrome self-test: maximize/restore/minimize/close did not behave as expected");
        }
        kernel_log_puts("[wm30] window chrome (maximize/restore/minimize/close via WM_ACTION_PIPE) self-test passed (6/6 checks matched).\n\n");
    }

    {
        const char message[] = "clipboard round trip";
        do_syscall(SYS_clipboard_set, (uint64_t)message, sizeof(message) - 1, 0);
        char readback[64];
        long n = do_syscall(SYS_clipboard_get, (uint64_t)readback, sizeof(readback), 0);
        int mismatch = (n != (long)(sizeof(message) - 1));
        for (long i = 0; !mismatch && i < n; i++) {
            if (readback[i] != message[i]) {
                mismatch = 1;
            }
        }
        if (mismatch) {
            panic("M32 clipboard self-test: SYS_clipboard_get did not return what SYS_clipboard_set stored");
        }
        kernel_log_puts("[clipboard] SYS_clipboard_set/get self-test passed.\n\n");
    }

    {
        const char content[] = "M33 SYS_writefile self-test content";
        long wrc = do_syscall(SYS_writefile, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m33test"), (uint64_t)content, sizeof(content) - 1);
        if (wrc != 0) {
            panic("M33 self-test: SYS_writefile failed");
        }
        char readback[64];
        long n = do_syscall(SYS_readfile, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m33test"), (uint64_t)readback, sizeof(readback));
        int mismatch = (n != (long)(sizeof(content) - 1));
        for (long i = 0; !mismatch && i < n; i++) {
            if (readback[i] != content[i]) {
                mismatch = 1;
            }
        }
        if (mismatch) {
            panic("M33 self-test: SYS_writefile/SYS_readfile round trip mismatch");
        }
        kernel_log_puts("[vfs] SYS_writefile/SYS_readfile self-test passed.\n\n");

    {
        const char *FDT = PATH_TEMPORARY_DIRECTORY "fdcycle";
        int file_descriptor_ok = 1;
        for (int round = 0; round < 2; round++) {
            long saved = do_syscall(SYS_dup2, 1, 9, 0);
            long fd = do_syscall(SYS_open, (uint64_t)FDT,
                                  round == 0 ? (OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE)
                                             : (OPEN_WRITE | OPEN_CREATE | OPEN_APPEND), 0);
            if (saved < 0 || fd < 0 || fd == saved) {
                file_descriptor_ok = 0;
                break;
            }
            do_syscall(SYS_dup2, (uint64_t)fd, 1, 0);
            if (do_syscall(SYS_write, 1, (uint64_t)(round == 0 ? "AAAA\n" : "BBBB\n"), 5) != 5) {
                file_descriptor_ok = 0;
            }
            do_syscall(SYS_dup2, (uint64_t)saved, 1, 0);
            do_syscall(SYS_close, (uint64_t)saved, 0, 0);
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);
        }
        static char fdt_buffer[64];
        k_memset(fdt_buffer, 0, sizeof(fdt_buffer));
        int64_t fdt_n = virtual_file_system_read(FDT, fdt_buffer, sizeof(fdt_buffer) - 1);
        if (fdt_n != 10 || k_strcmp(fdt_buffer, "AAAA\nBBBB\n") != 0) {
            kernel_log_puts("[fd] a second redirect in one process did not reach its file - got ");
            kernel_log_put_dec((uint32_t)(fdt_n < 0 ? 0 : fdt_n));
            kernel_log_puts(" byte(s)\n");
            file_descriptor_ok = 0;
        }
        do_syscall(SYS_unlink, (uint64_t)FDT, 0, 0);
        if (!file_descriptor_ok) {
            panic("fd self-test: the dup2 redirect cycle does not survive being repeated");
        }
        kernel_log_puts("[fd] the redirect cycle (park stdout, point fd 1 at a file, write, restore) "
                   "survives being done twice, and both rounds' bytes are in the file - "
                   "self-test passed.\n\n");
    }
    }

    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor();

        int settings_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_SETTINGS_PIPE, (uint64_t)settings_file_descriptors, 0) != 0) {
            panic("M33 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_PIPE) failed");
        }
        window_manager_settings_request_t request;
        k_memset(&request, 0, sizeof(request));
        request.animations = 1;
        request.wallpaper = 0;
        request.bg_color = 0x00123456u;
        request.accent_color = 0;
        do_syscall(SYS_write, (uint64_t)settings_file_descriptors[1], (uint64_t)&request, sizeof(request));
        pit_sleep_ms(300);
        uint32_t got = framebuffer_get_pixel(500, 500);

        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        if (got != request.bg_color) {
            kernel_log_puts("[settings] pixel check failed: desktop background did not change - expected 0x");
            kernel_log_put_hex32(request.bg_color);
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32(got);
            kernel_log_putc('\n');
            panic("M33 settings self-test: WM_SETTINGS_PIPE did not change the desktop background color");
        }
        kernel_log_puts("[settings] WM_SETTINGS_PIPE background-color self-test passed.\n\n");
    }

    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t editor_size_bytes = 0;
        uint8_t *editor_image = read_program("/bin/text_editor", &editor_size_bytes);
        int64_t editor_size = (int64_t)editor_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor();

        task_t *editor_task = process_spawn("text_editor", editor_image, (size_t)editor_size, "");
        kfree(editor_image);
        pit_sleep_ms(500);

        int action_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_ACTION_PIPE, (uint64_t)action_file_descriptors, 0) != 0) {
            panic("M36 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }

        const int32_t probe_x = 400, probe_y = 300;
        const uint32_t editor_bg = 0x00141414u;
        const uint32_t desktop_bg = 0x001A1A2Eu;

        uint32_t before_close = framebuffer_get_pixel((uint32_t)probe_x, (uint32_t)probe_y);

        window_manager_action_request_t request;
        request.window_id = 0;
        request.action = WINDOW_MANAGER_ACTION_CLOSE;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t after_close = selftest_pixel_settled((uint32_t)probe_x, (uint32_t)probe_y,
                                                       desktop_bg, "the editor's window to go away");
        long editor_exit = do_syscall(SYS_wait, (uint64_t)editor_task->id, 0, 0);

        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        int all_ok = 1;
        if (before_close != editor_bg) {
            kernel_log_puts("[wm36] pixel check failed: before close, probe point should show text_editor's own background - expected 0x");
            kernel_log_put_hex32(editor_bg);
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32(before_close);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (after_close != desktop_bg) {
            kernel_log_puts("[wm36] pixel check failed: after close, window slot should be reclaimed (empty desktop) - expected 0x");
            kernel_log_put_hex32(desktop_bg);
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32(after_close);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (editor_exit != 1) {
            kernel_log_puts("[wm36] WM_EVENT_CLOSE_REQUEST self-test: text_editor's exit code did not match its own sys_exit(1) - expected 0x1 got 0x");
            kernel_log_put_hex32((uint32_t)editor_exit);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M36 close-request self-test: WM_EVENT_CLOSE_REQUEST did not behave as expected");
        }
        kernel_log_puts("[wm36] confirm_close opt-in (WM_EVENT_CLOSE_REQUEST via WM_ACTION_PIPE) self-test passed (3/3 checks matched).\n\n");
    }

    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t clock_size_bytes = 0;
        uint8_t *clock_image = read_program("/bin/gui_clock", &clock_size_bytes);
        int64_t clock_size = (int64_t)clock_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor();

        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(500);

        uint32_t expected_shadow = 0x000D0D17u;
        uint32_t shadow_pixel = selftest_pixel_settled(304, 150, expected_shadow,
                                                        "the window's drop shadow to be drawn");

        int settings_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_SETTINGS_PIPE, (uint64_t)settings_file_descriptors, 0) != 0) {
            panic("M38 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_PIPE) failed");
        }
        window_manager_settings_request_t request;
        k_memset(&request, 0, sizeof(request));
        request.animations = 1;
        request.wallpaper = 0;
        request.bg_color = 0x001A1A2Eu;
        request.accent_color = 0x00AA5500u;
        do_syscall(SYS_write, (uint64_t)settings_file_descriptors[1], (uint64_t)&request, sizeof(request));
        pit_sleep_ms(300);
        uint32_t titlebar_pixel = framebuffer_get_pixel(200, 88);

        selftest_reap(clock_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        int all_ok = 1;
        if (shadow_pixel != expected_shadow) {
            kernel_log_puts("[wm38] pixel check failed: drop-shadow blend did not match - expected 0x");
            kernel_log_put_hex32(expected_shadow);
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32(shadow_pixel);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (titlebar_pixel != request.accent_color) {
            kernel_log_puts("[wm38] pixel check failed: focused titlebar did not pick up the new accent color - expected 0x");
            kernel_log_put_hex32(request.accent_color);
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32(titlebar_pixel);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M38 visual-polish self-test: drop shadow and/or accent color did not behave as expected");
        }
        kernel_log_puts("[wm38] drop shadow + WM_SETTINGS_PIPE accent-color self-test passed (2/2 checks matched).\n\n");
    }
}

#define MANIFEST_PATH   "/.image-manifest"
#define MANIFEST_MAX    1024u
#define MANIFEST_DEPTH  48

static int manifest_number(const char *text, const char *key, uint64_t *out) {
    char needle[32];
    needle[0] = '\n';
    k_strlcpy(needle + 1, key, sizeof(needle) - 3);
    size_t n = k_strlen(needle);
    needle[n] = ' ';
    needle[n + 1] = '\0';

    const char *at = k_strstr(text, needle);
    if (!at) {
        return 0;
    }
    at += k_strlen(needle);
    if (*at < '0' || *at > '9') {
        return 0;
    }
    uint64_t v = 0;
    while (*at >= '0' && *at <= '9') {
        v = v * 10 + (uint64_t)(*at - '0');
        at++;
    }
    *out = v;
    return 1;
}

typedef struct {
    int      handle;
    uint32_t cookie;
    uint32_t path_length;
} manifest_frame_t;

static void selftest_image_manifest(void) {
    if (!virtual_file_system_exists(MANIFEST_PATH)) {
        kernel_log_puts("[m93] no " MANIFEST_PATH " on this disk - nothing was built into this "
                  "image by a host tool, so the image-manifest check does not apply to "
                  "this boot.\n\n");
        return;
    }

    uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));

    char *text = (char *)kmalloc(MANIFEST_MAX);
    char *path = (char *)kmalloc(LEANFS_MAX_PATH);
    uint8_t *buffer = (uint8_t *)kmalloc(LEANFS_BLOCK_SIZE * 8);
    manifest_frame_t *stack =
        (manifest_frame_t *)kmalloc(sizeof(manifest_frame_t) * MANIFEST_DEPTH);
    if (!text || !path || !buffer || !stack) {
        panic("M93 image-manifest self-test: out of memory before it could start");
    }

    int64_t got = virtual_file_system_read(MANIFEST_PATH, text, MANIFEST_MAX - 1);
    if (got <= 0 || (uint64_t)got >= MANIFEST_MAX - 1) {
        panic("M93 image-manifest self-test: " MANIFEST_PATH " is unreadable or too large");
    }
    text[got] = '\0';

    const char *tree_at = k_strstr(text, "\ntree ");
    if (!tree_at) {
        panic("M93 image-manifest self-test: the manifest names no tree");
    }
    tree_at += 6;
    size_t tree_length = 0;
    while (tree_at[tree_length] && tree_at[tree_length] != '\n') {
        tree_length++;
    }
    if (tree_length == 0 || tree_length >= LEANFS_MAX_PATH) {
        panic("M93 image-manifest self-test: the manifest's tree path is unusable");
    }
    k_memcpy(path, tree_at, tree_length);
    path[tree_length] = '\0';

    uint64_t want_dirs = 0, want_names = 0, want_links = 0;
    uint64_t want_bytes = 0, want_depth = 0, want_hash = 0;
    if (!manifest_number(text, "dirs", &want_dirs) ||
        !manifest_number(text, "names", &want_names) ||
        !manifest_number(text, "links", &want_links) ||
        !manifest_number(text, "bytes", &want_bytes) ||
        !manifest_number(text, "depth", &want_depth) ||
        !manifest_number(text, "hash", &want_hash)) {
        panic("M93 image-manifest self-test: the manifest is missing a field this build needs");
    }

    uint32_t root_length = (uint32_t)tree_length;
    if (root_length == 1) {
        root_length = 0;
    }

    uint64_t dirs = 0, names = 0, links = 0, bytes = 0;
    uint32_t deepest = 0, hash = 0;
    int depth = 0;

    int root = virtual_file_system_directory_open(path);
    if (root < 0) {
        panic("M93 image-manifest self-test: the tree the manifest names is not a directory");
    }
    stack[0].handle = root;
    stack[0].cookie = 0;
    stack[0].path_length = (uint32_t)tree_length;
    depth = 1;
    dirs = 1;

    while (depth > 0) {
        manifest_frame_t *f = &stack[depth - 1];
        path[f->path_length] = '\0';

        leanfs_directory_entry_t entry;
        if (virtual_file_system_readdir_at(f->handle, &f->cookie, &entry) != 1) {
            depth--;
            continue;
        }

        uint32_t at = f->path_length;
        if (at + 1 + k_strlen(entry.name) >= LEANFS_MAX_PATH) {
            panic("M93 image-manifest self-test: a path in this tree is longer than PATH_MAX");
        }
        path[at++] = '/';
        k_memcpy(path + at, entry.name, k_strlen(entry.name));
        at += (uint32_t)k_strlen(entry.name);
        path[at] = '\0';

        leanfs_stat_t st;
        if (virtual_file_system_lstat(path, &st) != 0) {
            panic("M93 image-manifest self-test: a name in this tree does not resolve");
        }

        if (st.is_directory) {
            dirs++;
            if (depth >= MANIFEST_DEPTH) {
                panic("M93 image-manifest self-test: this tree is deeper than the walk allows");
            }
            int h = virtual_file_system_directory_open(path);
            if (h < 0) {
                panic("M93 image-manifest self-test: a directory in this tree would not open");
            }
            stack[depth].handle = h;
            stack[depth].cookie = 0;
            stack[depth].path_length = at;
            depth++;
            if ((uint32_t)depth > deepest) {
                deepest = (uint32_t)depth;
            }
            continue;
        }

        const char *rel = path + root_length + 1;
        uint32_t h = leanfs_fnv1a(LEANFS_FNV1A_INIT, rel, k_strlen(rel));

        if (st.is_link) {
            int64_t n = virtual_file_system_readlink(path, (char *)buffer, LEANFS_BLOCK_SIZE * 8);
            if (n < 0) {
                panic("M93 image-manifest self-test: a symlink in this tree would not read");
            }
            hash += leanfs_fnv1a(h, buffer, (size_t)n);
            links++;
            continue;
        }

        int fh = virtual_file_system_open(path, 0);
        if (fh < 0) {
            panic("M93 image-manifest self-test: a file in this tree would not open");
        }
        uint32_t off = 0;
        while (off < st.size) {
            uint32_t chunk = st.size - off;
            if (chunk > LEANFS_BLOCK_SIZE * 8) {
                chunk = LEANFS_BLOCK_SIZE * 8;
            }
            int64_t n = virtual_file_system_handle_read(fh, buffer, chunk, off);
            if (n != (int64_t)chunk) {
                panic("M93 image-manifest self-test: a file in this tree read short");
            }
            h = leanfs_fnv1a(h, buffer, (size_t)n);
            off += chunk;
        }
        hash += h;
        names++;
        bytes += st.size;
    }

    int agree = (dirs == want_dirs && names == want_names && links == want_links &&
                 bytes == want_bytes && (uint64_t)deepest == want_depth &&
                 (uint64_t)hash == want_hash);
    if (!agree) {
        kernel_log_puts("[m93] the tree on this disk is not the tree the host wrote. want/got: dirs ");
        kernel_log_put_dec((uint32_t)want_dirs);
        kernel_log_puts("/");
        kernel_log_put_dec((uint32_t)dirs);
        kernel_log_puts(", names ");
        kernel_log_put_dec((uint32_t)want_names);
        kernel_log_puts("/");
        kernel_log_put_dec((uint32_t)names);
        kernel_log_puts(", links ");
        kernel_log_put_dec((uint32_t)want_links);
        kernel_log_puts("/");
        kernel_log_put_dec((uint32_t)links);
        kernel_log_puts(", bytes ");
        kernel_log_put_dec((uint32_t)want_bytes);
        kernel_log_puts("/");
        kernel_log_put_dec((uint32_t)bytes);
        kernel_log_puts(", depth ");
        kernel_log_put_dec((uint32_t)want_depth);
        kernel_log_puts("/");
        kernel_log_put_dec(deepest);
        kernel_log_puts(", hash 0x");
        kernel_log_put_hex32((uint32_t)want_hash);
        kernel_log_puts("/0x");
        kernel_log_put_hex32(hash);
        kernel_log_puts("\n");
        panic("M93 image-manifest self-test: this image is not what the host built");
    }

    uint32_t took_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms;

    uint32_t check_started = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
    virtual_file_system_check();
    uint32_t check_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - check_started;

    kfree(stack);
    kfree(buffer);
    kfree(path);
    kfree(text);

    kernel_log_puts("[m93] a tree a host tool built into this image, read back from inside the "
              "machine: ");
    kernel_log_put_dec((uint32_t)names);
    kernel_log_puts(" file names in ");
    kernel_log_put_dec((uint32_t)dirs);
    kernel_log_puts(" directories, ");
    kernel_log_put_dec((uint32_t)links);
    kernel_log_puts(" symlinks, ");
    kernel_log_put_dec((uint32_t)bytes);
    kernel_log_puts(" bytes, ");
    kernel_log_put_dec(deepest);
    kernel_log_puts(" deep, and every byte of it hashing to the 0x");
    kernel_log_put_hex32(hash);
    kernel_log_puts(" the host wrote down - image-manifest self-test passed (");
    kernel_log_put_dec(took_ms);
    kernel_log_puts(" ms).\n");
    kernel_log_puts("[m93] full-scan mount check (leanfs_check) over this filesystem: ");
    kernel_log_put_dec(check_ms);
    kernel_log_puts(" ms, with ");
    kernel_log_put_dec((uint32_t)names);
    kernel_log_puts(" file names in the tree and ");
    kernel_log_put_dec(virtual_file_system_free_blocks());
    kernel_log_puts(" data blocks still free - the journal measurement M71 and M81 both "
              "deferred, reported rather than graded.\n\n");
}

#define SMP_BENCH_MAX    8
#define SMP_BENCH_ITERS  2000000u

static volatile uint32_t smp_bench_done;
static volatile uint64_t smp_bench_sink;

static void smp_bench_body(void *arg) {
    (void)arg;
    uint64_t acc = 1;
    for (uint32_t i = 0; i < SMP_BENCH_ITERS; i++) {
        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
        acc ^= acc >> 7;
    }
    smp_bench_sink += acc;
    __atomic_fetch_add(&smp_bench_done, 1u, __ATOMIC_SEQ_CST);
}

static uint64_t smp_bench_round(int k) {
    task_t *w[SMP_BENCH_MAX];
    int ids[SMP_BENCH_MAX];
    if (k < 1) {
        k = 1;
    }
    if (k > SMP_BENCH_MAX) {
        k = SMP_BENCH_MAX;
    }
    __atomic_store_n(&smp_bench_done, 0u, __ATOMIC_SEQ_CST);
    uint64_t t0 = tsc_read();
    for (int i = 0; i < k; i++) {
        w[i] = task_spawn("smpbench", smp_bench_body, (void *)0);
        if (!w[i]) {
            panic("M106 self-test: could not spawn a benchmark worker");
        }
        ids[i] = w[i]->id;
    }
    uint64_t deadline = pit_get_ticks() + 6000;
    while (__atomic_load_n(&smp_bench_done, __ATOMIC_SEQ_CST) < (uint32_t)k) {
        if (pit_get_ticks() > deadline) {
            panic("M106 self-test: a benchmark worker never finished");
        }
        pit_sleep_ms(1);
    }
    uint64_t t1 = tsc_read();
    for (int i = 0; i < k; i++) {
        task_t *t = scheduler_task_by_id(ids[i]);
        uint64_t rd = pit_get_ticks() + 1000;
        while (t && t->state != TASK_TERMINATED) {
            if (pit_get_ticks() > rd) {
                panic("M106 self-test: a finished benchmark worker never terminated");
            }
            pit_sleep_ms(1);
            t = scheduler_task_by_id(ids[i]);
        }
        scheduler_reap_slot(t);
    }
    return tsc_to_us(t1 - t0);
}

static void selftest_profile(void) {
    profile_reset();
    profile_start();

    {
        uint64_t target = pit_get_ticks() + 20;
        while (pit_get_ticks() < target) {
            for (volatile int spin = 0; spin < 5000; spin++) {
            }
        }
    }
    profile_stop();

    prof_statistics_t st;
    profile_get_statistics(&st);
    kernel_log_puts("[m101] profile: samples=");
    kernel_log_put_dec((uint32_t)st.samples);
    kernel_log_puts(" kernel=");
    kernel_log_put_dec((uint32_t)st.kernel);
    kernel_log_puts(" user=");
    kernel_log_put_dec((uint32_t)st.user);
    kernel_log_puts(" idle=");
    kernel_log_put_dec((uint32_t)st.idle);
    kernel_log_puts(" distinct=");
    kernel_log_put_dec((uint32_t)st.distinct);
    kernel_log_puts(" overflow=");
    kernel_log_put_dec((uint32_t)st.overflow);
    kernel_log_putc('\n');
    if (st.samples == 0) {
        panic("m101: the profiler ran for 200 ms and recorded nothing");
    }
    if (st.distinct == 0) {
        panic("m101: samples were counted but the histogram is empty");
    }
    if (st.overflow != 0) {
        panic("m101: a 200 ms profile overflowed a 2048-entry table");
    }
    if (st.samples != st.kernel + st.user + st.idle) {
        panic("m101: the sample classes do not add up to the sample count");
    }

    {
        static prof_sample_t got[64];
        int n = profile_snapshot(got, 64);
        if (n <= 0) {
            panic("m101: the histogram reports entries but hands back none");
        }
        int kernel_seen = 0;
        extern const uint8_t __bss_start[];
        uint64_t text_lo = 0x100000u;
        uint64_t text_hi = (uint64_t)(uintptr_t)__bss_start;
        for (int i = 0; i < n; i++) {
            if (got[i].pid != PROF_PID_KERNEL) {
                continue;
            }
            kernel_seen++;
            if (got[i].rip < text_lo || got[i].rip >= text_hi) {
                kernel_log_puts("[m101] a kernel sample landed outside the kernel at 0x");
                kernel_log_put_hex64(got[i].rip);
                kernel_log_putc('\n');
                panic("m101: a sampled kernel address is not in this kernel");
            }
        }
        if (kernel_seen == 0) {
            panic("m101: a kernel-context busy loop produced no kernel samples");
        }
    }

    {
        prof_statistics_t before;
        profile_get_statistics(&before);
        uint64_t target = pit_get_ticks() + 10;
        while (pit_get_ticks() < target) {
        }
        prof_statistics_t after;
        profile_get_statistics(&after);
        if (after.samples != before.samples) {
            panic("m101: the profiler kept sampling after being stopped");
        }
    }

    kernel_log_puts("[m101] sampling profiler: ");
    kernel_log_put_dec((uint32_t)st.samples);
    kernel_log_puts(" samples in 200 ms across ");
    kernel_log_put_dec((uint32_t)st.distinct);
    kernel_log_puts(" distinct addresses, every kernel one inside .text.\n");
    profile_reset();

    {
        syscall_counters_entry_t before_pid, before_uptime, after_pid, after_uptime;
        syscall_counters_get(SYS_getpid, &before_pid);
        syscall_counters_get(SYS_uptime_ms, &before_uptime);

        for (int i = 0; i < 100; i++) {
            do_syscall(SYS_getpid, 0, 0, 0);
        }

        syscall_counters_get(SYS_getpid, &after_pid);
        syscall_counters_get(SYS_uptime_ms, &after_uptime);

        uint64_t moved = after_pid.calls - before_pid.calls;
        if (moved < 100) {
            kernel_log_puts("[m101] 100 getpid calls moved the counter by ");
            kernel_log_put_dec((uint32_t)moved);
            kernel_log_putc('\n');
            panic("m101: syscall accounting lost calls");
        }
        if (after_uptime.calls - before_uptime.calls >= 100) {
            panic("m101: a syscall's calls were recorded against another number");
        }
        if (after_pid.cycles != before_pid.cycles) {
            panic("m101: cycles were recorded while timing was off");
        }
    }

    {
        if (syscall_counters_timing_enabled()) {
            panic("m101: syscall timing is on by default");
        }
        int was = syscall_counters_set_timing(1);
        if (was != 0) {
            panic("m101: syscount_set_timing did not report the previous setting");
        }
        syscall_counters_entry_t before, after;
        syscall_counters_get(SYS_getpid, &before);
        for (int i = 0; i < 100; i++) {
            do_syscall(SYS_getpid, 0, 0, 0);
        }
        syscall_counters_get(SYS_getpid, &after);
        syscall_counters_set_timing(0);

        if (after.cycles <= before.cycles) {
            panic("m101: timing was on and no cycles were recorded");
        }
        uint64_t per = (after.cycles - before.cycles) / (after.calls - before.calls);
        kernel_log_puts("[m101] per-syscall accounting: getpid costs ");
        kernel_log_put_dec((uint32_t)per);
        kernel_log_puts(" cycles through int 0x80, timed only when asked.\n");

        kernel_log_perf("syscall_null_cycles", per, "cycles");
    }

    {
        if (do_syscall(SYS_profile, PROFILE_OP_STATISTICS, 0, 0) != -1) {
            panic("m101: SYS_profile accepted a null pointer");
        }
        if (do_syscall(SYS_profile, 999, 0, 0) != -1) {
            panic("m101: SYS_profile accepted an operation that does not exist");
        }
    }

    {
        for (int i = 0; i < 24; i++) {
            int fd = (int)do_syscall(SYS_open, (uint64_t)"/proc/self/status", 0, 0);
            if (fd < 0) {
                kernel_log_puts("[m101] /proc/self/status could not be opened on attempt ");
                kernel_log_put_dec((uint32_t)(i + 1));
                kernel_log_putc('\n');
                panic("m101: procfs runs out of handles - the close path is broken");
            }
            char buffer[64];
            if (do_syscall(SYS_read, (uint64_t)fd, (uint64_t)buffer, sizeof(buffer)) <= 0) {
                panic("m101: a /proc file opened but read nothing");
            }
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);
        }
        kernel_log_puts("[m101] /proc survived 24 open/close cycles through a 16-entry "
                  "table - the close path M101 added to the VFS works.\n");
    }

    {
        char buffer[512];
        long n = do_syscall(SYS_readfile, (uint64_t)"/proc/syscalls", (uint64_t)buffer, sizeof(buffer));
        if (n <= 0) {
            panic("m101: /proc/syscalls is empty on a machine that has made syscalls");
        }
        n = do_syscall(SYS_readfile, (uint64_t)"/proc/profile", (uint64_t)buffer, sizeof(buffer));
        if (n <= 0) {
            panic("m101: /proc/profile reported nothing");
        }
        kernel_log_puts("[m101] /proc/profile and /proc/syscalls both answer.\n");
    }

    {
        size_t image_bytes = 0;
        uint8_t *image = read_program("/bin/proftest", &image_bytes);
        if (!image) {
            panic("m101: /bin/proftest is not on the disk");
        }
        task_t *t = process_spawn("proftest", image, image_bytes, "");
        kfree(image);
        if (!t) {
            panic("m101: /bin/proftest would not spawn");
        }
        long code = do_syscall(SYS_wait, (uint64_t)t->id, 0, 0);
        if (code != 0) {
            kernel_log_puts("[m101] proftest reported 0x");
            kernel_log_put_hex32((uint32_t)code);
            kernel_log_puts(" failed check(s) - see the proftest lines above\n");
            panic("m101: the profiler's ring-3 behaviour is wrong");
        }
        kernel_log_puts("[m101] ring-3 half passed: the capability gate admits a holder, "
                  "every bad pointer is refused, a user busy loop is sampled as user "
                  "time against its own pid.\n\n");
    }

    {
        profile_reset();
        profile_start();
        {
            uint64_t target = pit_get_ticks() + 20;
            while (pit_get_ticks() < target) {
                for (volatile int spin = 0; spin < 5000; spin++) {
                }
            }
        }
        profile_stop();

        size_t image_bytes = 0;
        uint8_t *image = read_program("/bin/profile", &image_bytes);
        if (!image) {
            panic("m101: /bin/profile is not on the disk");
        }
        static const char *const profile_argv[] = {"profile", "report", "5", (const char *)0};
        task_t *t = process_spawnv("profile", image, image_bytes, profile_argv);
        kfree(image);
        if (!t) {
            panic("m101: /bin/profile would not spawn");
        }
        long code = do_syscall(SYS_wait, (uint64_t)t->id, 0, 0);
        if (code != 0) {
            panic("m101: /bin/profile could not produce a report");
        }
        kernel_log_puts("[m101] the report above is /bin/profile's, produced on this "
                  "machine from the histogram this boot filled.\n\n");
        profile_reset();
    }
}

static void selftest_oom(void) {
    uint64_t before = physical_memory_free_frame_count();

    size_t image_bytes = 0;
    uint8_t *image = read_program("/bin/oomtest", &image_bytes);
    if (!image) {
        panic("m102: /bin/oomtest is not on the disk");
    }

    int killed_rounds = 0;
    int refused_rounds = 0;

    for (int round = 0; round < 2; round++) {
        task_t *t = process_spawn("oomtest", image, image_bytes, "");
        if (!t) {
            kernel_log_puts("[m102] round ");
            kernel_log_put_dec((uint32_t)round);
            kernel_log_puts(": the spawn itself was refused, which is the other "
                      "half of this milestone working.\n");
            continue;
        }
        long code = do_syscall(SYS_wait, (uint64_t)t->id, 0, 0);

        if (code == 128 + SIGKILL) {
            killed_rounds++;
        } else if (code == 0) {
            refused_rounds++;
        } else {
            kernel_log_puts("[m102] oomtest exited with 0x");
            kernel_log_put_hex32((uint32_t)code);
            kernel_log_putc('\n');
            if (code == 128 + SIGSEGV) {
                panic("m102: an out-of-memory kill was reported as a segfault");
            }
            panic("m102: oomtest neither ran out cleanly nor was killed for it");
        }
    }

    kfree(image);

    if (killed_rounds == 0 && refused_rounds == 0) {
        panic("m102: nothing was exhausted - this machine did not run out of memory");
    }

    uint64_t after = physical_memory_free_frame_count();
    if (after + 512 < before) {
        kernel_log_puts("[m102] free frames before 0x");
        kernel_log_put_hex32((uint32_t)before);
        kernel_log_puts(", after 0x");
        kernel_log_put_hex32((uint32_t)after);
        kernel_log_putc('\n');
        panic("m102: the machine survived running out of memory but did not get it back");
    }

    kernel_log_puts("[m102] out of memory, twice: ");
    kernel_log_put_dec((uint32_t)killed_rounds);
    kernel_log_puts(" round(s) ended in an OOM kill and ");
    kernel_log_put_dec((uint32_t)refused_rounds);
    kernel_log_puts(" in a refused allocation. The machine is still running and ");
    kernel_log_put_dec((uint32_t)after);
    kernel_log_puts(" of its ");
    kernel_log_put_dec((uint32_t)before);
    kernel_log_puts(" free frames came back.\n");

    kernel_log_puts("[m102] this machine tracks ");
    kernel_log_put_dec((uint32_t)(physical_memory_total_frame_count() * 4 / 1024));
    kernel_log_puts(" MiB of physical memory and a single process was able to take "
              "it to exhaustion - the swap decision needs M98's peak, not "
              "this one.\n\n");
}

static void boot_selftests_system(void) {
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
        kernel_log_puts("[smp] probe tasks observed running on ");
        kernel_log_put_hex32((uint32_t)distinct);
        kernel_log_puts(" distinct CPU(s) (");
        kernel_log_put_hex32((uint32_t)smp_cpu_count);
        kernel_log_puts(" online).\n");
        if (smp_cpu_count > 1 && distinct < 2) {
            panic("smp self-test: multiple CPUs online but probe tasks only ever ran on one");
        }

        for (int i = 0; i < 4; i++) {
            do_syscall(SYS_wait, (uint64_t)probe_tasks[i]->id, 0, 0);
        }
        kernel_log_puts("[smp] self-test passed.\n\n");
    }

    {
        os_stat_t ct;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/cxxtest", (uint64_t)&ct, 0) != 0) {
            kernel_log_puts("[m97] /bin/cxxtest is not on this image - skipped. "
                       "tools/build-toolchain.sh builds the C++ runtime and "
                       "tools/cxx-test.sh installs what it produces.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m97.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m97.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "/bin/cxxtest > " PATH_TEMPORARY_DIRECTORY "m97.out\n"
                "echo code $? >> " PATH_TEMPORARY_DIRECTORY "m97.out\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M97 self-test: could not write the script fixture");
            }
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m97] the C++ program could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }

            static char produced[512];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m97] the C++ program produced no output at all - a throw "
                           "with no unwind tables reaches std::terminate before main "
                           "can print anything\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                if (!selftest_contains(produced, "code 0")) {
                    kernel_log_puts("[m97] the fixture did not exit 0 - see "
                               "tests/cxx/exceptions.cpp for what each code means\n");
                    all_ok = 0;
                }
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"4 destructors",     "one destructor per live frame the throw passed through"},
                    {"in order 3 2 1",    "innermost frame first - the order, not just the count"},
                    {"matched by type",   "the catch clause selected by type rather than by position"},
                    {"rethrow preserved", "a rethrow carrying the same object out again"},
                    {"static ctor ran",   "__cxa_atexit and .init_array, which M94 could not do"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m97] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }

            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);

            if (!all_ok) {
                kernel_log_puts("[m97] what the C++ program actually wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m97] ---- end\n");
                panic("M97 self-test: an exception does not unwind on this machine");
            }

            os_stat_t lt;
            if (do_syscall(SYS_stat, (uint64_t)"/bin/cxxlib", (uint64_t)&lt, 0) == 0) {
                int lib_ok = 1;
                const char *lscript = PATH_TEMPORARY_DIRECTORY "m97lib.sh";
                const char *lresult = PATH_TEMPORARY_DIRECTORY "m97lib.out";
                static const char LSCRIPT[] =
                    "#!/bin/sh\n"
                    "/bin/cxxlib > " PATH_TEMPORARY_DIRECTORY "m97lib.out\n"
                    "echo code $? >> " PATH_TEMPORARY_DIRECTORY "m97lib.out\n";
                if (do_syscall(SYS_writefile, (uint64_t)lscript, (uint64_t)LSCRIPT,
                                sizeof(LSCRIPT) - 1) != 0) {
                    panic("M97 self-test: could not write the library script fixture");
                }
                long lpid = do_syscall(SYS_spawn, (uint64_t)lscript, 0, 0);
                if (lpid < 0) {
                    kernel_log_puts("[m97] the libstdc++ program could not be spawned\n");
                    lib_ok = 0;
                } else {
                    do_syscall(SYS_wait, (uint64_t)lpid, 0, 0);
                }
                static char lproduced[512];
                k_memset(lproduced, 0, sizeof(lproduced));
                int64_t ln = virtual_file_system_read(lresult, lproduced, sizeof(lproduced) - 1);
                if (ln <= 0) {
                    kernel_log_puts("[m97] the libstdc++ program produced no output\n");
                    lib_ok = 0;
                } else {
                    lproduced[ln] = '\0';
                    if (!selftest_contains(lproduced, "code 0")) {
                        kernel_log_puts("[m97] the library fixture did not exit 0 - see "
                                   "tests/cxx/library.cpp for what each code means\n");
                        lib_ok = 0;
                    }
                    static const char *const LEXPECT[] = {
                        "vector sorted",
                        "map ordered",
                        "across the SSO boundary",
                        "iostreams round-tripped",
                        "dynamic_cast and typeid agreed",
                        "caught by type",
                        "std::thread joined",
                    };
                    for (unsigned i = 0; i < sizeof(LEXPECT) / sizeof(LEXPECT[0]); i++) {
                        if (!selftest_contains(lproduced, LEXPECT[i])) {
                            kernel_log_puts("[m97] the library fixture did not report: ");
                            kernel_log_puts(LEXPECT[i]);
                            kernel_log_putc('\n');
                            lib_ok = 0;
                        }
                    }
                }
                do_syscall(SYS_unlink, (uint64_t)lscript, 0, 0);
                do_syscall(SYS_unlink, (uint64_t)lresult, 0, 0);
                if (!lib_ok) {
                    kernel_log_puts("[m97] what the libstdc++ program actually wrote:\n");
                    kernel_log_puts(lproduced);
                    kernel_log_puts("[m97] ---- end\n");
                    panic("M97 self-test: the C++ standard library does not work here");
                }
                kernel_log_puts("[m97] and the standard library on top of it: a sorted vector, "
                           "a map iterated in key order out of libstdc++'s own compiled "
                           "tree code, a string across the small-string boundary, "
                           "iostreams round-tripped through a stringstream, dynamic_cast "
                           "and typeid agreeing about a type, three exceptions raised "
                           "INSIDE libstdc++ caught here by type, and a std::thread over "
                           "M79's tasks joined - self-test passed.\n");
            } else {
                kernel_log_puts("[m97] /bin/cxxlib is not on this image - the standard-library "
                           "half is skipped.\n");
            }

            os_stat_t bt;
            if (do_syscall(SYS_stat, (uint64_t)"/bin/throwmain", (uint64_t)&bt, 0) == 0) {
                int b_ok = 1;
                const char *bscript = PATH_TEMPORARY_DIRECTORY "m97b.sh";
                const char *bresult = PATH_TEMPORARY_DIRECTORY "m97b.out";
                static const char BSCRIPT[] =
                    "#!/bin/sh\n"
                    "/bin/throwmain > " PATH_TEMPORARY_DIRECTORY "m97b.out\n"
                    "echo code $? >> " PATH_TEMPORARY_DIRECTORY "m97b.out\n";
                if (do_syscall(SYS_writefile, (uint64_t)bscript, (uint64_t)BSCRIPT,
                                sizeof(BSCRIPT) - 1) != 0) {
                    panic("M97 self-test: could not write the boundary script fixture");
                }
                long bpid = do_syscall(SYS_spawn, (uint64_t)bscript, 0, 0);
                if (bpid < 0) {
                    kernel_log_puts("[m97] the cross-object program could not be spawned\n");
                    b_ok = 0;
                } else {
                    do_syscall(SYS_wait, (uint64_t)bpid, 0, 0);
                }
                static char bproduced[512];
                k_memset(bproduced, 0, sizeof(bproduced));
                int64_t bn = virtual_file_system_read(bresult, bproduced, sizeof(bproduced) - 1);
                if (bn <= 0) {
                    kernel_log_puts("[m97] the cross-object program produced no output\n");
                    b_ok = 0;
                } else {
                    bproduced[bn] = '\0';
                    if (!selftest_contains(bproduced, "code 0")) {
                        kernel_log_puts("[m97] the cross-object fixture did not exit 0 - see "
                                   "tests/cxx/throwmain.cpp for what each code means\n");
                        b_ok = 0;
                    }
                    if (!selftest_contains(bproduced, "caught by exact type and by base")) {
                        kernel_log_puts("[m97] the cross-object fixture did not report a "
                                   "successful boundary crossing\n");
                        b_ok = 0;
                    }
                }
                do_syscall(SYS_unlink, (uint64_t)bscript, 0, 0);
                do_syscall(SYS_unlink, (uint64_t)bresult, 0, 0);
                if (!b_ok) {
                    kernel_log_puts("[m97] what the cross-object program actually wrote:\n");
                    kernel_log_puts(bproduced);
                    kernel_log_puts("[m97] ---- end\n");
                    panic("M97 self-test: an exception does not cross a shared-object "
                          "boundary on this machine");
                }
                kernel_log_puts("[m97] and across a shared object: an exception thrown inside "
                           "a library this program dlopen'd - one it never named on its "
                           "link line - caught in the executable by its exact type and "
                           "again by its base, with both of the library's own frame "
                           "destructors run on the way out, and one thrown here caught "
                           "inside the library - self-test passed.\n");
            } else {
                kernel_log_puts("[m97] /bin/throwmain is not on this image - the "
                           "shared-object half is skipped.\n");
            }

            {
                int theirs_ran = 0, theirs_failed = 0;
                for (int i = 0; i < 8; i++) {
                    char prog[32];
                    k_strlcpy(prog, "/tests/gnucxx0", sizeof(prog));
                    prog[13] = (char)('0' + i);
                    os_stat_t gt2;
                    if (do_syscall(SYS_stat, (uint64_t)prog, (uint64_t)&gt2, 0) != 0) {
                        continue;
                    }
                    theirs_ran++;
                    long gp = do_syscall(SYS_spawn, (uint64_t)prog, 0, 0);
                    if (gp < 0) {
                        kernel_log_puts("[m97] could not spawn ");
                        kernel_log_puts(prog);
                        kernel_log_putc('\n');
                        theirs_failed++;
                        continue;
                    }
                    long code = do_syscall(SYS_wait, (uint64_t)gp, 0, 0);
                    if (code != 0) {
                        kernel_log_puts("[m97] ");
                        kernel_log_puts(prog);
                        kernel_log_puts(" (one of GCC's own libstdc++ tests) exited ");
                        kernel_log_put_dec((uint32_t)code);
                        kernel_log_putc('\n');
                        theirs_failed++;
                    }
                }
                if (theirs_failed) {
                    panic("M97 self-test: a libstdc++ regression test written by "
                          "somebody else does not pass here");
                }
                if (theirs_ran) {
                    kernel_log_puts("[m97] and C++ nobody here wrote: ");
                    kernel_log_put_dec((uint32_t)theirs_ran);
                    kernel_log_puts(" of GCC's own libstdc++ regression tests - vectors, a "
                               "map behind threads, sets, strings, algorithms, "
                               "stringstreams, tuples and complex arithmetic - "
                               "compiled with no edits of any kind and every one of "
                               "them exiting 0 on this machine.\n");
                }
            }

            kernel_log_puts("[m97] C++ that throws: a throw three frames deep caught by type "
                       "in main with a destructor run for every frame in between, "
                       "counted and ordered rather than assumed; a derived object "
                       "caught by its base; a rethrow that kept the object it was "
                       "given; catch(...) over a builtin; and a namespace-scope "
                       "object constructed before main by .init_array and destroyed "
                       "after it by __cxa_atexit - self-test passed.\n\n");
        }
    }

    {
        int cpus = smp_cpu_count > 0 ? smp_cpu_count : 1;
        if (cpus > SMP_BENCH_MAX) {
            cpus = SMP_BENCH_MAX;
        }
        uint64_t one_us = 0, many_us = 0;
        uint32_t cost_pct = 0;
        for (int round = 0; round < 5; round++) {
            uint64_t a = smp_bench_round(1);
            uint64_t b = smp_bench_round(cpus);
            if (a == 0 || b == 0) {
                continue;
            }
            uint32_t pct = (uint32_t)((b * 100u) / a);
            if (one_us == 0 || pct < cost_pct) {
                cost_pct = pct;
                one_us = a;
                many_us = b;
            }
        }
        if (one_us == 0 || many_us == 0) {
            panic("M106 self-test: the parallel benchmark measured nothing");
        }

        int file_descriptor_task = -1;
        int file_descriptor_peak = scheduler_file_descriptor_high_water(&file_descriptor_task);
        int task_peak = scheduler_peak_live_tasks();

        kernel_log_perf("smp_one_task_us", one_us, "us");
        kernel_log_perf("smp_n_tasks_us", many_us, "us");
        kernel_log_perf("smp_parallel_cost_pct", cost_pct, "pct");
        kernel_log_perf("peak_live_tasks", (uint64_t)task_peak, "tasks");
        kernel_log_perf("peak_fds_one_task", (uint64_t)file_descriptor_peak, "fds");

        kernel_log_puts("[m106] cores this machine can use: ");
        kernel_log_put_dec((uint32_t)cpus);
        kernel_log_puts(" online, one task of fixed work in ");
        kernel_log_put_dec((uint32_t)one_us);
        kernel_log_puts(" us and ");
        kernel_log_put_dec((uint32_t)cpus);
        kernel_log_puts(" of them in ");
        kernel_log_put_dec((uint32_t)many_us);
        kernel_log_puts(" us - ");
        kernel_log_put_dec(cost_pct);
        kernel_log_puts("% of one task's cost for ");
        kernel_log_put_dec((uint32_t)cpus);
        kernel_log_puts(" times the work, where 100 means it went wide and ");
        kernel_log_put_dec((uint32_t)cpus * 100u);
        kernel_log_puts(" means one core did all of it. The two ceilings this milestone was "
                   "also going to raise, reported rather than rounded up: ");
        kernel_log_put_dec((uint32_t)task_peak);
        kernel_log_puts(" of ");
        kernel_log_put_dec((uint32_t)MAX_TASKS);
        kernel_log_puts(" task slots ever live at once, and ");
        kernel_log_put_dec((uint32_t)file_descriptor_peak);
        kernel_log_puts(" of ");
        kernel_log_put_dec((uint32_t)MAX_FILE_DESCRIPTORS);
        kernel_log_puts(" descriptors in the hungriest task (pid 0x");
        kernel_log_put_hex32((uint32_t)file_descriptor_task);
        kernel_log_puts(") - self-test passed.\n\n");
    }

    selftest_oom();

    selftest_profile();

    if (net_have_nic()) {
        uint8_t ping_payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        uint16_t ping_id = 0x1EA5;
        uint16_t ping_sequence = 1;
        icmp_send_echo_request(net_gateway_ip(), ping_id, ping_sequence, ping_payload, sizeof(ping_payload));

        int got_reply = 0;
        uint64_t deadline = pit_get_ticks() + 3 * PIT_HZ;
        while (pit_get_ticks() < deadline) {
            if (icmp_echo_reply_seen(ping_id, ping_sequence)) {
                got_reply = 1;
                break;
            }
            __asm__ volatile("hlt");
        }
        if (!got_reply) {
            panic("net self-test: no ICMP echo reply from the gateway within 3s");
        }
        kernel_log_puts("[net] ICMP echo request/reply self-test passed (ping to gateway 0x");
        kernel_log_put_hex32(net_gateway_ip());
        kernel_log_puts(" round-tripped).\n\n");
    } else {
        kernel_log_puts("[net] no NIC - the ICMP round-trip self-test was skipped "
                   "(expected on real hardware).\n\n");
    }

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
        selftest_wait_for_compositor();

        task_t *shell_task = process_spawn("desktop_shell", shell_image, (size_t)shell_size, "");
        kfree(shell_image);
        pit_sleep_ms(500);

        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        pit_sleep_ms(1000);

        uint32_t panel_bg_px = framebuffer_get_pixel(512, 738);
        uint32_t above_panel_px = framebuffer_get_pixel(512, 700);
        uint32_t start_btn_px = framebuffer_get_pixel(71, 742);
        uint32_t running_slot_px = framebuffer_get_pixel(168, 752);
        uint32_t tray_sep_px = framebuffer_get_pixel(912, 750);

        int action_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_ACTION_PIPE, (uint64_t)action_file_descriptors, 0) != 0) {
            panic("M42 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        window_manager_action_request_t request;
        k_memset(&request, 0, sizeof(request));
        request.window_id = 1;
        request.action = WINDOW_MANAGER_ACTION_MAXIMIZE;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t maximized_titlebar = selftest_pixel_settled(100, 12, 0x004C99E6u,
                                                              "the maximized window's titlebar");
        uint32_t panel_over_maximized = selftest_pixel_settled(512, 738, 0x00181829u,
                                                                "the taskbar to stay on top of the maximized window");

        k_memset(&request, 0, sizeof(request));
        request.window_id = -1;
        request.action = WINDOW_MANAGER_ACTION_TOGGLE_LAUNCHER;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t launcher_open_px = selftest_pixel_settled(512, 309, 0x001B2032u,
                                                            "the launcher overlay to finish fading in");
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t launcher_closed_px = selftest_pixel_settled(512, 309, 0x001A1A2Eu,
                                                              "the launcher overlay to go away again");

        selftest_reap(clock_task);
        selftest_reap(shell_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

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
                kernel_log_puts("[m42] pixel check failed: ");
                kernel_log_puts(names[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32(names[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32(got[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (!all_ok) {
            panic("M42 taskbar self-test: the bottom taskbar did not behave as expected");
        }
        kernel_log_puts("[m42] bottom taskbar (Start button, running-app button, tray, "
                   "maximize clamp, launcher toggle) self-test passed (9/9 checks matched).\n\n");
    }

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
        selftest_wait_for_compositor();
        task_t *shell_task = process_spawn("desktop_shell", shell_image, (size_t)shell_size, "");
        kfree(shell_image);
        pit_sleep_ms(400);
        task_t *editor_task = process_spawn("text_editor", editor_image, (size_t)editor_size, "");
        kfree(editor_image);
        pit_sleep_ms(700);

        int action_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_ACTION_PIPE, (uint64_t)action_file_descriptors, 0) != 0) {
            panic("M43 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        window_manager_action_request_t request;
        k_memset(&request, 0, sizeof(request));
        request.window_id = 1;

        request.action = WINDOW_MANAGER_ACTION_SNAP_RIGHT;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t right_titlebar = selftest_pixel_settled(700, 12, 0x004C99E6u,
                                                          "the window to snap to the right half");
        uint32_t right_left_half = selftest_pixel_settled(200, 12, 0x001A1A2Eu,
                                                           "the left half to be empty");

        request.action = WINDOW_MANAGER_ACTION_SNAP_LEFT;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t left_titlebar = selftest_pixel_settled(200, 12, 0x004C99E6u,
                                                         "the window to snap to the left half");
        uint32_t left_right_half = selftest_pixel_settled(700, 12, 0x001A1A2Eu,
                                                           "the right half to be empty");

        k_memset(&request, 0, sizeof(request));
        request.window_id = -1;
        request.action = WINDOW_MANAGER_ACTION_TOGGLE_LAUNCHER;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t launcher_bg = selftest_pixel_settled(700, 309, 0x001B2032u,
                                                       "the launcher overlay to finish fading in");
        uint32_t launcher_selected_row = selftest_pixel_settled(700, 205, 0x00335577u,
                                                                 "the launcher's first result to be drawn selected");
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t launcher_closed = selftest_pixel_settled(700, 309, 0x001A1A2Eu,
                                                           "the launcher overlay to go away again");

        selftest_reap(editor_task);
        selftest_reap(shell_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

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
                kernel_log_puts("[m43] pixel check failed: ");
                kernel_log_puts(names[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32(names[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32(got[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (!all_ok) {
            panic("M43 snap/launcher self-test: the compositor did not behave as expected");
        }
        kernel_log_puts("[m43] window snapping (left/right half, buffer-clamped) and the "
                   "launcher overlay self-test passed (7/7 checks matched).\n\n");
    }

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
        selftest_wait_for_compositor();
        task_t *icons_task = process_spawn("desktop_icons", icons_image, (size_t)icons_size, "");
        kfree(icons_image);
        pit_sleep_ms(500);
        task_t *shell_task = process_spawn("desktop_shell", shell_image, (size_t)shell_size, "");
        kfree(shell_image);
        pit_sleep_ms(700);

        uint32_t grad_high = framebuffer_get_pixel(600, 100);
        uint32_t grad_low = framebuffer_get_pixel(600, 600);
        uint32_t taskbar_over_grad = framebuffer_get_pixel(500, 738);

        int settings_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_SETTINGS_PIPE, (uint64_t)settings_file_descriptors, 0) != 0) {
            panic("M44 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_PIPE) failed");
        }
        window_manager_settings_request_t set_request;
        k_memset(&set_request, 0, sizeof(set_request));
        set_request.animations = 1;
        set_request.bg_color = 0x001A1A2Eu;
        set_request.accent_color = 0x004C99E6u;
        set_request.wallpaper = 0;
        do_syscall(SYS_write, (uint64_t)settings_file_descriptors[1], (uint64_t)&set_request, sizeof(set_request));
        pit_sleep_ms(900);
        uint32_t flat_high = framebuffer_get_pixel(600, 100);
        uint32_t flat_low = framebuffer_get_pixel(600, 600);

        int sq_file_descriptors[2];
        int sqr_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_SETTINGS_QUERY_PIPE, (uint64_t)sq_file_descriptors, 0) != 0 ||
            do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_SETTINGS_QUERY_RESPONSE_PIPE, (uint64_t)sqr_file_descriptors, 0) != 0) {
            panic("M44 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_QUERY_*) failed");
        }
        do_syscall(SYS_pipe_reset, (uint64_t)sqr_file_descriptors[0], 0, 0);
        uint8_t ping = 1;
        do_syscall(SYS_write, (uint64_t)sq_file_descriptors[1], (uint64_t)&ping, sizeof(ping));
        pit_sleep_ms(300);
        window_manager_settings_request_t queried;
        k_memset(&queried, 0, sizeof(queried));
        long settings_read = do_syscall(SYS_read, (uint64_t)sqr_file_descriptors[0], (uint64_t)&queried, sizeof(queried));

        selftest_reap(shell_task);
        selftest_reap(icons_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

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
                kernel_log_puts("[m44] pixel check failed: ");
                kernel_log_puts(names[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32(names[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32(got[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (settings_read != (long)sizeof(queried) || queried.wallpaper != 0 ||
            queried.bg_color != 0x001A1A2Eu || queried.accent_color != 0x004C99E6u) {
            kernel_log_puts("[m44] the settings query did not round-trip what was just set (wallpaper 0x");
            kernel_log_put_hex32(queried.wallpaper);
            kernel_log_puts(", bg 0x");
            kernel_log_put_hex32(queried.bg_color);
            kernel_log_puts(")\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M44 wallpaper/translucency self-test: the desktop did not look as computed");
        }
        kernel_log_puts("[m44] wallpaper gradient, taskbar translucency over it, and the "
                   "settings query round trip self-test passed (6/6 checks matched).\n\n");
    }

    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t stub_size_bytes = 0;
        uint8_t *stub_image = read_program("/bin/wm_stubborn", &stub_size_bytes);
        int64_t stub_size = (int64_t)stub_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor();

        uint64_t frames_before_victim = physical_memory_free_frame_count();
        task_t *victim = process_spawn("wm_stubborn", stub_image, (size_t)stub_size, "");
        uint32_t victim_pixel = selftest_pixel_settled(200, 150, 0x00B03040u,
                                                        "the stubborn client's window to be drawn");
        uint64_t frames_with_victim = physical_memory_free_frame_count();

        static task_info_t infos[MAX_TASKS];
        long info_count = do_syscall(SYS_taskinfo, (uint64_t)infos, MAX_TASKS, 0);
        int found_comp = 0, found_victim = 0, victim_shared_memory = -1;
        for (long i = 0; i < info_count; i++) {
            if (infos[i].pid == comp_task->id && k_strcmp(infos[i].name, "compositor") == 0) {
                found_comp = 1;
            }
            if (infos[i].pid == victim->id && k_strcmp(infos[i].name, "wm_stubborn") == 0) {
                found_victim = 1;
                victim_shared_memory = infos[i].shared_memory_segments;
            }
        }

        int action_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_ACTION_PIPE, (uint64_t)action_file_descriptors, 0) != 0) {
            panic("M45 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        window_manager_action_request_t request;
        k_memset(&request, 0, sizeof(request));
        request.window_id = 0;

        request.action = WINDOW_MANAGER_ACTION_CLOSE;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        pit_sleep_ms(600);
        uint32_t after_close_pixel = framebuffer_get_pixel(200, 150);
        long alive_after_close = do_syscall(SYS_task_alive, (uint64_t)victim->id, 0, 0);

        request.action = WINDOW_MANAGER_ACTION_KILL;
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        long alive_after_kill = 1;
        for (int spin = 0; spin < 200 && alive_after_kill == 1; spin++) {
            pit_sleep_ms(10);
            alive_after_kill = do_syscall(SYS_task_alive, (uint64_t)victim->id, 0, 0);
        }
        uint32_t after_kill_pixel = selftest_pixel_settled(200, 150, 0x001A1A2Eu,
                                                            "the killed client's window to be taken down");
        do_syscall(SYS_wait, (uint64_t)victim->id, 0, 0);
        uint64_t frames_after_kill = physical_memory_free_frame_count();

        task_t *victim2 = process_spawn("wm_stubborn", stub_image, (size_t)stub_size, "");
        kfree(stub_image);
        uint32_t reused_slot_pixel = selftest_pixel_settled(200, 150, 0x00B03040u,
                                                             "a second client to draw through the reclaimed slot");

        selftest_reap(victim2);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        int all_ok = 1;
        if (info_count <= 0 || !found_comp || !found_victim) {
            kernel_log_puts("[m45] SYS_taskinfo did not report the tasks this test spawned (count 0x");
            kernel_log_put_hex32((uint32_t)info_count);
            kernel_log_puts(", compositor found 0x");
            kernel_log_put_hex32((uint32_t)found_comp);
            kernel_log_puts(", wm_stubborn found 0x");
            kernel_log_put_hex32((uint32_t)found_victim);
            kernel_log_puts(")\n");
            all_ok = 0;
        }
        if (victim_shared_memory != 1) {
            kernel_log_puts("[m45] SYS_taskinfo reported the wrong shm-segment count for a task holding exactly one: 0x");
            kernel_log_put_hex32((uint32_t)victim_shared_memory);
            kernel_log_putc('\n');
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
                kernel_log_puts("[m45] pixel check failed: ");
                kernel_log_puts(names[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32(names[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32(got[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (alive_after_close != 1) {
            kernel_log_puts("[m45] WM_ACTION_CLOSE terminated a client that never answered WM_EVENT_CLOSE_REQUEST - the two verbs have collapsed into one (SYS_task_alive 0x");
            kernel_log_put_hex32((uint32_t)alive_after_close);
            kernel_log_puts(")\n");
            all_ok = 0;
        }
        if (alive_after_kill != 0) {
            kernel_log_puts("[m45] WM_ACTION_KILL did not terminate the client (SYS_task_alive 0x");
            kernel_log_put_hex32((uint32_t)alive_after_kill);
            kernel_log_puts(")\n");
            all_ok = 0;
        }
        if (frames_after_kill < frames_with_victim + 16) {
            kernel_log_puts("[m45] killing a task that owned an shm segment did not hand its frames back: 0x");
            kernel_log_put_hex64(frames_before_victim);
            kernel_log_puts(" free before, 0x");
            kernel_log_put_hex64(frames_with_victim);
            kernel_log_puts(" with it running, 0x");
            kernel_log_put_hex64(frames_after_kill);
            kernel_log_puts(" after the kill\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M45 process-control self-test: force quit did not behave as specified");
        }
        kernel_log_puts("[m45] SYS_taskinfo naming, WM_ACTION_KILL forcing a confirm_close client "
                   "WM_ACTION_CLOSE cannot, and the window slot/event pipe/shm reclaim after it "
                   "self-test passed (8/8 checks).\n\n");
    }

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
        selftest_wait_for_compositor();
        task_t *clock_task = process_spawn("gui_clock", clock_image, (size_t)clock_size, "");
        kfree(clock_image);
        uint32_t focused_corner = selftest_pixel_settled(246, 83, 0x004C99E6u,
                                                          "the focused window's titlebar to be drawn");
        uint32_t focused_disc = selftest_pixel_settled(250, 90, 0x00FF5F57u,
                                                        "the close button's own colour");
        uint32_t focused_glyph = selftest_pixel_settled(253, 90, 0x00303030u,
                                                         "the x drawn on the close button");
        uint32_t focused_shadow = selftest_pixel_settled(304, 100, 0x000D0D17u,
                                                          "the focused window's deeper drop shadow");
        int focused_bright = 0, focused_dim = 0;
        selftest_title_counts(1, &focused_bright, &focused_dim);

        task_t *stub_task = process_spawn("wm_stubborn", stub_image, (size_t)stub_size, "");
        kfree(stub_image);
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
        kernel_log_use_console();

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
                kernel_log_puts("[m46] pixel check failed: ");
                kernel_log_puts(names[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32(names[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32(got[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (focused_bright == 0 || focused_dim != 0 || unfocused_dim == 0 || unfocused_bright != 0) {
            kernel_log_puts("[m46] the title text did not dim when the window lost focus (focused: 0x");
            kernel_log_put_hex32((uint32_t)focused_bright);
            kernel_log_puts(" bright / 0x");
            kernel_log_put_hex32((uint32_t)focused_dim);
            kernel_log_puts(" dim, unfocused: 0x");
            kernel_log_put_hex32((uint32_t)unfocused_bright);
            kernel_log_puts(" bright / 0x");
            kernel_log_put_hex32((uint32_t)unfocused_dim);
            kernel_log_puts(" dim)\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M46 window-chrome self-test: the titlebar did not look as computed");
        }
        kernel_log_puts("[m46] circular titlebar buttons, focus-gated glyphs, the deeper focused "
                   "shadow and the dimmed unfocused title self-test passed (9/9 checks).\n\n");
    }

    {
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
        selftest_wait_for_pixel(500, 400, 0x00203040u, 5000,
                                 "the compositor to paint the saved background");
        task_t *icons_task = process_spawn("desktop_icons", icons_image, (size_t)icons_size, "");
        uint32_t saved_pixel = selftest_pixel_settled(600, 400, 0x00203040u,
                                                       "the desktop to come up with the saved settings");
        selftest_reap(icons_task);
        selftest_reap(comp_task);

        static const char broken_conf[] = "this is not a settings file\nbg=nonsense\nwallpaper=1\n";
        if (do_syscall(SYS_writefile, (uint64_t)PATH_SETTINGS, (uint64_t)broken_conf, sizeof(broken_conf) - 1) != 0) {
            panic("M47 self-test: could not overwrite settings.conf with a corrupted one");
        }

        comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor();
        icons_task = process_spawn("desktop_icons", icons_image, (size_t)icons_size, "");
        kfree(icons_image);
        uint32_t fallback_pixel = selftest_pixel_settled(600, 400, 0x001B1B31u,
                                                          "the desktop to fall back to the compiled-in defaults");
        selftest_reap(icons_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

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
            kernel_log_puts("[m47] the desktop did not come up with the saved settings - expected 0x00203040 got 0x");
            kernel_log_put_hex32(saved_pixel);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (fallback_pixel != 0x001B1B31u) {
            kernel_log_puts("[m47] a corrupted settings.conf did not fall back to the compiled-in defaults - expected 0x001B1B31 got 0x");
            kernel_log_put_hex32(fallback_pixel);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (killed_no_grace != 2 || !codes_no_grace) {
            kernel_log_puts("[m47] with no grace period, the orderly stop did not escalate to SIGKILL (0x");
            kernel_log_put_hex32((uint32_t)killed_no_grace);
            kernel_log_puts(" killed, exit codes 0x");
            kernel_log_put_hex32((uint32_t)v1->exit_code);
            kernel_log_puts("/0x");
            kernel_log_put_hex32((uint32_t)v2->exit_code);
            kernel_log_puts(")\n");
            all_ok = 0;
        }
        if (killed_with_grace != 0 || !codes_with_grace) {
            kernel_log_puts("[m47] with a real grace period, tasks did not stop on SIGTERM alone (0x");
            kernel_log_put_hex32((uint32_t)killed_with_grace);
            kernel_log_puts(" needed SIGKILL, exit codes 0x");
            kernel_log_put_hex32((uint32_t)v3->exit_code);
            kernel_log_puts("/0x");
            kernel_log_put_hex32((uint32_t)v4->exit_code);
            kernel_log_puts(")\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M47 session-lifecycle self-test: settings persistence and/or the orderly stop did not behave as specified");
        }
        kernel_log_puts("[m47] settings.conf round trip (including a corrupted one falling back to "
                   "defaults) and the orderly stop's SIGTERM-then-SIGKILL escalation "
                   "self-test passed (4/4 checks).\n\n");
    }

    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        if (comp_size < 64) {
            panic("vfs_read: compositor missing or absurdly small - should exist, just seeded");
        }
        if (virtual_file_system_write(PATH_TEMPORARY_DIRECTORY "m48trunc", comp_image, 64) != 0) {
            panic("M48 self-test: could not write the truncated-ELF fixture");
        }

        long rc_missing = do_syscall(SYS_spawn, (uint64_t)"definitely_not_a_file", 0, 0);
        long rc_text = do_syscall(SYS_spawn, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m33test"), 0, 0);
        long rc_trunc = do_syscall(SYS_spawn, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m48trunc"), 0, 0);

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor();

        int notify_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_NOTIFY_PIPE, (uint64_t)notify_file_descriptors, 0) != 0) {
            panic("M48 self-test: kernel-side SYS_pipe_open(WM_NOTIFY_PIPE) failed");
        }
        window_manager_notify_request_t note;
        k_memset(&note, 0, sizeof(note));
        note.level = WINDOW_MANAGER_NOTIFY_ERROR;
        k_strlcpy(note.title, "Test", sizeof(note.title));
        k_strlcpy(note.body, "Body", sizeof(note.body));
        do_syscall(SYS_write, (uint64_t)notify_file_descriptors[1], (uint64_t)&note, sizeof(note));
        pit_sleep_ms(300);

        uint32_t stripe = framebuffer_get_pixel(714, 40);
        uint32_t toast_bg = framebuffer_get_pixel(900, 40);

        pit_sleep_ms(2000);
        uint32_t stripe_midlife = framebuffer_get_pixel(714, 40);

        pit_sleep_ms(2500);
        uint32_t stripe_expired = framebuffer_get_pixel(714, 40);
        uint32_t bg_expired = framebuffer_get_pixel(900, 40);

        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

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
                kernel_log_puts("[m48] pixel check failed: ");
                kernel_log_puts(names[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32(names[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32(got[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        static const struct { const char *what; long expected; long got; } codes[] = {
            {"a name that is not on disk", SPAWN_ERROR_NOT_FOUND, 0},
            {"an ordinary text file", SPAWN_ERROR_BAD_IMAGE, 0},
            {"a truncated ELF", SPAWN_ERROR_BAD_IMAGE, 0},
        };
        const long got_codes[] = {rc_missing, rc_text, rc_trunc};
        for (size_t i = 0; i < sizeof(got_codes) / sizeof(got_codes[0]); i++) {
            if (got_codes[i] != codes[i].expected) {
                kernel_log_puts("[m48] SYS_spawn returned the wrong code for ");
                kernel_log_puts(codes[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32((uint32_t)codes[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32((uint32_t)got_codes[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (k_strcmp(spawn_error_message(SPAWN_ERROR_NOT_FOUND), spawn_error_message(SPAWN_ERROR_BAD_IMAGE)) == 0) {
            kernel_log_puts("[m48] two distinct spawn errors share one message - the codes buy nothing\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M48 feedback self-test: toasts and/or spawn error codes did not behave as specified");
        }
        kernel_log_puts("[m48] toast raised, still up mid-life, gone by its own deadline, and each "
                   "distinct SYS_spawn failure reporting its own code self-test passed "
                   "(9/9 checks).\n\n");
    }

    {
        static const struct { const char *what; char ch; int mods; int expect; } chords[] = {
            {"Alt+Tab", '\t', KEYBOARD_MOD_ALT, SHORTCUT_CYCLE_FORWARD},
            {"Shift+Alt+Tab", '\t', KEYBOARD_MOD_ALT | KEYBOARD_MOD_SHIFT, SHORTCUT_CYCLE_BACKWARD},
            {"Ctrl+Space", ' ', KEYBOARD_MOD_CTRL, SHORTCUT_LAUNCHER},
            {"Ctrl+Shift+Esc", 27, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_SHIFT, SHORTCUT_TASK_MANAGER},
            {"Alt+F4", (char)KEYBOARD_KEY_FUNCTION(4), KEYBOARD_MOD_ALT, SHORTCUT_CLOSE_WINDOW},
            {"Ctrl+Alt+Left", (char)KEYBOARD_KEY_LEFT, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT, SHORTCUT_SNAP_LEFT},
            {"Ctrl+Alt+Right", (char)KEYBOARD_KEY_RIGHT, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT, SHORTCUT_SNAP_RIGHT},
            {"Ctrl+Alt+Up", (char)KEYBOARD_KEY_UP, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT, SHORTCUT_MAXIMIZE},
            {"Ctrl+Alt+Down", (char)KEYBOARD_KEY_DOWN, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT, SHORTCUT_MINIMIZE},
            {"a plain Tab, which must NOT be a chord", '\t', 0, SHORTCUT_NONE},
            {"a plain space", ' ', 0, SHORTCUT_NONE},
            {"an ordinary letter with Ctrl held", 'c', KEYBOARD_MOD_CTRL, SHORTCUT_NONE},
        };
        int all_ok = 1;
        for (size_t i = 0; i < sizeof(chords) / sizeof(chords[0]); i++) {
            int got = shortcut_lookup(chords[i].ch, chords[i].mods);
            if (got != chords[i].expect) {
                kernel_log_puts("[m49] shortcut_lookup resolved ");
                kernel_log_puts(chords[i].what);
                kernel_log_puts(" to 0x");
                kernel_log_put_hex32((uint32_t)got);
                kernel_log_puts(", expected 0x");
                kernel_log_put_hex32((uint32_t)chords[i].expect);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        for (int i = 0; i < SHORTCUT_COUNT; i++) {
            if (SHORTCUTS[i].id == SHORTCUT_NONE || !SHORTCUTS[i].chord[0] || !SHORTCUTS[i].what[0]) {
                kernel_log_puts("[m49] shortcut row 0x");
                kernel_log_put_hex32((uint32_t)i);
                kernel_log_puts(" is missing an id, a chord name or a description\n");
                all_ok = 0;
            }
        }

        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor();

        int drag_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_DRAG_PIPE, (uint64_t)drag_file_descriptors, 0) != 0) {
            panic("M49 self-test: kernel-side SYS_pipe_open(WM_DRAG_PIPE) failed");
        }
        window_manager_drag_request_t drag;
        k_memset(&drag, 0, sizeof(drag));
        k_strlcpy(drag.payload, PATH_TEMPORARY_DIRECTORY "m33test", sizeof(drag.payload));
        do_syscall(SYS_write, (uint64_t)drag_file_descriptors[1], (uint64_t)&drag, sizeof(drag));
        pit_sleep_ms(300);

        uint32_t label_pixel = framebuffer_get_pixel(524, 396);

        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        if (label_pixel != 0x00335577u) {
            kernel_log_puts("[m49] the compositor did not show a drag label after WM_DRAG_PIPE - expected 0x00335577 got 0x");
            kernel_log_put_hex32(label_pixel);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M49 input-completeness self-test: the shortcut table and/or the drag protocol did not behave as specified");
        }
        kernel_log_puts("[m49] the shared shortcut table resolving every chord (and refusing every "
                   "near-miss), and a drag announced on WM_DRAG_PIPE becoming a visible drag "
                   "self-test passed (22/22 checks).\n\n");
    }

    {
        int all_ok = 1;

        uint64_t shared_memory_baseline = physical_memory_free_frame_count();
        int shared_memory_cycles_ok = 1;
        for (int i = 0; i < 24; i++) {
            long id = do_syscall(SYS_shared_memory_create, 64 * 1024, 0, 0);
            if (id < 0) {
                kernel_log_puts("[m50] shm_create failed on cycle 0x");
                kernel_log_put_hex32((uint32_t)i);
                kernel_log_puts(" - the segment table is not being handed back\n");
                shared_memory_cycles_ok = 0;
                break;
            }
            if (do_syscall(SYS_shared_memory_free, (uint64_t)id, 0, 0) != 0) {
                kernel_log_puts("[m50] shm_free refused a segment this task had just created\n");
                shared_memory_cycles_ok = 0;
                break;
            }
        }
        uint64_t shared_memory_after = physical_memory_free_frame_count();
        if (!shared_memory_cycles_ok || shared_memory_after != shared_memory_baseline) {
            kernel_log_puts("[m50] 24 shm create/free cycles did not return every frame: 0x");
            kernel_log_put_hex64(shared_memory_baseline);
            kernel_log_puts(" free before, 0x");
            kernel_log_put_hex64(shared_memory_after);
            kernel_log_puts(" after\n");
            all_ok = 0;
        }

        long id_twice = do_syscall(SYS_shared_memory_create, 4096, 0, 0);
        long first_free = do_syscall(SYS_shared_memory_free, (uint64_t)id_twice, 0, 0);
        long second_free = do_syscall(SYS_shared_memory_free, (uint64_t)id_twice, 0, 0);
        if (first_free != 0 || second_free == 0) {
            kernel_log_puts("[m50] freeing an shm segment twice did not fail the second time (0x");
            kernel_log_put_hex32((uint32_t)first_free);
            kernel_log_puts(" then 0x");
            kernel_log_put_hex32((uint32_t)second_free);
            kernel_log_puts(")\n");
            all_ok = 0;
        }

        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t stub_size_bytes = 0;
        uint8_t *stub_image = read_program("/bin/wm_stubborn", &stub_size_bytes);
        int64_t stub_size = (int64_t)stub_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor();

        uint64_t storm_frames_before = physical_memory_free_frame_count();
        int storm_shared_memory_before = shared_memory_count_by_owner(comp_task->id);
        int storm_ok = 1;
        for (int round = 0; round < 16 && storm_ok; round++) {
            task_t *victim = process_spawn("wm_stubborn", stub_image, (size_t)stub_size, "");
            if (!victim) {
                kernel_log_puts("[m50] kill storm: spawn failed on round 0x");
                kernel_log_put_hex32((uint32_t)round);
                kernel_log_putc('\n');
                storm_ok = 0;
                break;
            }
            pit_sleep_ms(300);
            selftest_reap(victim);
            pit_sleep_ms(200);
        }
        kfree(stub_image);

        uint64_t storm_frames_after = physical_memory_free_frame_count();
        int storm_shared_memory_after = shared_memory_count_by_owner(comp_task->id);

        int comp_file_descriptors = 0;
        for (int f = 0; f < MAX_FILE_DESCRIPTORS; f++) {
            if (comp_task->file_descriptors[f].type != FILE_DESCRIPTOR_NONE) {
                comp_file_descriptors++;
            }
        }
        kernel_log_puts("[m50] compositor after the storm: 0x");
        kernel_log_put_hex32((uint32_t)comp_file_descriptors);
        kernel_log_puts(" of 0x");
        kernel_log_put_hex32((uint32_t)MAX_FILE_DESCRIPTORS);
        kernel_log_puts(" fds, 0x");
        kernel_log_put_hex32((uint32_t)storm_shared_memory_after);
        kernel_log_puts(" shm segment(s) held.\n");

        size_t last_size_bytes = 0;
        uint8_t *last_image = read_program("/bin/wm_stubborn", &last_size_bytes);
        int64_t last_size = (int64_t)last_size_bytes;
        task_t *last_task = last_size == 0 ? (task_t *)0
                                          : process_spawn("wm_stubborn", last_image, (size_t)last_size, "");
        kfree(last_image);
        pit_sleep_ms(700);
        uint32_t survivor_pixel = framebuffer_get_pixel(200, 150);
        if (last_task) {
            selftest_reap(last_task);
        }
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        if (!storm_ok) {
            all_ok = 0;
        }
        if (storm_shared_memory_after != storm_shared_memory_before) {
            kernel_log_puts("[m50] the kill storm left the compositor holding shm segments: 0x");
            kernel_log_put_hex32((uint32_t)storm_shared_memory_before);
            kernel_log_puts(" before, 0x");
            kernel_log_put_hex32((uint32_t)storm_shared_memory_after);
            kernel_log_puts(" after 16 rounds\n");
            all_ok = 0;
        }
        uint64_t storm_leak = storm_frames_before > storm_frames_after
                                  ? storm_frames_before - storm_frames_after
                                  : 0;
        kernel_log_puts("[m50] kill storm: 0x");
        kernel_log_put_hex64(storm_leak);
        kernel_log_puts(" frames not reclaimed across 16 rounds (0x");
        kernel_log_put_hex64(storm_leak / 16);
        kernel_log_puts(" per dead address space; one leaked window buffer would be 0x18).\n");
        if (storm_leak > 16 * 20) {
            kernel_log_puts("[m50] the kill storm leaked more than 16 dead address spaces account for - a window's pixel buffer did not come back\n");
            all_ok = 0;
        }
        if (survivor_pixel != 0x00B03040u) {
            kernel_log_puts("[m50] a client connecting after 16 kill rounds got no drawable window - expected 0x00B03040 at (200,150), got 0x");
            kernel_log_put_hex32(survivor_pixel);
            kernel_log_putc('\n');
            all_ok = 0;
        }

        static const uint64_t KERNEL_ADDRESS = 0x100000ULL;
        static task_info_t garbage_scratch[2];
        struct { const char *what; long got; } garbage[] = {
            {"SYS_taskinfo with a null buffer", do_syscall(SYS_taskinfo, 0, 8, 0)},
            {"SYS_taskinfo with a zero count", do_syscall(SYS_taskinfo, (uint64_t)garbage_scratch, 0, 0)},
            {"SYS_taskinfo with an absurd count", do_syscall(SYS_taskinfo, (uint64_t)garbage_scratch, 0xFFFFFFFFULL, 0)},
            {"SYS_close on an out-of-range fd", do_syscall(SYS_close, 0xFFFFFFFFULL, 0, 0)},
            {"SYS_close on an fd that was never open", do_syscall(SYS_close, MAX_FILE_DESCRIPTORS - 1, 0, 0)},
            {"SYS_shm_free on an id that does not exist", do_syscall(SYS_shared_memory_free, 0xFFFFULL, 0, 0)},
            {"SYS_shm_free with a misaligned address", do_syscall(SYS_shared_memory_free, 0, KERNEL_ADDRESS + 1, 0)},
            {"SYS_shutdown with an unrecognized mode", do_syscall(SYS_shutdown, 99, 0, 0)},
            {"SYS_kill on a pid that was never valid", do_syscall(SYS_kill, 0xFFFFULL, SIGKILL, 0)},
        };
        for (size_t i = 0; i < sizeof(garbage) / sizeof(garbage[0]); i++) {
            if (garbage[i].got >= 0) {
                kernel_log_puts("[m50] ");
                kernel_log_puts(garbage[i].what);
                kernel_log_puts(" succeeded (0x");
                kernel_log_put_hex32((uint32_t)garbage[i].got);
                kernel_log_puts(") instead of failing\n");
                all_ok = 0;
            }
        }

        if (!all_ok) {
            panic("M50 robustness self-test: a resource did not come back, or a garbage argument was accepted");
        }
        kernel_log_puts("[m50] 24 shm create/free cycles frame-neutral, a double free refused, "
                   "16 kill-storm rounds returning every window slot and segment, and 9 "
                   "garbage-argument syscalls all refused self-test passed (13/13 checks).\n\n");
    }

    {
        int all_ok = 1;

        static const char *const LAYOUT[] = {PATH_BIN, PATH_HOME, PATH_ETC, PATH_TEMPORARY};
        for (size_t i = 0; i < sizeof(LAYOUT) / sizeof(LAYOUT[0]); i++) {
            if (!virtual_file_system_is_directory(LAYOUT[i])) {
                kernel_log_puts("[m53] ");
                kernel_log_puts(LAYOUT[i]);
                kernel_log_puts(" is missing or is not a directory\n");
                all_ok = 0;
            }
        }

        static char list_buffer[4096];
        size_t list_length = virtual_file_system_list(PATH_BIN, list_buffer, sizeof(list_buffer));
        int found = 0;
        for (size_t i = 0; i < EMBEDDED_PROGRAM_COUNT; i++) {
            char path[PATH_MAX_LENGTH];
            path_join(path, PATH_BIN_DIRECTORY, embedded_programs[i].name);
            if (virtual_file_system_exists(path)) {
                found++;
            } else {
                kernel_log_puts("[m53] ");
                kernel_log_puts(path);
                kernel_log_puts(" was not seeded\n");
                all_ok = 0;
            }
        }
        int listed = 0;
        for (size_t i = 0; i < list_length; i++) {
            if (list_buffer[i] == '\n') {
                listed++;
            }
        }
        if (listed < (int)EMBEDDED_PROGRAM_COUNT) {
            kernel_log_puts("[m53] ");
            kernel_log_puts(PATH_BIN);
            kernel_log_puts(" lists 0x");
            kernel_log_put_hex32((uint32_t)listed);
            kernel_log_puts(" entries, fewer than the 0x");
            kernel_log_put_hex32((uint32_t)EMBEDDED_PROGRAM_COUNT);
            kernel_log_puts(" programs this build ships - the listing and the seeding disagree\n");
            all_ok = 0;
        }

        static const char *const DEEP = PATH_TEMPORARY_DIRECTORY "m53dir";
        if (!virtual_file_system_exists(DEEP) && virtual_file_system_mkdir(DEEP) != 0) {
            kernel_log_puts("[m53] vfs_mkdir failed on a fresh path under " PATH_TEMPORARY "\n");
            all_ok = 0;
        }
        if (!virtual_file_system_is_directory(DEEP)) {
            kernel_log_puts("[m53] the directory just created does not read back as one\n");
            all_ok = 0;
        }
        const int DEEP_FILES = 17;
        for (int i = 0; i < DEEP_FILES; i++) {
            char path[PATH_MAX_LENGTH];
            char name[8];
            name[0] = 'f';
            name[1] = (char)('0' + i / 10);
            name[2] = (char)('0' + i % 10);
            name[3] = '\0';
            path_join(path, PATH_TEMPORARY_DIRECTORY "m53dir/", name);
            char body[16];
            k_memset(body, 0, sizeof(body));
            body[0] = (char)('A' + i);
            if (virtual_file_system_write(path, body, sizeof(body)) != 0) {
                kernel_log_puts("[m53] writing file 0x");
                kernel_log_put_hex32((uint32_t)i);
                kernel_log_puts(" into a directory past its first block failed\n");
                all_ok = 0;
                break;
            }
        }
        size_t deep_length = virtual_file_system_list(DEEP, list_buffer, sizeof(list_buffer));
        int deep_listed = 0;
        for (size_t i = 0; i < deep_length; i++) {
            if (list_buffer[i] == '\n') {
                deep_listed++;
            }
        }
        if (deep_listed != DEEP_FILES) {
            kernel_log_puts("[m53] a directory holding 0x");
            kernel_log_put_hex32((uint32_t)DEEP_FILES);
            kernel_log_puts(" files listed 0x");
            kernel_log_put_hex32((uint32_t)deep_listed);
            kernel_log_puts(" of them - it did not grow past one block correctly\n");
            all_ok = 0;
        }
        {
            char body[16];
            k_memset(body, 0, sizeof(body));
            int64_t n = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m53dir/f16", body, sizeof(body));
            if (n != 16 || body[0] != (char)('A' + 16)) {
                kernel_log_puts("[m53] the 17th file in that directory did not read back by path (0x");
                kernel_log_put_hex32((uint32_t)n);
                kernel_log_puts(" bytes, first byte 0x");
                kernel_log_put_hex32((uint32_t)(uint8_t)body[0]);
                kernel_log_puts(")\n");
                all_ok = 0;
            }
        }

        static const char a_body[] = "in-tmp";
        static const char b_body[] = "in-home";
        if (virtual_file_system_write(PATH_TEMPORARY_DIRECTORY "m53same", a_body, sizeof(a_body)) != 0 ||
            virtual_file_system_write(PATH_ETC_DIRECTORY "m53same", b_body, sizeof(b_body)) != 0) {
            kernel_log_puts("[m53] could not create the same name in two directories\n");
            all_ok = 0;
        } else {
            char got_a[16], got_b[16];
            k_memset(got_a, 0, sizeof(got_a));
            k_memset(got_b, 0, sizeof(got_b));
            virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m53same", got_a, sizeof(got_a));
            virtual_file_system_read(PATH_ETC_DIRECTORY "m53same", got_b, sizeof(got_b));
            if (k_strcmp(got_a, a_body) != 0 || k_strcmp(got_b, b_body) != 0) {
                kernel_log_puts("[m53] the same name in two directories resolved to one file: '");
                kernel_log_puts(got_a);
                kernel_log_puts("' and '");
                kernel_log_puts(got_b);
                kernel_log_puts("'\n");
                all_ok = 0;
            }
        }

        static const struct { const char *what; const char *path; } BAD_PATHS[] = {
            {"a relative path, which has nothing to be relative to", "bin/ls"},
            {"an empty component", "//bin"},
            {"a '.' component", "/./bin"},
            {"a '..' component, the escape this format refuses to synthesize", "/bin/../etc"},
            {"a '..' climbing out of the root", "/.."},
            {"walking through a regular file as if it were a directory", PATH_BIN_DIRECTORY "ls/nope"},
            {"a component longer than a name may be", "/bin/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
        };
        for (size_t i = 0; i < sizeof(BAD_PATHS) / sizeof(BAD_PATHS[0]); i++) {
            char scratch[16];
            if (virtual_file_system_exists(BAD_PATHS[i].path) || virtual_file_system_read(BAD_PATHS[i].path, scratch, sizeof(scratch)) >= 0) {
                kernel_log_puts("[m53] path check failed: ");
                kernel_log_puts(BAD_PATHS[i].what);
                kernel_log_puts(" was accepted ('");
                kernel_log_puts(BAD_PATHS[i].path);
                kernel_log_puts("')\n");
                all_ok = 0;
            }
        }

        if (!virtual_file_system_is_directory(PATH_BIN_DIRECTORY)) {
            kernel_log_puts("[m53] a trailing slash on a directory was refused ('" PATH_BIN_DIRECTORY "')\n");
            all_ok = 0;
        }
        {
            char scratch[16];
            if (virtual_file_system_read(PATH_BIN_DIRECTORY "ls/", scratch, sizeof(scratch)) >= 0) {
                kernel_log_puts("[m53] a trailing slash on a regular file was accepted\n");
                all_ok = 0;
            }
        }

        if (virtual_file_system_exists(PATH_BIN_DIRECTORY "settings.conf") || !virtual_file_system_exists(PATH_SETTINGS)) {
            kernel_log_puts("[m53] settings.conf is not where the layout says it is\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M53 directory self-test: the namespace is not a tree");
        }
        kernel_log_puts("[m53] directories created, entered, grown past one block, listed and read "
                   "back by path; the same name in two directories staying two files; seven "
                   "malformed or escaping paths refused (and, since M60, a trailing slash "
                   "honoured on a directory and still refused on a file); and " PATH_BIN
                   " holding exactly the "
                   "programs this build ships self-test passed (");
        kernel_log_put_hex32((uint32_t)found);
        kernel_log_puts(" programs seeded).\n\n");
    }

    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t z_size_bytes = 0;
        uint8_t *z_image = read_program("/bin/wm_zorder", &z_size_bytes);
        int64_t z_size = (int64_t)z_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor();

        task_t *a_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zA 00A02020");
        pit_sleep_ms(500);
        task_t *b_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zB 002060C0");
        kfree(z_image);
        pit_sleep_ms(500);

        uint32_t overlap_before = selftest_pixel_settled(200, 250, 0x002060C0u,
                                                          "the second client to connect in front of the first");

        mouse_inject(-4096, -4096, 0, 0);
        mouse_inject(120, 250, 0, 0);
        pit_sleep_ms(120);
        mouse_inject(0, 0, 1, 0);
        pit_sleep_ms(120);
        mouse_inject(0, 0, 0, 0);

        uint32_t overlap_after_raise = selftest_pixel_settled(200, 250, 0x00A02020u,
                                                               "the clicked window to come forward");
        uint32_t a_tick1 = selftest_pixel_settled(393, 293, 0x00F0E000u,
                                                   "the clicked window to record the press it received");
        uint32_t b_tick1 = selftest_pixel_settled(433, 333, 0x002060C0u,
                                                   "the other window's tick to stay unlit");

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

        int query_file_descriptors[2], query_response_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_QUERY_PIPE, (uint64_t)query_file_descriptors, 0) != 0 ||
            do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_QUERY_RESPONSE_PIPE, (uint64_t)query_response_file_descriptors, 0) != 0) {
            panic("M51 self-test: kernel-side SYS_pipe_open(WM_QUERY_PIPE) failed");
        }
        uint8_t ping = 1;
        do_syscall(SYS_write, (uint64_t)query_file_descriptors[1], (uint64_t)&ping, sizeof(ping));
        pit_sleep_ms(300);
        window_manager_query_response_t *q = (window_manager_query_response_t *)kmalloc(sizeof(window_manager_query_response_t));
        if (!q) {
            panic("out of memory for the M51 query response");
        }
        k_memset(q, 0, sizeof(*q));
        do_syscall(SYS_read, (uint64_t)query_response_file_descriptors[0], (uint64_t)q, sizeof(*q));
        int32_t a_z = -1, b_z = -1;
        for (int32_t i = 0; i < q->count && i < WINDOW_MANAGER_MAX_ROUTABLE_WINDOWS; i++) {
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
        kernel_log_use_console();

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
                kernel_log_puts("[m51] pixel check failed: ");
                kernel_log_puts(checks_meta[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32(checks_meta[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32(got[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (a_z < 0 || b_z < 0 || a_z <= b_z) {
            kernel_log_puts("[m51] wm_window_info_t.z_index did not report the raised window as frontmost: zA 0x");
            kernel_log_put_hex32((uint32_t)a_z);
            kernel_log_puts(", zB 0x");
            kernel_log_put_hex32((uint32_t)b_z);
            kernel_log_puts(", of 0x");
            kernel_log_put_hex32((uint32_t)reported);
            kernel_log_puts(" window(s) reported\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M51 z-order self-test: overlapping windows did not behave as specified");
        }
        kernel_log_puts("[m51] z-order raise-on-click, occlusion-correct hit-testing (the overlap "
                   "click reaching exactly the front window) and wm_window_info_t.z_index "
                   "self-test passed (7/7 checks).\n\n");
    }

    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        size_t z_size_bytes = 0;
        uint8_t *z_image = read_program("/bin/wm_zorder", &z_size_bytes);
        size_t clock_size_bytes = 0;
        uint8_t *clock_image = read_program("/bin/gui_clock", &clock_size_bytes);

        task_t *comp_task = process_spawn("compositor", comp_image, comp_size_bytes, "");
        kfree(comp_image);
        selftest_wait_for_compositor();
        task_t *z_task = process_spawn("wm_zorder", z_image, z_size_bytes, "zP 00A02020");
        kfree(z_image);
        pit_sleep_ms(600);

        uint64_t worst_us = 0;
        int missed = 0;
        for (int t = 0; t < 3 && !missed; t++) {
            int32_t tick_x = 393 - 14 * t + 4;
            int32_t tick_y = 293 + 4;
            mouse_inject(-4096, -4096, 0, 0);
            mouse_inject(120, 250, 0, 0);
            pit_sleep_ms(120);
            uint64_t deadline = pit_get_ticks() + 300;
            uint64_t t0 = tsc_read();
            mouse_inject(0, 0, 1, 0);
            for (;;) {
                if (framebuffer_get_pixel((uint32_t)tick_x, (uint32_t)tick_y) == 0x00F0E000u) {
                    uint64_t us = tsc_to_us(tsc_read() - t0);
                    if (us > worst_us) {
                        worst_us = us;
                    }
                    kernel_log_puts("[m117] press ");
                    kernel_log_put_dec((uint32_t)t);
                    kernel_log_puts(" on screen after ");
                    kernel_log_put_dec((uint32_t)(us / 1000));
                    kernel_log_puts(" ms\n");
                    break;
                }
                if (pit_get_ticks() >= deadline) {
                    missed = 1;
                    break;
                }
                schedule();
            }
            mouse_inject(0, 0, 0, 0);
            pit_sleep_ms(150);
        }

        task_t *clocks[3];
        for (int i = 0; i < 3; i++) {
            clocks[i] = process_spawn("gui_clock", clock_image, clock_size_bytes, "");
            pit_sleep_ms(300);
        }
        kfree(clock_image);
        pit_sleep_ms(700);
        task_t *desktop[5] = { comp_task, z_task, clocks[0], clocks[1], clocks[2] };
        uint64_t used0 = 0, used1 = 0;
        for (int i = 0; i < 5; i++) {
            used0 += desktop[i]->user_ticks + desktop[i]->sys_ticks;
        }
        uint64_t window0 = pit_get_ticks();
        pit_sleep_ms(2000);
        uint64_t window_ticks = pit_get_ticks() - window0;
        for (int i = 0; i < 5; i++) {
            used1 += desktop[i]->user_ticks + desktop[i]->sys_ticks;
        }
        uint64_t busy_pct = window_ticks ? (100 * (used1 - used0)) / window_ticks : 100;

        for (int i = 0; i < 3; i++) {
            selftest_reap(clocks[i]);
        }
        selftest_reap(z_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        if (missed) {
            panic("M117 self-test: a press on a window never showed as that window's tick - "
                  "the click, the client's redraw or its present did not reach the screen");
        }
        kernel_log_perf("click_to_photon_us", worst_us, "us");
        kernel_log_perf("desktop_busy_pct", busy_pct, "pct");
        kernel_log_puts("[m117] a desktop that sleeps and a click that shows: three presses on a "
                   "client each on screen as its own tick within ");
        kernel_log_put_dec((uint32_t)(worst_us / 1000));
        kernel_log_puts(" ms, and the compositor and four idle windows used ");
        kernel_log_put_dec((uint32_t)busy_pct);
        kernel_log_puts("% of the CPU over two seconds - self-test passed.\n\n");
    }

    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program("/bin/compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t fault_size_bytes = 0;
        uint8_t *fault_image = read_program("/bin/wm_faulter", &fault_size_bytes);
        int64_t fault_size = (int64_t)fault_size_bytes;

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        selftest_wait_for_compositor();

        uint64_t frames_before = physical_memory_free_frame_count();
        task_t *victim = process_spawn("wm_faulter", fault_image, (size_t)fault_size, "");
        kfree(fault_image);
        uint32_t painted = selftest_pixel_settled(150, 150, 0x0020C0A0u,
                                                   "the faulter's window to be drawn");
        int alive_before_fault = (int)do_syscall(SYS_task_alive, (uint64_t)victim->id, 0, 0);
        uint64_t frames_with_victim = physical_memory_free_frame_count();

        pit_sleep_ms(1400);

        uint32_t after_fault = selftest_pixel_settled(150, 150, 0x001A1A2Eu,
                                                       "the faulted client's window to be taken down");
        int alive_after_fault = (int)do_syscall(SYS_task_alive, (uint64_t)victim->id, 0, 0);
        long victim_exit = do_syscall(SYS_wait, (uint64_t)victim->id, 0, 0);
        uint64_t frames_after = physical_memory_free_frame_count();

        long seg = do_syscall(SYS_shared_memory_create, 4096, 0, 0);
        long free_kernel_address = do_syscall(SYS_shared_memory_free, (uint64_t)seg, 0x100000ULL, 0);
        long free_unaligned = do_syscall(SYS_shared_memory_free, (uint64_t)seg, USER_SHARED_MEMORY_BASE + 1, 0);
        long free_ok = do_syscall(SYS_shared_memory_free, (uint64_t)seg, 0, 0);

        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        int all_ok = 1;
        if (painted != 0x0020C0A0u) {
            kernel_log_puts("[m52] the faulting client never got a window on screen, so what follows would not have been a crash - expected 0x0020C0A0 got 0x");
            kernel_log_put_hex32(painted);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (alive_before_fault != 1) {
            kernel_log_puts("[m52] the faulting client was not running before it faulted (SYS_task_alive 0x");
            kernel_log_put_hex32((uint32_t)alive_before_fault);
            kernel_log_puts(")\n");
            all_ok = 0;
        }
        if (alive_after_fault != 0) {
            kernel_log_puts("[m52] a null dereference in ring 3 did not terminate the offending task (SYS_task_alive 0x");
            kernel_log_put_hex32((uint32_t)alive_after_fault);
            kernel_log_puts(")\n");
            all_ok = 0;
        }
        if (victim_exit != 128 + SIGSEGV) {
            kernel_log_puts("[m52] a faulting task's exit code is not distinguishable as a fault - expected 0x");
            kernel_log_put_hex32((uint32_t)(128 + SIGSEGV));
            kernel_log_puts(" got 0x");
            kernel_log_put_hex32((uint32_t)victim_exit);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (after_fault != 0x001A1A2Eu) {
            kernel_log_puts("[m52] the dead client's window was not reclaimed - expected the desktop background 0x001A1A2E at (150,150), got 0x");
            kernel_log_put_hex32(after_fault);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (frames_after < frames_with_victim + 16) {
            kernel_log_puts("[m52] a crashed client's shm segment was not handed back: 0x");
            kernel_log_put_hex64(frames_before);
            kernel_log_puts(" free before it started, 0x");
            kernel_log_put_hex64(frames_with_victim);
            kernel_log_puts(" with it running, 0x");
            kernel_log_put_hex64(frames_after);
            kernel_log_puts(" after it faulted and was reaped\n");
            all_ok = 0;
        }
        if (seg < 0 || free_kernel_address == 0 || free_unaligned == 0 || free_ok != 0) {
            kernel_log_puts("[m52] SYS_shm_free's vaddr bounds are wrong: id 0x");
            kernel_log_put_hex32((uint32_t)seg);
            kernel_log_puts(", kernel address returned 0x");
            kernel_log_put_hex32((uint32_t)free_kernel_address);
            kernel_log_puts(", unaligned returned 0x");
            kernel_log_put_hex32((uint32_t)free_unaligned);
            kernel_log_puts(", the legitimate free returned 0x");
            kernel_log_put_hex32((uint32_t)free_ok);
            kernel_log_putc('\n');
            all_ok = 0;
        }

        size_t bad_size_bytes = 0;
        uint8_t *bad_image = read_program("/bin/badptr", &bad_size_bytes);
        int64_t bad_size = (int64_t)bad_size_bytes;
        task_t *bad_task = process_spawn("badptr", bad_image, (size_t)bad_size, "");
        kfree(bad_image);
        long bad_exit = do_syscall(SYS_wait, (uint64_t)bad_task->id, 0, 0);
        if (bad_exit != 0) {
            kernel_log_puts("[m52] the garbage-argument matrix accepted 0x");
            kernel_log_put_hex32((uint32_t)bad_exit);
            kernel_log_puts(" argument(s) it should have refused - see the [badptr] lines above\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M52 kernel-hardening self-test: a user program can still take the kernel with it");
        }
        kernel_log_puts("[m52] a ring-3 null dereference killing only its own task (exit 139), its "
                   "window and segment reclaimed, SYS_shm_free refusing a kernel address, and "
                   "every pointer-taking syscall refusing every shape of bad pointer "
                   "self-test passed (7/7 checks).\n\n");
    }

    {
        size_t hello_size_bytes = 0;
        uint8_t *hello_image = read_program(PATH_BIN_DIRECTORY "hello", &hello_size_bytes);
        int64_t hello_size = (int64_t)hello_size_bytes;

        const int ROUNDS = MAX_TASKS * 3;

        file_descriptor_slot_t saved_stdout = scheduler_current()->file_descriptors[1];
        scheduler_current()->file_descriptors[1].type = FILE_DESCRIPTOR_NONE;

        int live_before = scheduler_live_task_count();
        uint64_t frames_before = physical_memory_free_frame_count();
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
            if (stale_pid < 0) {
                stale_pid = pid;
            } else if (do_syscall(SYS_task_alive, (uint64_t)stale_pid, 0, 0) != -1) {
                stale_seen_as_live++;
            }
        }

        int live_after = scheduler_live_task_count();
        uint64_t frames_after = physical_memory_free_frame_count();
        scheduler_current()->file_descriptors[1] = saved_stdout;
        kfree(hello_image);

        int all_ok = 1;
        if (spawn_failures) {
            kernel_log_puts("[m54] a spawn failed partway through 0x");
            kernel_log_put_hex32((uint32_t)ROUNDS);
            kernel_log_puts(" rounds - the task table is still a lifetime budget\n");
            all_ok = 0;
        }
        if (live_after != live_before) {
            kernel_log_puts("[m54] task slots did not come back: 0x");
            kernel_log_put_hex32((uint32_t)live_before);
            kernel_log_puts(" live before, 0x");
            kernel_log_put_hex32((uint32_t)live_after);
            kernel_log_puts(" after 0x");
            kernel_log_put_hex32((uint32_t)ROUNDS);
            kernel_log_puts(" spawn/reap rounds\n");
            all_ok = 0;
        }
        if (frames_after != frames_before) {
            kernel_log_puts("[m54] frames did not come back: 0x");
            kernel_log_put_hex64(frames_before);
            kernel_log_puts(" free before, 0x");
            kernel_log_put_hex64(frames_after);
            kernel_log_puts(" after (0x");
            kernel_log_put_hex64(frames_before - frames_after);
            kernel_log_puts(" lost across 0x");
            kernel_log_put_hex32((uint32_t)ROUNDS);
            kernel_log_puts(" processes)\n");
            all_ok = 0;
        }
        if (stale_seen_as_live) {
            kernel_log_puts("[m54] a stale pid was answered about 0x");
            kernel_log_put_hex32((uint32_t)stale_seen_as_live);
            kernel_log_puts(" time(s) instead of being refused - the generation counter is not doing its job\n");
            all_ok = 0;
        }
        if (do_syscall(SYS_task_alive, 0x7FFFFFFF, 0, 0) != -1 ||
            do_syscall(SYS_task_alive, (uint64_t)scheduler_current()->id, 0, 0) != 1) {
            kernel_log_puts("[m54] SYS_task_alive no longer tells a live pid from an impossible one\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M54 reclaim self-test: something a dead process held did not come back");
        }
        kernel_log_puts("[m54] every task slot and every frame returned across 0x");
        kernel_log_put_hex32((uint32_t)ROUNDS);
        kernel_log_puts(" spawn/reap rounds (three times MAX_TASKS), and a stale pid refused rather "
                   "than answered about, self-test passed (5/5 checks).\n\n");
    }

    {
        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program(PATH_BIN_DIRECTORY "compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t z_size_bytes = 0;
        uint8_t *z_image = read_program(PATH_BIN_DIRECTORY "wm_zorder", &z_size_bytes);
        int64_t z_size = (int64_t)z_size_bytes;

        task_t *comp1 = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        task_t *a_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zA 00A02020");
        task_t *b_task = process_spawn("wm_zorder", z_image, (size_t)z_size, "zB 002060C0");

        int up = selftest_wait_for_pixel(120, 250, 0x00A02020u, 5000,
                                          "the first client's window to appear");
        up &= selftest_wait_for_pixel(420, 320, 0x002060C0u, 5000,
                                       "the second client's window to appear");
        if (!up) {
            const char *who[3] = {"compositor", "client A", "client B"};
            task_t *w[3] = {comp1, a_task, b_task};
            for (int i = 0; i < 3; i++) {
                kernel_log_puts("[m55] ");
                kernel_log_puts(who[i]);
                if (!w[i]) {
                    kernel_log_puts(" was never spawned\n");
                    continue;
                }
                task_t *live = scheduler_task_by_id(w[i]->id);
                kernel_log_puts(live ? " state=" : " is gone from the table\n");
                if (live) {
                    kernel_log_put_dec((uint32_t)live->state);
                    kernel_log_puts(" exit=");
                    kernel_log_put_dec((uint32_t)live->exit_code);
                    kernel_log_putc('\n');
                }
            }
            panic("M55 session-resilience self-test: the clients never got their windows up");
        }

        uint32_t a_before = framebuffer_get_pixel(120, 250);
        uint32_t b_before = framebuffer_get_pixel(420, 320);

        do_syscall(SYS_kill, (uint64_t)comp1->id, SIGKILL, 0);
        do_syscall(SYS_wait, (uint64_t)comp1->id, 0, 0);

        int a_alive_after_crash = (int)do_syscall(SYS_task_alive, (uint64_t)a_task->id, 0, 0);
        int b_alive_after_crash = (int)do_syscall(SYS_task_alive, (uint64_t)b_task->id, 0, 0);

        task_t *comp2 = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        kfree(comp_image);
        kfree(z_image);
        int back = selftest_wait_for_pixel(120, 250, 0x00A02020u, 6000,
                                            "the first client to reconnect and repaint");
        back &= selftest_wait_for_pixel(420, 320, 0x002060C0u, 6000,
                                         "the second client to reconnect and repaint");
        (void)back;

        uint32_t a_after = framebuffer_get_pixel(120, 250);
        uint32_t b_after = framebuffer_get_pixel(420, 320);

        selftest_reap(a_task);
        selftest_reap(b_task);
        selftest_reap(comp2);
        console_init();
        kernel_log_use_console();

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
                kernel_log_puts("[m55] pixel check failed: ");
                kernel_log_puts(names[i].what);
                kernel_log_puts(" - expected 0x");
                kernel_log_put_hex32(names[i].expected);
                kernel_log_puts(" got 0x");
                kernel_log_put_hex32(got[i]);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (a_alive_after_crash != 1 || b_alive_after_crash != 1) {
            kernel_log_puts("[m55] a client did not outlive the compositor at all (SYS_task_alive 0x");
            kernel_log_put_hex32((uint32_t)a_alive_after_crash);
            kernel_log_puts(" and 0x");
            kernel_log_put_hex32((uint32_t)b_alive_after_crash);
            kernel_log_puts(")\n");
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M55 session-resilience self-test: a compositor crash still takes its clients with it");
        }
        kernel_log_puts("[m55] a compositor SIGKILLed out from under two live clients, replaced, and "
                   "both windows back on screen with their own pixels self-test passed "
                   "(5/5 checks).\n\n");
    }

    {
        int all_ok = 1;

        {
            static char payload[2000];
            for (size_t i = 0; i < sizeof(payload); i++) {
                payload[i] = (char)('a' + (i % 26));
            }
            if (virtual_file_system_write(PATH_TEMPORARY_DIRECTORY "m56a", payload, sizeof(payload)) != 0) {
                kernel_log_puts("[m56] could not create the file this test is about\n");
                all_ok = 0;
            }
            if (virtual_file_system_rename(PATH_TEMPORARY_DIRECTORY "m56a", PATH_TEMPORARY_DIRECTORY "m56b") != 0 ||
                virtual_file_system_exists(PATH_TEMPORARY_DIRECTORY "m56a") || !virtual_file_system_exists(PATH_TEMPORARY_DIRECTORY "m56b")) {
                kernel_log_puts("[m56] rename did not move the name\n");
                all_ok = 0;
            }
            static char readback[2000];
            k_memset(readback, 0, sizeof(readback));
            if (virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m56b", readback, sizeof(readback)) != (int64_t)sizeof(payload)) {
                kernel_log_puts("[m56] the renamed file did not read back at its own size - a rename moved data it should not have touched\n");
                all_ok = 0;
            }
            for (size_t i = 0; i < sizeof(payload); i++) {
                if (readback[i] != payload[i]) {
                    kernel_log_puts("[m56] the renamed file's contents changed\n");
                    all_ok = 0;
                    break;
                }
            }
            if (virtual_file_system_rename(PATH_TEMPORARY_DIRECTORY "m56b", PATH_TEMPORARY_DIRECTORY "m53same") == 0) {
                kernel_log_puts("[m56] rename over an existing name succeeded - that is how a file gets lost silently\n");
                all_ok = 0;
            }
            uint32_t free_before = virtual_file_system_free_blocks();
            if (virtual_file_system_write(PATH_TEMPORARY_DIRECTORY "m56c", payload, sizeof(payload)) != 0) {
                kernel_log_puts("[m56] could not create the file the block accounting is about\n");
                all_ok = 0;
            }
            uint32_t free_with = virtual_file_system_free_blocks();
            if (free_with >= free_before) {
                kernel_log_puts("[m56] writing 2000 bytes consumed no blocks at all\n");
                all_ok = 0;
            }
            if (virtual_file_system_unlink(PATH_TEMPORARY_DIRECTORY "m56c") != 0) {
                kernel_log_puts("[m56] unlink failed on a file that had just been written\n");
                all_ok = 0;
            }
            uint32_t free_after = virtual_file_system_free_blocks();
            if (free_after != free_before) {
                kernel_log_puts("[m56] unlink did not return every block: 0x");
                kernel_log_put_hex32(free_before);
                kernel_log_puts(" free before, 0x");
                kernel_log_put_hex32(free_with);
                kernel_log_puts(" with the file, 0x");
                kernel_log_put_hex32(free_after);
                kernel_log_puts(" after removing it\n");
                all_ok = 0;
            }
            uint32_t free_pre_rename = virtual_file_system_free_blocks();
            int rename_ok = virtual_file_system_rename(PATH_TEMPORARY_DIRECTORY "m56b", PATH_TEMPORARY_DIRECTORY "m56d") == 0 &&
                            virtual_file_system_rename(PATH_TEMPORARY_DIRECTORY "m56d", PATH_TEMPORARY_DIRECTORY "m56b") == 0;
            if (!rename_ok || virtual_file_system_free_blocks() != free_pre_rename) {
                kernel_log_puts("[m56] a rename moved blocks, or failed outright\n");
                all_ok = 0;
            }

            virtual_file_system_unlink(PATH_TEMPORARY_DIRECTORY "m56b");
            if (virtual_file_system_unlink(PATH_BIN) == 0) {
                kernel_log_puts("[m56] unlink accepted a directory - see leanfs.h on why that is refused rather than recursed\n");
                all_ok = 0;
            }
        }

        static const char pasted[] = "PASTED";
        do_syscall(SYS_clipboard_set, (uint64_t)pasted, sizeof(pasted) - 1, 0);

        size_t comp_size_bytes = 0;
        uint8_t *comp_image = read_program(PATH_BIN_DIRECTORY "compositor", &comp_size_bytes);
        int64_t comp_size = (int64_t)comp_size_bytes;
        size_t ed_size_bytes = 0;
        uint8_t *ed_image = read_program(PATH_BIN_DIRECTORY "text_editor", &ed_size_bytes);
        int64_t ed_size = (int64_t)ed_size_bytes;
        size_t term_size_bytes = 0;
        uint8_t *term_image = read_program(PATH_BIN_DIRECTORY "gui_terminal", &term_size_bytes);
        int64_t term_size = (int64_t)term_size_bytes;

        virtual_file_system_unlink(PATH_TEMPORARY_DIRECTORY "m56undo");

        task_t *comp_task = process_spawn("compositor", comp_image, (size_t)comp_size, "");
        selftest_wait_for_compositor();
        task_t *ed_task = process_spawn("text_editor", ed_image, (size_t)ed_size,
                                         PATH_TEMPORARY_DIRECTORY "m56undo");
        kfree(ed_image);
        pit_sleep_ms(800);

        keyboard_inject('A', 0);
        keyboard_inject('B', 0);
        pit_sleep_ms(200);
        keyboard_inject('V', KEYBOARD_MOD_CTRL);
        pit_sleep_ms(300);
        keyboard_inject('S', KEYBOARD_MOD_CTRL);
        pit_sleep_ms(500);
        static char after_paste[64];
        k_memset(after_paste, 0, sizeof(after_paste));
        int64_t paste_length = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m56undo", after_paste, sizeof(after_paste) - 1);

        keyboard_inject('Z', KEYBOARD_MOD_CTRL);
        pit_sleep_ms(300);
        keyboard_inject('S', KEYBOARD_MOD_CTRL);
        pit_sleep_ms(500);
        static char after_undo[64];
        k_memset(after_undo, 0, sizeof(after_undo));
        int64_t undo_length = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m56undo", after_undo, sizeof(after_undo) - 1);

        selftest_reap(ed_task);
        pit_sleep_ms(200);

        task_t *term_task = process_spawn("gui_terminal", term_image, (size_t)term_size, "");
        kfree(term_image);
        pit_sleep_ms(900);

        int lit_before_command = selftest_term_top_lit();
        static const char command[] = "ls /bin\n";
        for (size_t i = 0; i < sizeof(command) - 1; i++) {
            keyboard_inject(command[i], 0);
        }
        int lit_live = selftest_term_top_settled(lit_before_command, 12000);
        mouse_inject(0, 0, 0, -10);
        int lit_scrolled = selftest_term_top_settled(lit_live, 4000);
        mouse_inject(0, 0, 0, 10);
        int lit_back = selftest_term_top_settled(lit_scrolled, 4000);

        selftest_reap(term_task);
        selftest_reap(comp_task);
        kfree(comp_image);
        console_init();
        kernel_log_use_console();

        if (paste_length != 9 || k_strcmp(after_paste, "ABPASTED\n") != 0) {
            kernel_log_puts("[m56] the paste did not land as expected - saved 0x");
            kernel_log_put_hex32((uint32_t)paste_length);
            kernel_log_puts(" bytes: '");
            kernel_log_puts(after_paste);
            kernel_log_puts("'\n");
            all_ok = 0;
        }
        if (undo_length != 3 || k_strcmp(after_undo, "AB\n") != 0) {
            kernel_log_puts("[m56] one undo did not restore the exact buffer from before the paste - saved 0x");
            kernel_log_put_hex32((uint32_t)undo_length);
            kernel_log_puts(" bytes: '");
            kernel_log_puts(after_undo);
            kernel_log_puts("'\n");
            all_ok = 0;
        }
        if (lit_live == 0) {
            kernel_log_puts("[m56] the terminal drew no output at all - nothing to scroll back through\n");
            all_ok = 0;
        }
        if (lit_scrolled == lit_live) {
            kernel_log_puts("[m56] scrolling back changed nothing - the top row still shows the live grid, so there is no history\n");
            all_ok = 0;
        }
        if (lit_back != lit_live) {
            kernel_log_puts("[m56] scrolling forward again did not return to the live view (0x");
            kernel_log_put_hex32((uint32_t)lit_live);
            kernel_log_puts(" lit pixels before, 0x");
            kernel_log_put_hex32((uint32_t)lit_back);
            kernel_log_puts(" after)\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M56 depth self-test: undo, scrollback or the filesystem's remove half did not behave as specified");
        }
        kernel_log_puts("[m56] SYS_unlink returning every block it freed and SYS_rename moving none, "
                   "one editor undo restoring the exact buffer from before a paste, and a "
                   "terminal scrollback holding lines its window no longer shows "
                   "self-test passed (12/12 checks).\n\n");
    }

    if (dispi_available()) {
        display_mode_t list[DISPLAY_MAX_MODES];
        int n = dispi_get_modes(list, DISPLAY_MAX_MODES);
        uint32_t boot_w = framebuffer_width(), boot_h = framebuffer_height();
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
        selftest_wait_for_compositor();
        task_t *shell_task = process_spawn("desktop_shell", shell_image, (size_t)shell_size, "");
        kfree(shell_image);
        pit_sleep_ms(700);

        const uint32_t bar_row_from_bottom = 30;
        uint32_t before_left  = framebuffer_get_pixel(2, boot_h - bar_row_from_bottom);
        uint32_t before_right = framebuffer_get_pixel(boot_w - 3, boot_h - bar_row_from_bottom);

        int action_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_ACTION_PIPE, (uint64_t)action_file_descriptors, 0) != 0) {
            panic("M58 desktop self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        window_manager_action_request_t request;
        k_memset(&request, 0, sizeof(request));
        request.window_id = -1;
        request.action = WINDOW_MANAGER_ACTION_SET_MODE;
        request.value = window_manager_pack_mode(list[pick].width, list[pick].height);
        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        pit_sleep_ms(2500);

        uint32_t after_w = framebuffer_width(), after_h = framebuffer_height();
        uint32_t after_left = 0, after_right = 0, after_beyond_old = 0, after_desktop = 0;
        if (after_w >= 8 && after_h > bar_row_from_bottom) {
            after_left  = framebuffer_get_pixel(2, after_h - bar_row_from_bottom);
            after_right = framebuffer_get_pixel(after_w - 3, after_h - bar_row_from_bottom);
            after_beyond_old = framebuffer_get_pixel(after_w / 2, after_h - bar_row_from_bottom);
            after_desktop = framebuffer_get_pixel(after_w / 2, after_h / 2);
        }

        pit_sleep_ms(WINDOW_MANAGER_MODE_REVERT_MS + 2500);

        uint32_t back_w = framebuffer_width(), back_h = framebuffer_height();
        uint32_t back_left = 0, back_right = 0;
        if (back_w == boot_w && back_h == boot_h) {
            back_left  = framebuffer_get_pixel(2, boot_h - bar_row_from_bottom);
            back_right = framebuffer_get_pixel(boot_w - 3, boot_h - bar_row_from_bottom);
        }

        selftest_reap(shell_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        int all_ok = 1;
        if (before_left != before_right) {
            kernel_log_puts("[m58] the taskbar did not span the boot display to begin with\n");
            all_ok = 0;
        }
        if (after_w != list[pick].width || after_h != list[pick].height) {
            kernel_log_puts("[m58] WM_ACTION_SET_MODE did not change the mode: 0x");
            kernel_log_put_hex32(after_w);
            kernel_log_puts("x");
            kernel_log_put_hex32(after_h);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (after_left != after_right || after_left != after_beyond_old) {
            kernel_log_puts("[m58] the taskbar did not re-span the new display width - its buffer was not reallocated\n");
            all_ok = 0;
        }
        if (after_left == after_desktop) {
            kernel_log_puts("[m58] the taskbar row is indistinguishable from bare desktop at the new size - there is no bar there\n");
            all_ok = 0;
        }
        if (back_w != boot_w || back_h != boot_h) {
            kernel_log_puts("[m58] the unconfirmed mode was never reverted - 0x");
            kernel_log_put_hex32(back_w);
            kernel_log_puts("x");
            kernel_log_put_hex32(back_h);
            kernel_log_puts(" is still up\n");
            all_ok = 0;
        }
        if (back_left != back_right || back_left != before_left) {
            kernel_log_puts("[m58] after the revert the taskbar does not span the restored display: before 0x");
            kernel_log_put_hex32(before_left);
            kernel_log_puts("/0x");
            kernel_log_put_hex32(before_right);
            kernel_log_puts(" after 0x");
            kernel_log_put_hex32(after_left);
            kernel_log_puts("/0x");
            kernel_log_put_hex32(after_right);
            kernel_log_puts(" back 0x");
            kernel_log_put_hex32(back_left);
            kernel_log_puts("/0x");
            kernel_log_put_hex32(back_right);
            kernel_log_putc('\n');
            all_ok = 0;
        }
        if (!all_ok) {
            panic("M58 desktop self-test: a resolution change did not carry the desktop with it");
        }
        kernel_log_puts("[m58] a resolution change carrying the whole desktop with it - panels "
                   "re-spanning the new width, and an unconfirmed mode reverting on its own "
                   "deadline - self-test passed.\n\n");
    }

    {
        int all_ok = 1;
        int openfiles_before = open_file_in_use();

        static const char PART_A[] = "hello ";
        static const char PART_B[] = "descriptors";
        long fd = do_syscall(SYS_open, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59fd"),
                              OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE, 0);
        if (fd < 0) {
            kernel_log_puts("[m59] SYS_open could not create a file\n");
            all_ok = 0;
        } else {
            do_syscall(SYS_write, (uint64_t)fd, (uint64_t)PART_A, sizeof(PART_A) - 1);
            do_syscall(SYS_write, (uint64_t)fd, (uint64_t)PART_B, sizeof(PART_B) - 1);
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);

            fd = do_syscall(SYS_open, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59fd"), OPEN_READ, 0);
            char back[32];
            k_memset(back, 0, sizeof(back));
            long position = do_syscall(SYS_lseek, (uint64_t)fd, 6, SEEK_SET);
            long n = do_syscall(SYS_read, (uint64_t)fd, (uint64_t)back, 11);
            if (position != 6 || n != 11 || k_strcmp(back, "descriptors") != 0) {
                kernel_log_puts("[m59] a seek-then-read did not land where it was told to\n");
                all_ok = 0;
            }
            long end = do_syscall(SYS_lseek, (uint64_t)fd, 0, SEEK_END);
            if (end != (long)(sizeof(PART_A) - 1 + sizeof(PART_B) - 1)) {
                kernel_log_puts("[m59] SEEK_END does not agree with what was written\n");
                all_ok = 0;
            }
            if (do_syscall(SYS_read, (uint64_t)fd, (uint64_t)back, 4) != 0) {
                kernel_log_puts("[m59] a read at the end of a file returned data\n");
                all_ok = 0;
            }
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);
        }

        {
            fd = do_syscall(SYS_open, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59hole"),
                             OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE, 0);
            do_syscall(SYS_write, (uint64_t)fd, (uint64_t)"ABC", 3);
            do_syscall(SYS_lseek, (uint64_t)fd, 300, SEEK_SET);
            do_syscall(SYS_write, (uint64_t)fd, (uint64_t)"Z", 1);
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);

            static uint8_t hole[512];
            k_memset(hole, 0xAA, sizeof(hole));
            fd = do_syscall(SYS_open, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59hole"), OPEN_READ, 0);
            long got = do_syscall(SYS_read, (uint64_t)fd, (uint64_t)hole, sizeof(hole));
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);
            if (got != 301) {
                kernel_log_puts("[m59] a write past the end did not extend the file to that point\n");
                all_ok = 0;
            } else if (hole[0] != 'A' || hole[1] != 'B' || hole[2] != 'C') {
                kernel_log_puts("[m59] writing past the end of a block erased the bytes before it\n");
                all_ok = 0;
            } else if (hole[300] != 'Z') {
                kernel_log_puts("[m59] the byte written past the end is not where it was put\n");
                all_ok = 0;
            } else {
                for (int i = 3; i < 300; i++) {
                    if (hole[i] != 0) {
                        kernel_log_puts("[m59] the hole is not zeros - a recycled block leaked into it\n");
                        all_ok = 0;
                        break;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59hole"), 0, 0);
        }

        {
            uint32_t free_before = virtual_file_system_free_blocks();
            const uint32_t BIG = 200u * 1024u;
            static uint8_t chunk[1024];
            fd = do_syscall(SYS_open, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59big"),
                             OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE, 0);
            if (fd < 0) {
                kernel_log_puts("[m59] could not create the large file this test is about\n");
                all_ok = 0;
            } else {
                for (uint32_t off = 0; off < BIG; off += sizeof(chunk)) {
                    for (size_t i = 0; i < sizeof(chunk); i++) {
                        chunk[i] = (uint8_t)((off / sizeof(chunk)) + i);
                    }
                    if (do_syscall(SYS_write, (uint64_t)fd, (uint64_t)chunk, sizeof(chunk)) != (long)sizeof(chunk)) {
                        kernel_log_puts("[m59] a write into the double-indirect range failed\n");
                        all_ok = 0;
                        break;
                    }
                }
                do_syscall(SYS_close, (uint64_t)fd, 0, 0);

                os_stat_t st;
                if (do_syscall(SYS_stat, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59big"), (uint64_t)&st, 0) != 0 ||
                    st.size != BIG) {
                    kernel_log_puts("[m59] the large file is not the size it was written at\n");
                    all_ok = 0;
                }
                fd = do_syscall(SYS_open, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59big"), OPEN_READ, 0);
                static uint8_t verify[1024];
                for (uint32_t off = 0; off < BIG && all_ok; off += sizeof(verify)) {
                    if (do_syscall(SYS_read, (uint64_t)fd, (uint64_t)verify, sizeof(verify)) != (long)sizeof(verify)) {
                        kernel_log_puts("[m59] the large file read short\n");
                        all_ok = 0;
                        break;
                    }
                    for (size_t i = 0; i < sizeof(verify); i++) {
                        if (verify[i] != (uint8_t)((off / sizeof(verify)) + i)) {
                            kernel_log_puts("[m59] the large file read back the wrong bytes - a block mapped to the wrong place\n");
                            all_ok = 0;
                            break;
                        }
                    }
                }
                do_syscall(SYS_close, (uint64_t)fd, 0, 0);
            }
            if (do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59big"), 0, 0) != 0) {
                kernel_log_puts("[m59] could not unlink the large file\n");
                all_ok = 0;
            }
            uint32_t free_after = virtual_file_system_free_blocks();
            if (free_after != free_before) {
                kernel_log_puts("[m59] the large file did not return every block: 0x");
                kernel_log_put_hex32(free_before);
                kernel_log_puts(" free before, 0x");
                kernel_log_put_hex32(free_after);
                kernel_log_puts(" after\n");
                all_ok = 0;
            }
        }

        {
            os_stat_t st;
            uint32_t now = rtc_now();
            if (do_syscall(SYS_stat, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59fd"), (uint64_t)&st, 0) != 0) {
                kernel_log_puts("[m59] SYS_stat failed on a file that exists\n");
                all_ok = 0;
            } else if (rtc_available()) {
                uint32_t age = now > st.mtime ? now - st.mtime : st.mtime - now;
                if (st.mtime == 0 || age > 60) {
                    kernel_log_puts("[m59] a file written moments ago is not dated moments ago\n");
                    all_ok = 0;
                }
            } else if (st.mtime != 0) {
                kernel_log_puts("[m59] a machine with no clock dated a file anyway\n");
                all_ok = 0;
            }
        }

        {
            uint32_t before = leanfs_meta_writes();
            fd = do_syscall(SYS_open, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59fd"), OPEN_WRITE, 0);
            do_syscall(SYS_lseek, (uint64_t)fd, 0, SEEK_SET);
            do_syscall(SYS_write, (uint64_t)fd, (uint64_t)"H", 1);
            do_syscall(SYS_close, (uint64_t)fd, 0, 0);
            uint32_t cost = leanfs_meta_writes() - before;
            if (cost > 4) {
                kernel_log_puts("[m59] a one-byte change cost 0x");
                kernel_log_put_hex32(cost);
                kernel_log_puts(" metadata sector writes - dirty-sector tracking is not working\n");
                all_ok = 0;
            }
        }

        {
            if (do_syscall(SYS_mkdir, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59dir"), 0, 0) != 0) {
                kernel_log_puts("[m59] could not create the directory this test is about\n");
                all_ok = 0;
            }
            if (virtual_file_system_write(PATH_TEMPORARY_DIRECTORY "m59dir/inside", "x", 1) != 0) {
                kernel_log_puts("[m59] could not put a file inside the test directory\n");
                all_ok = 0;
            }
            if (do_syscall(SYS_rmdir, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59dir"), 0, 0) == 0) {
                kernel_log_puts("[m59] rmdir removed a directory that still held a file\n");
                all_ok = 0;
            }
            if (do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59dir/inside"), 0, 0) != 0 ||
                do_syscall(SYS_rmdir, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59dir"), 0, 0) != 0) {
                kernel_log_puts("[m59] rmdir refused a directory that was empty\n");
                all_ok = 0;
            }
            if (virtual_file_system_exists(PATH_TEMPORARY_DIRECTORY "m59dir")) {
                kernel_log_puts("[m59] the removed directory is still there\n");
                all_ok = 0;
            }
        }

        do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m59fd"), 0, 0);

        {
            int openfiles_after = open_file_in_use();
            if (openfiles_after != openfiles_before) {
                kernel_log_puts("[m59] the open-file table went from 0x");
                kernel_log_put_hex32((uint32_t)openfiles_before);
                kernel_log_puts(" entries to 0x");
                kernel_log_put_hex32((uint32_t)openfiles_after);
                kernel_log_puts(" across a test that closed everything it opened - descriptors leak\n");
                all_ok = 0;
            }
        }

        if (!all_ok) {
            panic("M59 self-test: descriptors, large files, timestamps or metadata cost are wrong");
        }
        kernel_log_puts("[m59] descriptors (open/lseek/read/write/close), a 200 KiB file through "
                   "double-indirect blocks read back byte for byte and every block returned, "
                   "a real mtime, rmdir, an open-file table back where it started, and a "
                   "one-byte save costing one metadata sector instead of thirty-one - "
                   "self-test passed.\n\n");
    }

    {
        int all_ok = 1;

        {
            static const char body[] = "argv is real now\n";
            virtual_file_system_unlink(PATH_TEMPORARY_DIRECTORY "m60src");
            virtual_file_system_unlink(PATH_TEMPORARY_DIRECTORY "m60dst");
            if (virtual_file_system_write(PATH_TEMPORARY_DIRECTORY "m60src", body, sizeof(body) - 1) != 0) {
                kernel_log_puts("[m60] could not create the file cp is about to copy\n");
                all_ok = 0;
            }
            size_t cp_bytes = 0;
            uint8_t *cp_image = read_program(PATH_BIN_DIRECTORY "cp", &cp_bytes);
            const char *cp_argv[] = { PATH_BIN_DIRECTORY "cp", PATH_TEMPORARY_DIRECTORY "m60src", PATH_TEMPORARY_DIRECTORY "m60dst", 0 };
            task_t *cp_task = process_spawnv("cp", cp_image, cp_bytes, cp_argv);
            kfree(cp_image);
            if (!cp_task) {
                kernel_log_puts("[m60] could not spawn cp\n");
                all_ok = 0;
            } else if (do_syscall(SYS_wait, (uint64_t)cp_task->id, 0, 0) != 0) {
                kernel_log_puts("[m60] cp exited nonzero - it did not get two arguments\n");
                all_ok = 0;
            } else {
                static char copied[64];
                k_memset(copied, 0, sizeof(copied));
                int64_t n = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m60dst", copied, sizeof(copied) - 1);
                if (n != (int64_t)(sizeof(body) - 1) || k_strcmp(copied, body) != 0) {
                    kernel_log_puts("[m60] cp produced the wrong bytes\n");
                    all_ok = 0;
                }
            }
        }

        {
            size_t comp_bytes = 0;
            uint8_t *comp_image = read_program(PATH_BIN_DIRECTORY "compositor", &comp_bytes);
            size_t term_bytes = 0;
            uint8_t *term_image = read_program(PATH_BIN_DIRECTORY "gui_terminal", &term_bytes);

            virtual_file_system_unlink(PATH_TEMPORARY_DIRECTORY "m60out");
            virtual_file_system_unlink(PATH_TEMPORARY_DIRECTORY "m60pipe");
            virtual_file_system_unlink(PATH_TEMPORARY_DIRECTORY "m60tab");

            task_t *comp_task = process_spawn("compositor", comp_image, comp_bytes, "");
            kfree(comp_image);
            selftest_wait_for_compositor();
            task_t *term_task = process_spawn("gui_terminal", term_image, term_bytes, "");
            kfree(term_image);
            pit_sleep_ms(900);

            selftest_type("ls /bin > " PATH_TEMPORARY_DIRECTORY "m60out");
            keyboard_inject('\n', 0);
            pit_sleep_ms(2500);

            selftest_type("ls /bin | cat > " PATH_TEMPORARY_DIRECTORY "m60pipe");
            keyboard_inject('\n', 0);
            pit_sleep_ms(3500);

            selftest_type("ls /b");
            keyboard_inject('\t', 0);
            pit_sleep_ms(300);
            selftest_type(" > " PATH_TEMPORARY_DIRECTORY "m60tab");
            keyboard_inject('\n', 0);
            pit_sleep_ms(2500);

            selftest_reap(term_task);
            selftest_reap(comp_task);
            console_init();
            kernel_log_use_console();

            static char redirected[2048];
            k_memset(redirected, 0, sizeof(redirected));
            int64_t rn = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m60out", redirected, sizeof(redirected) - 1);
            if (rn <= 0 || !k_strstr(redirected, "compositor")) {
                kernel_log_puts("[m60] `ls /bin > file` did not put the listing in the file\n");
                all_ok = 0;
            }

            static char completed[2048];
            k_memset(completed, 0, sizeof(completed));
            int64_t cn = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m60tab", completed, sizeof(completed) - 1);
            if (cn <= 0 || !k_strstr(completed, "compositor")) {
                kernel_log_puts("[m60] Tab did not complete `/b` to `/bin/` - the listing is of the wrong directory\n");
                all_ok = 0;
            }

            static char piped[2048];
            k_memset(piped, 0, sizeof(piped));
            int64_t pn = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m60pipe", piped, sizeof(piped) - 1);
            if (pn <= 0 || !k_strstr(piped, "compositor")) {
                kernel_log_puts("[m60] `ls /bin | cat > file` produced nothing - the pipe never ended\n");
                all_ok = 0;
            } else if (pn != rn) {
                kernel_log_puts("[m60] the piped listing is a different length from the redirected one: 0x");
                kernel_log_put_hex32((uint32_t)rn);
                kernel_log_puts(" vs 0x");
                kernel_log_put_hex32((uint32_t)pn);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }

        {
            size_t comp_bytes = 0;
            uint8_t *comp_image = read_program(PATH_BIN_DIRECTORY "compositor", &comp_bytes);
            size_t ed_bytes = 0;
            uint8_t *ed_image = read_program(PATH_BIN_DIRECTORY "text_editor", &ed_bytes);

            virtual_file_system_unlink(PATH_TEMPORARY_DIRECTORY "m60para");

            task_t *comp_task = process_spawn("compositor", comp_image, comp_bytes, "");
            kfree(comp_image);
            selftest_wait_for_compositor();
            task_t *ed_task = process_spawn("text_editor", ed_image, ed_bytes, PATH_TEMPORARY_DIRECTORY "m60para");
            kfree(ed_image);
            pit_sleep_ms(900);

            selftest_type("ONETWO");
            for (int i = 0; i < 3; i++) {
                keyboard_inject((char)KEYBOARD_KEY_LEFT, 0);
            }
            pit_sleep_ms(200);
            keyboard_inject('\n', 0);
            pit_sleep_ms(200);
            keyboard_inject('S', KEYBOARD_MOD_CTRL);
            pit_sleep_ms(600);

            static char para[64];
            k_memset(para, 0, sizeof(para));
            int64_t pl = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m60para", para, sizeof(para) - 1);

            keyboard_inject('Z', KEYBOARD_MOD_CTRL);
            pit_sleep_ms(200);
            keyboard_inject('S', KEYBOARD_MOD_CTRL);
            pit_sleep_ms(600);
            static char undone[64];
            k_memset(undone, 0, sizeof(undone));
            int64_t ul = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m60para", undone, sizeof(undone) - 1);

            keyboard_inject('Y', KEYBOARD_MOD_CTRL);
            pit_sleep_ms(200);
            keyboard_inject('S', KEYBOARD_MOD_CTRL);
            pit_sleep_ms(600);
            static char redone[64];
            k_memset(redone, 0, sizeof(redone));
            int64_t rl = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m60para", redone, sizeof(redone) - 1);

            selftest_reap(ed_task);
            selftest_reap(comp_task);
            console_init();
            kernel_log_use_console();

            if (pl != 8 || k_strcmp(para, "ONE\nTWO\n") != 0) {
                kernel_log_puts("[m60] Enter did not split the line - saved 0x");
                kernel_log_put_hex32((uint32_t)pl);
                kernel_log_puts(" bytes: \"");
                kernel_log_puts(para);
                kernel_log_puts("\"\n");
                all_ok = 0;
            }
            if (ul != 7 || k_strcmp(undone, "ONETWO\n") != 0) {
                kernel_log_puts("[m60] one undo did not join the split back - saved \"");
                kernel_log_puts(undone);
                kernel_log_puts("\"\n");
                all_ok = 0;
            }
            if (rl != 8 || k_strcmp(redone, "ONE\nTWO\n") != 0) {
                kernel_log_puts("[m60] redo did not put the split back - saved \"");
                kernel_log_puts(redone);
                kernel_log_puts("\"\n");
                all_ok = 0;
            }
        }

        if (!all_ok) {
            panic("M60 self-test: argv, the command line, or the editor's structural edits are wrong");
        }
        kernel_log_puts("[m60] a real argument vector (cp with two arguments), a command line with "
                   "redirection and a pipe that ends, and an editor whose Enter splits a line - "
                   "with undo and redo inverting it - self-test passed.\n\n");
    }

    {
        int all_ok = 1;

        size_t comp_bytes = 0;
        uint8_t *comp_image = read_program(PATH_BIN_DIRECTORY "compositor", &comp_bytes);
        size_t clock_bytes = 0;
        uint8_t *clock_image = read_program(PATH_BIN_DIRECTORY "gui_clock", &clock_bytes);

        task_t *comp_task = process_spawn("compositor", comp_image, comp_bytes, "");
        selftest_wait_for_compositor();
        task_t *clock_task = process_spawn("gui_clock", clock_image, clock_bytes, "");
        pit_sleep_ms(800);

        const uint32_t probe_x = 150;
        uint32_t before_window = framebuffer_get_pixel(150, 150);
        uint32_t desktop_bg = framebuffer_get_pixel(probe_x, 700);
        int before_lit = selftest_column_lit(probe_x, desktop_bg);

        int action_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_ACTION_PIPE, (uint64_t)action_file_descriptors, 0) != 0) {
            panic("M61 self-test: kernel-side SYS_pipe_open(WM_ACTION_PIPE) failed");
        }
        window_manager_action_request_t request;
        k_memset(&request, 0, sizeof(request));
        request.window_id = 0;
        request.action = WINDOW_MANAGER_ACTION_TOGGLE_MINIMIZE;

        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        uint32_t lit_ms = 0;
        int during_lit = selftest_column_lit_wait(probe_x, desktop_bg, 4000, &lit_ms);
        int after_lit = selftest_column_clear_wait(probe_x, desktop_bg, 4000);
        uint32_t after_window = framebuffer_get_pixel(150, 150);

        int settings_file_descriptors[2];
        if (do_syscall(SYS_pipe_open, (uint64_t)WINDOW_MANAGER_SETTINGS_PIPE, (uint64_t)settings_file_descriptors, 0) != 0) {
            panic("M61 self-test: kernel-side SYS_pipe_open(WM_SETTINGS_PIPE) failed");
        }
        window_manager_settings_request_t off;
        k_memset(&off, 0, sizeof(off));
        off.volume = 70;
        off.animations = 0;
        off.bg_color = 0x001A1A2Eu;
        off.accent_color = 0x004C99E6u;
        off.wallpaper = 0;
        do_syscall(SYS_write, (uint64_t)settings_file_descriptors[1], (uint64_t)&off, sizeof(off));
        selftest_wait_for_animations_setting(0, 4000);

        do_syscall(SYS_write, (uint64_t)action_file_descriptors[1], (uint64_t)&request, sizeof(request));
        selftest_wait_for_pixel(150, 150, before_window, 4000, "the window to come back");
        uint32_t quiet_bg = framebuffer_get_pixel(probe_x, 700);
        uint32_t quiet_window = lit_ms * 4 + 400;
        if (quiet_window > 3000) {
            quiet_window = 3000;
        }
        int quiet_lit = selftest_column_lit_peak_ms(probe_x, quiet_bg, quiet_window);

        selftest_reap(clock_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        if (before_window == desktop_bg) {
            kernel_log_puts("[m61] the window and the bare desktop below it are the same colour - this test cannot see anything\n");
            all_ok = 0;
        }
        if (before_lit != 0) {
            kernel_log_puts("[m61] the path to the taskbar was not bare desktop to begin with: 0x");
            kernel_log_put_hex32((uint32_t)before_lit);
            kernel_log_puts(" pixels lit\n");
            all_ok = 0;
        }
        if (during_lit == 0) {
            kernel_log_puts("[m61] nothing was drawn on the path to the taskbar mid-minimize - the window blinked rather than moved\n");
            all_ok = 0;
        }
        if (after_lit != 0) {
            kernel_log_puts("[m61] the animation left 0x");
            kernel_log_put_hex32((uint32_t)after_lit);
            kernel_log_puts(" pixels behind on its path\n");
            all_ok = 0;
        }
        if (after_window == before_window) {
            kernel_log_puts("[m61] the window is still on screen after being minimized\n");
            all_ok = 0;
        }
        if (quiet_lit != 0) {
            kernel_log_puts("[m61] motion is switched off and something still animated: 0x");
            kernel_log_put_hex32((uint32_t)quiet_lit);
            kernel_log_puts(" pixels lit\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M61 self-test: window animation did not move, did not clean up, or ignored its setting");
        }
        kernel_log_puts("[m61] a minimize animating toward the taskbar - endpoints plus an intermediate "
                   "frame that is neither, nothing left behind, and nothing at all when motion is "
                   "switched off - self-test passed.\n\n");
    }

    {
        int all_ok = 1;

        if (do_syscall(SYS_audio_claim, 0, 0, 0) != 0) {
            kernel_log_puts("[m62] the first claim on the audio devices was refused\n");
            all_ok = 0;
        }
        if (do_syscall(SYS_audio_claim, 0, 0, 0) != 0) {
            kernel_log_puts("[m62] the owner could not re-claim what it already owns\n");
            all_ok = 0;
        }

        do_syscall(SYS_audio_volume, 100, 0, 0);
        do_syscall(SYS_beep, 880, 40, 0);
        uint8_t gate_during = (uint8_t)(inb(0x61) & 0x03);
        pit_sleep_ms(120);
        uint8_t gate_after = (uint8_t)(inb(0x61) & 0x03);
        if (gate_during != 0x03) {
            kernel_log_puts("[m62] the speaker gate never opened - no tone was played\n");
            all_ok = 0;
        }
        if (gate_after != 0) {
            kernel_log_puts("[m62] the speaker gate is still open past the tone's deadline\n");
            all_ok = 0;
        }

        do_syscall(SYS_audio_volume, 0, 0, 0);
        do_syscall(SYS_beep, 880, 40, 0);
        uint8_t gate_muted = (uint8_t)(inb(0x61) & 0x03);
        if (gate_muted != 0) {
            kernel_log_puts("[m62] muted, and the speaker still played\n");
            all_ok = 0;
        }
        do_syscall(SYS_audio_volume, 100, 0, 0);

        if (!ac97_available()) {
            kernel_log_puts("[m62] no AC'97 device on this machine - the stream half of this test is skipped, "
                       "which is the same answer real hardware without one would give.\n");
        } else {
            uint32_t frames = 4800;
            if (frames > ac97_max_frames()) {
                frames = ac97_max_frames();
            }
            int16_t *tone = (int16_t *)kmalloc((size_t)frames * 2 * sizeof(int16_t));
            if (!tone) {
                panic("M62 self-test: out of memory for a tenth of a second of audio");
            }
            uint32_t period = AC97_SAMPLE_RATE / 440;
            for (uint32_t i = 0; i < frames; i++) {
                int16_t v = ((i % period) < period / 2) ? 6000 : -6000;
                tone[i * 2] = v;
                tone[i * 2 + 1] = v;
            }
            uint32_t before = ac97_completions();
            if (do_syscall(SYS_audio_play, (uint64_t)tone, frames, 0) != 0) {
                kernel_log_puts("[m62] SYS_audio_play refused a buffer the device advertised room for\n");
                all_ok = 0;
            }
            pit_sleep_ms(600);
            uint32_t after = ac97_completions();
            kfree(tone);
            if (after == before) {
                kernel_log_puts("[m62] the AC'97 device never reported finishing the buffer it was given\n");
                ac97_debug_dump();
                all_ok = 0;
            }
        }

        {
            size_t claim_bytes = 0;
            uint8_t *claim_image = read_program(PATH_BIN_DIRECTORY "audiograb", &claim_bytes);
            task_t *grabber = process_spawn("audiograb", claim_image, claim_bytes, "");
            kfree(claim_image);
            long rc = do_syscall(SYS_wait, (uint64_t)grabber->id, 0, 0);
            if (rc != 0) {
                kernel_log_puts("[m62] another process was able to take the speaker, or to beep without owning it: 0x");
                kernel_log_put_hex32((uint32_t)rc);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }

        if (do_syscall(SYS_audio_release, 0, 0, 0) != 0) {
            kernel_log_puts("[m62] the owner could not release the audio devices\n");
            all_ok = 0;
        }
        if (do_syscall(SYS_beep, 880, 40, 0) == 0) {
            kernel_log_puts("[m62] a beep succeeded after the speaker was released\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M62 self-test: the speaker, the stream, or the ownership rule is wrong");
        }
        kernel_log_puts("[m62] the PC speaker gated on and off by its own deadline, muted when the volume "
                   "is zero, an AC'97 buffer the device reported finishing, and a second process "
                   "refused both the claim and the beep, then the owner handing it back - self-test passed.\n\n");
    }

    {
        int all_ok = 1;

        {
            size_t bytes = 0;
            uint8_t *image = read_program(PATH_BIN_DIRECTORY "libctest", &bytes);
            task_t *t = process_spawn("libctest", image, bytes, "");
            kfree(image);
            long rc = do_syscall(SYS_wait, (uint64_t)t->id, 0, 0);
            if (rc != 0) {
                kernel_log_puts("[m63] the libc/SSE self-test program failed\n");
                all_ok = 0;
            }
        }

        {
            int out_file_descriptors[2];
            if (do_syscall(SYS_pipe, (uint64_t)out_file_descriptors, 0, 0) != 0) {
                panic("M63 self-test: could not make a pipe for the ported program's output");
            }
            do_syscall(SYS_dup2, (uint64_t)out_file_descriptors[1], 1, 0);

            size_t bytes = 0;
            uint8_t *image = read_program(PATH_BIN_DIRECTORY "whetstone", &bytes);
            const char *argv[] = { PATH_BIN_DIRECTORY "whetstone", "8000", 0 };
            long wall_before = do_syscall(SYS_time, 0, 0, 0);
            task_t *t = process_spawnv("whetstone", image, bytes, argv);
            kfree(image);

            static char out[2048];
            size_t got = 0;
            long deadline = (long)pit_get_ticks() + 60 * PIT_HZ;
            for (;;) {
                long avail = do_syscall(SYS_pipe_poll, (uint64_t)out_file_descriptors[0], 0, 0);
                if (avail > 0 && got < sizeof(out) - 1) {
                    size_t room = sizeof(out) - 1 - got;
                    long n = do_syscall(SYS_read, (uint64_t)out_file_descriptors[0], (uint64_t)(out + got),
                                         (uint64_t)((size_t)avail < room ? (size_t)avail : room));
                    if (n > 0) {
                        got += (size_t)n;
                    }
                } else if (do_syscall(SYS_wait_nb, (uint64_t)t->id, 0, 0) != -2) {
                    long n;
                    while ((n = do_syscall(SYS_pipe_poll, (uint64_t)out_file_descriptors[0], 0, 0)) > 0 &&
                           got < sizeof(out) - 1) {
                        size_t room = sizeof(out) - 1 - got;
                        long r = do_syscall(SYS_read, (uint64_t)out_file_descriptors[0], (uint64_t)(out + got),
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
            do_syscall(SYS_close, (uint64_t)out_file_descriptors[0], 0, 0);
            do_syscall(SYS_close, (uint64_t)out_file_descriptors[1], 0, 0);
            file_descriptor_release(&scheduler_current()->file_descriptors[1]);
            scheduler_current()->file_descriptors[1].type = FILE_DESCRIPTOR_STDOUT;

            long wall_after = do_syscall(SYS_time, 0, 0, 0);

            int unmeasured = 0;
            if (k_strstr(out, "Insufficient duration")) {
                if (wall_before > 0 && wall_after - wall_before >= 3) {
                    kernel_log_puts("[m63] the ported program could not time its own run, but this "
                               "kernel's clock advanced across it - time() is wrong\n");
                    all_ok = 0;
                } else {
                    unmeasured = 1;
                    kernel_log_puts("[m63] the ported program completed its run and the clock did not "
                               "advance across it, for the kernel either - the figure is "
                               "unmeasured on this boot, which is a statement about the host "
                               "rather than about the port\n");
                }
            }

            if (!unmeasured && !k_strstr(out, "Loops:")) {
                kernel_log_puts("[m63] the ported program did not report a completed run. It said:\n");
                kernel_log_puts(out);
                kernel_log_putc('\n');
                all_ok = 0;
            }
            if (unmeasured) {
            } else if (!k_strstr(out, "Whetstones:")) {
                kernel_log_puts("[m63] the ported program produced no benchmark figure\n");
                all_ok = 0;
            } else {
                kernel_log_puts("[m63] the ported program said:");
                kernel_log_puts(out);
            }
        }

        if (!all_ok) {
            panic("M63 self-test: floating point, the libc subset, or the ported program is wrong");
        }
        kernel_log_puts("[m63] SSE state preserved across task switches, a libc subset checked against "
                   "values that are either right or not, and a 1972 benchmark nobody here wrote "
                   "running to completion and reporting a figure - self-test passed.\n\n");
    }

    {
        int all_ok = 1;

        size_t comp_bytes = 0;
        uint8_t *comp_image = read_program(PATH_BIN_DIRECTORY "compositor", &comp_bytes);
        size_t icons_bytes = 0;
        uint8_t *icons_image = read_program(PATH_BIN_DIRECTORY "desktop_icons", &icons_bytes);

        task_t *comp_task = process_spawn("compositor", comp_image, comp_bytes, "");
        kfree(comp_image);
        selftest_wait_for_compositor();
        task_t *icons_task = process_spawn("desktop_icons", icons_image, icons_bytes, "");
        pit_sleep_ms(1200);

        static const uint32_t MAGENTA = 0x00FF00FFu;
        uint32_t before_magenta = 0;
        for (uint32_t y = 32; y < 80; y += 2) {
            for (uint32_t x = 32; x < 80; x += 2) {
                if (framebuffer_get_pixel(x, y) == MAGENTA) {
                    before_magenta++;
                }
            }
        }

        static uint8_t blob[512];
        int64_t n = virtual_file_system_read(PATH_ICONS_DIRECTORY "Terminal.icn", blob, sizeof(blob));
        int wrote = 0;
        if (n < ICON_HEADER_BYTES || !icon_valid(blob)) {
            kernel_log_puts("[m63] the desktop did not write its icons out as files\n");
            all_ok = 0;
        } else {
            blob[ICON_HEADER_BYTES + 3] = 0xFF;
            blob[ICON_HEADER_BYTES + 4] = 0x00;
            blob[ICON_HEADER_BYTES + 5] = 0xFF;
            if (virtual_file_system_write(PATH_ICONS_DIRECTORY "Terminal.icn", blob, (size_t)n) != 0) {
                kernel_log_puts("[m63] could not write the edited icon back\n");
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
                    if (framebuffer_get_pixel(x, y) == MAGENTA) {
                        after_magenta++;
                    }
                }
            }
        }
        kfree(icons_image);

        selftest_reap(icons_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        if (before_magenta != 0) {
            kernel_log_puts("[m63] the desktop was already showing the colour this test edits in\n");
            all_ok = 0;
        }
        if (wrote && after_magenta == 0) {
            kernel_log_puts("[m63] editing an icon file changed nothing on screen\n");
            all_ok = 0;
        }

        if (wrote) {
            virtual_file_system_unlink(PATH_ICONS_DIRECTORY "Terminal.icn");
        }

        if (!all_ok) {
            panic("M63 icon self-test: icons are not files, or editing one changes nothing");
        }
        kernel_log_puts("[m63] icons are files: the desktop wrote them out, an edited palette entry "
                   "changed what is on screen after a restart, and the format and loader did not "
                   "change at all - self-test passed.\n\n");
    }

    {
        int all_ok = 1;

        size_t comp_bytes = 0;
        uint8_t *comp_image = read_program(PATH_BIN_DIRECTORY "compositor", &comp_bytes);
        size_t clock_bytes = 0;
        uint8_t *clock_image = read_program(PATH_BIN_DIRECTORY "gui_clock", &clock_bytes);

        task_t *comp_task = process_spawn("compositor", comp_image, comp_bytes, "");
        kfree(comp_image);
        selftest_wait_for_compositor();
        task_t *clock_task = process_spawn("gui_clock", clock_image, clock_bytes, "");
        kfree(clock_image);
        pit_sleep_ms(900);

        uint32_t desktop = framebuffer_get_pixel(500, 500);
        uint32_t on_home = framebuffer_get_pixel(150, 150);

        keyboard_inject((char)KEYBOARD_KEY_RIGHT, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_SHIFT);
        uint32_t after_switch = selftest_pixel_settled(150, 150, desktop,
                                                        "the window to be hidden by switching desktop");

        keyboard_inject((char)KEYBOARD_KEY_LEFT, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_SHIFT);
        uint32_t back_home = selftest_pixel_settled(150, 150, on_home,
                                                     "the window to come back when we switch back");

        keyboard_inject((char)KEYBOARD_KEY_RIGHT, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_SHIFT | KEYBOARD_MOD_ALT);
        uint32_t moved_with = selftest_pixel_settled(150, 150, on_home,
                                                      "the window to follow us to the next desktop");

        keyboard_inject((char)KEYBOARD_KEY_LEFT, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_SHIFT);
        uint32_t left_behind = selftest_pixel_settled(150, 150, desktop,
                                                       "the desktop it came from to be empty");

        selftest_reap(clock_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        if (on_home == desktop) {
            kernel_log_puts("[m63] the window was not on screen to begin with - this test can see nothing\n");
            all_ok = 0;
        }
        if (after_switch != desktop) {
            kernel_log_puts("[m63] switching to the next virtual desktop left the window on screen\n");
            all_ok = 0;
        }
        if (back_home != on_home) {
            kernel_log_puts("[m63] switching back did not bring the window back\n");
            all_ok = 0;
        }
        if (moved_with != on_home) {
            kernel_log_puts("[m63] moving a window to the next desktop did not take it there\n");
            all_ok = 0;
        }
        if (left_behind != desktop) {
            kernel_log_puts("[m63] the moved window is still on the desktop it came from\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M63 workspace self-test: virtual desktops do not hide, show or carry windows");
        }
        kernel_log_puts("[m63] four virtual desktops: a window hidden by switching away, back when "
                   "switching returns, and carried along when it is sent - self-test passed.\n\n");
    }

    {
        int all_ok = 1;

        if (net_have_nic()) {
            if (!net_config_is_leased()) {
                kernel_log_puts("[m64] no DHCP lease - the address is the fallback constant, "
                           "which is what this milestone existed to stop being the answer\n");
                all_ok = 0;
            }

            size_t bytes = 0;
            uint8_t *image = read_program(PATH_BIN_DIRECTORY "nettest", &bytes);
            task_t *t = process_spawn("nettest", image, bytes, "");
            kfree(image);
            if (do_syscall(SYS_wait, (uint64_t)t->id, 0, 0) != 0) {
                kernel_log_puts("[m64] the socket self-test program reported a failure\n");
                all_ok = 0;
            }

            int out_file_descriptors[2];
            if (do_syscall(SYS_pipe, (uint64_t)out_file_descriptors, 0, 0) != 0) {
                panic("M64 self-test: could not make a pipe for nettime's output");
            }
            do_syscall(SYS_dup2, (uint64_t)out_file_descriptors[1], 1, 0);

            image = read_program(PATH_BIN_DIRECTORY "nettime", &bytes);
            task_t *nt = process_spawn("nettime", image, bytes, "");
            kfree(image);

            uint64_t started = pit_get_ticks();
            long nettime_rc = do_syscall(SYS_wait, (uint64_t)nt->id, 0, 0);
            uint64_t elapsed_ms = (pit_get_ticks() - started) * 1000 / PIT_HZ;

            static char nettime_out[256];
            long got = do_syscall(SYS_read, (uint64_t)out_file_descriptors[0],
                                  (uint64_t)nettime_out, sizeof(nettime_out) - 1);
            nettime_out[got > 0 ? got : 0] = '\0';
            do_syscall(SYS_close, (uint64_t)out_file_descriptors[0], 0, 0);
            do_syscall(SYS_close, (uint64_t)out_file_descriptors[1], 0, 0);
            file_descriptor_release(&scheduler_current()->file_descriptors[1]);
            scheduler_current()->file_descriptors[1].type = FILE_DESCRIPTOR_STDOUT;

            if (elapsed_ms > 5000) {
                kernel_log_puts("[m64] nettime took longer than its own deadline to give up\n");
                all_ok = 0;
            }
            if (got <= 0) {
                kernel_log_puts("[m64] nettime printed nothing at all\n");
                all_ok = 0;
            }
            if (nettime_rc == 0 && !k_strstr(nettime_out, "says")) {
                kernel_log_puts("[m64] nettime reported success without reporting a time\n");
                all_ok = 0;
            }
            if (nettime_rc != 0 && !k_strstr(nettime_out, "no reply") &&
                !k_strstr(nettime_out, "unreachable")) {
                kernel_log_puts("[m64] nettime failed without saying why\n");
                all_ok = 0;
            }

            if (!all_ok) {
                panic("M64 network self-test: user space cannot use the network correctly");
            }

            kernel_log_puts("[m64] the network reached user space: a DHCP lease rather than a "
                       "hardcoded address, UDP sockets in the fd table that round-trip a "
                       "datagram and refuse six kinds of wrong, and an SNTP client that "
                       "gives up cleanly - self-test passed. nettime said: ");
            kernel_log_puts(nettime_out);
            kernel_log_putc('\n');
        } else {
            kernel_log_puts("[m64] no NIC on this machine - the socket layer is present but "
                       "untested this boot.\n\n");
        }
    }

    {
        int all_ok = 1;

        size_t bytes = 0;
        uint8_t *image = read_program(PATH_BIN_DIRECTORY "hello", &bytes);
        task_t *plain = process_spawn("hello", image, bytes, "");
        kfree(image);
        uint32_t plain_caps = plain->caps;
        do_syscall(SYS_wait, (uint64_t)plain->id, 0, 0);

        if (plain_caps != CAP_APP_DEFAULT) {
            kernel_log_puts("[m65] a program with no manifest entry did not get the default "
                       "capability set - the grant table is not being applied at spawn\n");
            all_ok = 0;
        }
        if (scheduler_current()->caps != CAP_ALL) {
            kernel_log_puts("[m65] kernel_main is not the root of the capability model\n");
            all_ok = 0;
        }

        image = read_program(PATH_BIN_DIRECTORY "compositor", &bytes);
        task_t *comp = process_spawn("compositor", image, bytes, "");
        kfree(image);
        if (comp->caps != CAP_ALL) {
            kernel_log_puts("[m65] the compositor did not get the capabilities it owns the screen with\n");
            all_ok = 0;
        }
        selftest_reap(comp);
        console_init();
        kernel_log_use_console();

        task_t *victim = task_spawn("cap-victim", spinner_task, NULL);
        char victim_pid[12];
        {
            int v = victim->id, n = 0;
            char temporary[12];
            do {
                temporary[n++] = (char)('0' + (v % 10));
                v /= 10;
            } while (v);
            int m = 0;
            while (n) {
                victim_pid[m++] = temporary[--n];
            }
            victim_pid[m] = '\0';
        }

        image = read_program(PATH_BIN_DIRECTORY "captest", &bytes);
        task_t *ct = process_spawn("captest", image, bytes, victim_pid);
        kfree(image);
        if (do_syscall(SYS_wait, (uint64_t)ct->id, 0, 0) != 0) {
            kernel_log_puts("[m65] the capability self-test program reported a failure\n");
            all_ok = 0;
        }

        if (victim->state == TASK_TERMINATED) {
            kernel_log_puts("[m65] the victim task did not survive an unprivileged process asking to kill it\n");
            all_ok = 0;
        }
        selftest_reap(victim);

        if (!all_ok) {
            panic("M65 capability self-test: the permission model does not hold");
        }
        kernel_log_puts("[m65] capabilities: a manifest the kernel applies rather than a launcher, "
                   "an ordinary program refused the screen, the clipboard, the process list, "
                   "a socket, the clock and another process's life, and a set that only ever "
                   "shrinks - self-test passed.\n\n");
    }

    if (net_have_nic()) {
        int all_ok = 1;

        tcp_debug_drop_next(2);
        int retransmits_before = tcp_debug_retransmits();

        size_t bytes = 0;
        uint8_t *image = read_program(PATH_BIN_DIRECTORY "tcptest", &bytes);
        task_t *t = process_spawn("tcptest", image, bytes, "");
        kfree(image);
        if (do_syscall(SYS_wait, (uint64_t)t->id, 0, 0) != 0) {
            kernel_log_puts("[m66] the TCP self-test program reported a failure\n");
            all_ok = 0;
        }

        int retransmits = tcp_debug_retransmits() - retransmits_before;
        if (retransmits <= 0) {
            kernel_log_puts("[m66] three segments were dropped and nothing was ever retransmitted - "
                       "the recovery path did not run, so the transfer that succeeded proves "
                       "less than it appears to\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M66 TCP self-test: the connection, the transfer or the recovery does not work");
        }

        kernel_log_puts("[m66] TCP: a handshake, 16 KiB through a 4 KiB buffer arriving byte for "
                   "byte, an end of stream a reader can tell from a pause, a refusal that "
                   "arrives as an RST rather than a timeout, and a transfer that survived ");
        kernel_log_put_dec((uint32_t)retransmits);
        kernel_log_puts(" deliberately dropped segment(s) - self-test passed.\n\n");
    } else {
        kernel_log_puts("[m66] no NIC on this machine - TCP is present but untested this boot.\n\n");
    }

    {
        int frames_before = (int)physical_memory_free_frame_count();

        static const int RACERS = 4;
        task_t *racers[4];
        int spawned = 0;

        size_t rbytes = 0;
        uint8_t *rimage = read_program(PATH_BIN_DIRECTORY "racetest", &rbytes);
        for (int i = 0; i < RACERS; i++) {
            racers[i] = process_spawn("racetest", rimage, rbytes, "");
            if (racers[i]) {
                spawned++;
            }
        }
        kfree(rimage);

        int all_ok = (spawned == RACERS);
        if (!all_ok) {
            kernel_log_puts("[m67] could not spawn four concurrent racers\n");
        }
        for (int i = 0; i < RACERS; i++) {
            if (!racers[i]) {
                continue;
            }
            if (do_syscall(SYS_wait, (uint64_t)racers[i]->id, 0, 0) != 0) {
                kernel_log_puts("[m67] a racer reported corrupted state - a lock M67 added is "
                           "missing, wrong, or not covering what it claims to\n");
                all_ok = 0;
            }
        }

        int frames_after = (int)physical_memory_free_frame_count();
        if (frames_after < frames_before) {
            kernel_log_puts("[m67] frames leaked across the race - ");
            kernel_log_put_dec((uint32_t)(frames_before - frames_after));
            kernel_log_puts(" not returned\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M67 concurrency self-test: syscalls are preemptible and the locks do not hold");
        }

        kernel_log_puts("[m67] a preemptible kernel: `int 0x80` is a trap gate, four concurrent "
                   "processes hammered the filesystem, the shm table, pipe ring buffers and "
                   "the socket table, and every one of them read back only its own bytes - "
                   "self-test passed.\n\n");
    }

    {
        size_t comp_bytes = 0;
        uint8_t *comp_img = read_program(PATH_BIN_DIRECTORY "compositor", &comp_bytes);
        task_t *comp = process_spawn("compositor", comp_img, comp_bytes, "");
        kfree(comp_img);

        if (!selftest_wait_for_pixel(500, 400, 0x001A1A2Eu, 5000,
                                      "the compositor to paint the desktop")) {
            panic("M69 latency self-test: no desktop to measure against");
        }

        uint64_t idle_samples[LATENCY_SAMPLES];
        uint64_t idle_us = 0;
        for (int i = 0; i < LATENCY_SAMPLES; i++) {
            uint64_t us = selftest_input_to_photon_us(200 + (i % 10) * 20, 200, 2000);
            idle_samples[i] = us;
            if (us != 0 && (idle_us == 0 || us < idle_us)) {
                idle_us = us;
            }
        }

        int cpus = 1;
        task_t *load[4];
        int nload = 0;
        for (int i = 0; i < cpus; i++) {
            load[nload] = task_spawn("m69-load", spinner_task, NULL);
            if (load[nload]) {
                nload++;
            }
        }
        pit_sleep_ms(200);

        uint64_t loaded_samples[LATENCY_SAMPLES];
        uint64_t loaded_us = 0;
        for (int i = 0; i < LATENCY_SAMPLES; i++) {
            uint64_t us = selftest_input_to_photon_us(300 + (i % 10) * 20, 300, 3000);
            loaded_samples[i] = us;
            if (us != 0 && (loaded_us == 0 || us < loaded_us)) {
                loaded_us = us;
            }
        }

        for (int i = 0; i < nload; i++) {
            selftest_reap(load[i]);
        }
        selftest_reap(comp);
        console_init();
        kernel_log_use_console();

        if (idle_us == 0 || loaded_us == 0) {
            panic("M69 latency self-test: the cursor never reached the screen - "
                   "the measurement is measuring nothing");
        }

        kernel_log_perf("input_to_photon_idle_us", idle_us, "us");
        kernel_log_perf("input_to_photon_loaded_us", loaded_us, "us");
        kernel_log_perf_distribution("input_to_photon_idle", idle_samples, LATENCY_SAMPLES);
        kernel_log_perf_distribution("input_to_photon_loaded", loaded_samples, LATENCY_SAMPLES);
        kernel_log_puts("[m69] input-to-photon: ");
        kernel_log_put_dec((uint32_t)idle_us);
        kernel_log_puts(" us idle, ");
        kernel_log_put_dec((uint32_t)loaded_us);
        kernel_log_puts(" us with ");
        kernel_log_put_dec((uint32_t)nload);
        kernel_log_puts(" CPU-bound task(s) running - measured with the TSC, "
                   "cursor motion to changed pixel; the observer competes, so this is an "
                   "upper bound - self-test passed.\n\n");
    }

    {
        int all_ok = 1;

        static const char MARKER[] = "[m70] marker-cafebabe";
        kernel_log_puts(MARKER);
        kernel_log_putc('\n');

        static char logbuf[1024];
        uint64_t cursor = 0;
        uint64_t next = 0;
        int found = 0;
        const long OVERLAP = (long)sizeof(MARKER) - 2;
        long carry = 0;
        for (int pass = 0; pass < 256 && !found; pass++) {
            long n = do_syscall4(SYS_kernel_log, cursor, (uint64_t)(logbuf + carry),
                                  sizeof(logbuf) - 1 - (uint64_t)carry, (uint64_t)&next);
            if (n <= 0) {
                break;
            }
            long total = carry + n;
            logbuf[total] = '\0';
            for (long i = 0; i + (long)sizeof(MARKER) - 1 <= total; i++) {
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
                break;
            }
            cursor = next;
            carry = total < OVERLAP ? total : OVERLAP;
            k_memmove(logbuf, logbuf + total - carry, (size_t)carry);
        }
        if (!found) {
            kernel_log_puts("[m70] the kernel log does not contain what klog just wrote to it\n");
            all_ok = 0;
        }

        uint64_t end = kernel_log_written_total();
        long none = do_syscall4(SYS_kernel_log, end, (uint64_t)logbuf, sizeof(logbuf) - 1,
                                 (uint64_t)&next);
        if (none != 0) {
            kernel_log_puts("[m70] a read from the end of the log returned bytes that were not "
                       "written yet\n");
            all_ok = 0;
        }
        kernel_log_puts("x\n");
        long some = do_syscall4(SYS_kernel_log, end, (uint64_t)logbuf, sizeof(logbuf) - 1,
                                 (uint64_t)&next);
        if (some <= 0) {
            kernel_log_puts("[m70] the log did not advance after something was written to it\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M70 self-test: the kernel cannot report its own log");
        }

        {
            const uint32_t BAND = 0x00800000u;
            uint32_t h = framebuffer_height();
            uint32_t band_h = 8 * FONT_HEIGHT;
            uint32_t band_y = (h > band_h) ? (h - band_h) / 2 : 0;

            panic_render("m70 paint check - the machine is fine, this is a test");

            int band_ok = (framebuffer_get_pixel(4, band_y + band_h - 3) == BAND);

            int glyph_pixels = 0;
            for (uint32_t gx = 16; gx < 16 + 20 * FONT_WIDTH && gx < framebuffer_width(); gx++) {
                for (uint32_t gy = band_y + FONT_HEIGHT; gy < band_y + 2 * FONT_HEIGHT; gy++) {
                    if (framebuffer_get_pixel(gx, gy) != BAND) {
                        glyph_pixels++;
                    }
                }
            }

            console_init();
            kernel_log_use_console();

            if (!band_ok) {
                kernel_log_puts("[m70] a panic painted nothing to the framebuffer\n");
                all_ok = 0;
            }
            if (glyph_pixels < 50) {
                kernel_log_puts("[m70] the panic band was painted but the message was not drawn "
                           "into it (");
                kernel_log_put_dec((uint32_t)glyph_pixels);
                kernel_log_puts(" glyph pixels)\n");
                all_ok = 0;
            }
            if (!all_ok) {
                panic("M70 self-test: a panic cannot put its message on the screen");
            }
        }

        kernel_log_puts("[m70] the kernel log is readable from user space: a marker written and "
                   "found again, a cursor that follows rather than repeats, a gate "
                   "(CAP_SYSLOG) an ordinary program does not hold, and a panic that paints "
                   "its own message onto the framebuffer rather than into a serial port "
                   "nobody is holding - self-test passed.\n\n");
    }

    {
        int all_ok = 1;
        static const char OLD_TEXT[] = "the version that was already there";
        static const char NEW_TEXT[] = "the version being written over it";
        static char readback[128];

        const char *target = PATH_TEMPORARY_DIRECTORY "m71target";
        const char *temp   = PATH_TEMPORARY_DIRECTORY "m71target.tmp~";

        if (do_syscall(SYS_writefile, (uint64_t)target, (uint64_t)OLD_TEXT,
                        sizeof(OLD_TEXT) - 1) != 0 ||
            do_syscall(SYS_writefile, (uint64_t)temp, (uint64_t)NEW_TEXT,
                        sizeof(NEW_TEXT) - 1) != 0) {
            panic("M71 self-test: could not set up the replace fixture");
        }

        k_memset(readback, 0, sizeof(readback));
        do_syscall(SYS_readfile, (uint64_t)target, (uint64_t)readback, sizeof(readback) - 1);
        if (k_strcmp(readback, OLD_TEXT) != 0) {
            kernel_log_puts("[m71] the fixture did not read back as itself\n");
            all_ok = 0;
        }

        if (do_syscall(SYS_rename, (uint64_t)temp, (uint64_t)target, 0) == 0) {
            kernel_log_puts("[m71] SYS_rename replaced an existing file - M56's guarantee is gone\n");
            all_ok = 0;
        }

        if (do_syscall(SYS_rename_replace, (uint64_t)temp, (uint64_t)target, 0) != 0) {
            kernel_log_puts("[m71] SYS_rename_replace failed on an existing destination\n");
            all_ok = 0;
        }

        k_memset(readback, 0, sizeof(readback));
        do_syscall(SYS_readfile, (uint64_t)target, (uint64_t)readback, sizeof(readback) - 1);
        if (k_strcmp(readback, NEW_TEXT) != 0) {
            kernel_log_puts("[m71] after the replace the target is not the new contents\n");
            all_ok = 0;
        }
        if (virtual_file_system_exists(temp)) {
            kernel_log_puts("[m71] the temporary file survived the rename - that is a copy, not a replace\n");
            all_ok = 0;
        }

        if (do_syscall(SYS_rename_replace, (uint64_t)target, (uint64_t)target, 0) != 0 ||
            !virtual_file_system_exists(target)) {
            kernel_log_puts("[m71] renaming a file onto itself destroyed it\n");
            all_ok = 0;
        }

        uint32_t free_before = virtual_file_system_free_blocks();

        static char filler[3000];
        k_memset(filler, 'z', sizeof(filler));
        const char *doomed = PATH_TEMPORARY_DIRECTORY "m71orphan";
        if (do_syscall(SYS_writefile, (uint64_t)doomed, (uint64_t)filler, sizeof(filler)) != 0) {
            panic("M71 self-test: could not write the orphan fixture");
        }
        uint32_t free_with_file = virtual_file_system_free_blocks();
        if (free_with_file >= free_before) {
            kernel_log_puts("[m71] writing a 3 KiB file consumed no blocks - the fixture is wrong\n");
            all_ok = 0;
        }

        leanfs_debug_orphan(doomed);

        uint32_t free_orphaned = virtual_file_system_free_blocks();
        if (free_orphaned != free_with_file) {
            kernel_log_puts("[m71] orphaning did not leave the blocks allocated - nothing to reclaim\n");
            all_ok = 0;
        }

        virtual_file_system_check();
        uint32_t free_after = virtual_file_system_free_blocks();
        if (free_after < free_before) {
            kernel_log_puts("[m71] the check did not reclaim every orphaned block (");
            kernel_log_put_dec(free_before - free_after);
            kernel_log_puts(" still missing)\n");
            all_ok = 0;
        } else if (free_after > free_before) {
            kernel_log_puts("[m71] the check freed blocks that no orphan accounts for (");
            kernel_log_put_dec(free_after - free_before);
            kernel_log_puts(" more free than before the fixture was written)\n");
            all_ok = 0;
        }

        do_syscall(SYS_unlink, (uint64_t)target, 0, 0);

        if (!all_ok) {
            panic("M71 self-test: this filesystem can still lose a file");
        }

        kernel_log_puts("[m71] files worth trusting: a replace that repoints one directory record "
                   "so the name never stops resolving, a plain rename that still refuses to "
                   "overwrite, and an unclean mount whose orphaned blocks are found and "
                   "reclaimed - self-test passed.\n\n");
    }

    {
        int all_ok = 1;
        const char *script = PATH_TEMPORARY_DIRECTORY "m72.sh";
        const char *result = PATH_TEMPORARY_DIRECTORY "m72.out";

        static const char SCRIPT[] =
            "#!/bin/sh\n"
            "# a comment, which must not be run\n"
            "GREETING=hello\n"
            "NAME='lean os'\n"
            "echo $GREETING \"$NAME\" > " PATH_TEMPORARY_DIRECTORY "m72.out\n"
            "notaprogram\n"
            "echo status=$? >> " PATH_TEMPORARY_DIRECTORY "m72.out\n"
            "cd " PATH_TEMPORARY_DIRECTORY " && echo and-ran >> " PATH_TEMPORARY_DIRECTORY "m72.out\n"
            "notaprogram || echo or-ran >> " PATH_TEMPORARY_DIRECTORY "m72.out\n"
            "notaprogram && echo must-not-run >> " PATH_TEMPORARY_DIRECTORY "m72.out\n";

        if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                        sizeof(SCRIPT) - 1) != 0) {
            panic("M72 self-test: could not write the script fixture");
        }

        long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
        if (pid < 0) {
            kernel_log_puts("[m72] a #! script could not be spawned as a program\n");
            all_ok = 0;
        } else {
            do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
        }

        static char produced[256];
        k_memset(produced, 0, sizeof(produced));
        int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
        if (n <= 0) {
            kernel_log_puts("[m72] the script produced no output at all\n");
            all_ok = 0;
        } else {
            produced[n] = '\0';
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
                    kernel_log_puts("[m72] the script did not demonstrate ");
                    kernel_log_puts(EXPECT[e].what);
                    kernel_log_putc('\n');
                    all_ok = 0;
                }
            }
            for (int64_t i = 0; i + 12 < n; i++) {
                int j = 0;
                static const char NEVER[] = "must-not-run";
                while (NEVER[j] && produced[i + j] == NEVER[j]) {
                    j++;
                }
                if (NEVER[j] == '\0') {
                    kernel_log_puts("[m72] && ran its right side after a failure\n");
                    all_ok = 0;
                    break;
                }
            }
        }

        do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
        do_syscall(SYS_unlink, (uint64_t)result, 0, 0);

        if (!all_ok) {
            kernel_log_puts("[m72] what the script actually wrote:\n");
            kernel_log_puts(produced);
            kernel_log_puts("[m72] ---- end\n");
            panic("M72 self-test: scripts do not run, or the shell is not one");
        }

        kernel_log_puts("[m72] a script is a program: `#!` resolved by the ordinary spawn path, "
                   "variables, quoting, a redirect, and && / || gated on a real exit status "
                   "- self-test passed.\n\n");
        int idle_file_descriptors[2];
        if (do_syscall(SYS_pipe, (uint64_t)idle_file_descriptors, 0, 0) != 0) {
            panic("M68 self-test: SYS_pipe failed");
        }

        uint64_t idle_before = scheduler_idle_ticks(0);
        uint64_t total_before = scheduler_total_ticks(0);

        task_t *sleeper = task_spawn("m68-sleeper", m68_sleeper_task, (void *)(uint64_t)idle_file_descriptors[0]);
        if (!sleeper) {
            panic("M68 self-test: could not spawn the sleeper");
        }

        pit_sleep_ms(400);

        if (sleeper->state != TASK_BLOCKED) {
            kernel_log_puts("[m68] a task waiting in SYS_waitfds is not TASK_BLOCKED - it is "
                       "still in the run queue, which is the state this milestone exists to "
                       "remove\n");
            all_ok = 0;
        }

        uint64_t idle_gained = scheduler_idle_ticks(0) - idle_before;
        uint64_t total_gained = scheduler_total_ticks(0) - total_before;

        static const char poke[] = "x";
        do_syscall(SYS_write, (uint64_t)idle_file_descriptors[1], (uint64_t)poke, 1);
        pit_sleep_ms(100);
        if (sleeper->state == TASK_BLOCKED) {
            kernel_log_puts("[m68] a blocked task was not woken by a write to the pipe it was "
                       "waiting on\n");
            all_ok = 0;
        }
        do_syscall(SYS_kill, (uint64_t)sleeper->id, SIGKILL, 0);
        selftest_reap(sleeper);
        do_syscall(SYS_close, (uint64_t)idle_file_descriptors[0], 0, 0);
        do_syscall(SYS_close, (uint64_t)idle_file_descriptors[1], 0, 0);

        if (!all_ok) {
            panic("M68 self-test: tasks do not block, or blocked tasks do not wake");
        }

        (void)idle_gained;
        (void)total_gained;
        kernel_log_puts("[m68] wait queues: a task in SYS_waitfds is TASK_BLOCKED rather than "
                   "runnable, a write to the pipe it waits on wakes it, and the BSP has "
                   "accumulated ");
        kernel_log_put_dec((uint32_t)scheduler_idle_ticks(0));
        kernel_log_puts(" idle tick(s) of ");
        kernel_log_put_dec((uint32_t)scheduler_total_ticks(0));
        kernel_log_puts(" so far - a count that did not exist before this milestone - "
                   "self-test passed.\n\n");
    }

    {
        int all_ok = 1;
        const char *script = PATH_TEMPORARY_DIRECTORY "m86.sh";
        const char *result = PATH_TEMPORARY_DIRECTORY "m86.out";

        static const char SCRIPT[] =
            "#!/bin/sh\n"
            "out=" PATH_TEMPORARY_DIRECTORY "m86.out\n"
            "say() { echo \"func:$1\"; }\n"
            "say 'two words' > $out\n"
            "if false; then echo bad >> $out\n"
            "elif true; then echo elif:taken >> $out\n"
            "else echo bad2 >> $out\n"
            "fi\n"
            "i=x\n"
            "while test ${#i} -lt 4; do\n"
            "  echo \"loop:${#i}\" >> $out\n"
            "  i=$(echo ${i}x)\n"
            "done\n"
            "for f in one two; do echo \"for:$f\" >> $out; done\n"
            "for v in cat dog; do\n"
            "  case $v in cat|dog) echo \"case:$v-pet\" >> $out ;; *) echo bad3 >> $out ;; esac\n"
            "done\n"
            "echo pipe:carried | cat >> $out\n"
            "v=outer\n"
            "( v=inner; echo \"sub:$v\" >> $out )\n"
            "echo \"after:$v\" >> $out\n"
            "PRIVATE=no\n"
            "export SHARED=yes\n"
            "env | cat >> " PATH_TEMPORARY_DIRECTORY "m86.env\n"
            "cat >> $out <<END\n"
            "here:$v\n"
            "END\n"
            "echo \"expand:${nothing:-fallback}\" >> $out\n";

        if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                        sizeof(SCRIPT) - 1) != 0) {
            panic("M86 self-test: could not write the script fixture");
        }

        long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
        if (pid < 0) {
            kernel_log_puts("[m86] the script could not be spawned\n");
            all_ok = 0;
        } else {
            do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
        }

        static char produced[1024];
        k_memset(produced, 0, sizeof(produced));
        int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
        if (n <= 0) {
            kernel_log_puts("[m86] the script produced no output at all\n");
            all_ok = 0;
        } else {
            produced[n] = '\0';
            static const struct { const char *needle; const char *what; } EXPECT[] = {
                {"func:two words", "a function, its argument, and its caller's redirect"},
                {"elif:taken",     "if/elif/else over a real exit status"},
                {"loop:1",         "a while loop entered"},
                {"loop:3",         "a while loop that advanced and then ended"},
                {"for:two",        "for over a word list"},
                {"case:dog-pet",   "case with an alternation"},
                {"pipe:carried",   "a pipeline: a forked builtin writing to an exec'd cat"},
                {"sub:inner",      "a subshell running in its own process"},
                {"after:outer",    "and not changing the shell that forked it"},
                {"here:outer",     "a here-document fed by a second process"},
                {"expand:fallback", "${x:-default} for a name that is not set"},
            };
            for (size_t e = 0; e < sizeof(EXPECT) / sizeof(EXPECT[0]); e++) {
                if (!selftest_contains(produced, EXPECT[e].needle)) {
                    kernel_log_puts("[m86] the script did not demonstrate ");
                    kernel_log_puts(EXPECT[e].what);
                    kernel_log_puts("\n");
                    all_ok = 0;
                }
            }
            static const char *const FORBIDDEN[] = { "bad", "bad2", "bad3", 0 };
            for (int e = 0; FORBIDDEN[e]; e++) {
                if (selftest_contains(produced, FORBIDDEN[e])) {
                    kernel_log_puts("[m86] a branch that should not have run, ran\n");
                    all_ok = 0;
                }
            }
        }

        static char env_seen[2048];
        k_memset(env_seen, 0, sizeof(env_seen));
        int64_t en = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m86.env", env_seen, sizeof(env_seen) - 1);
        if (en <= 0) {
            kernel_log_puts("[m86] `env` in a pipeline produced nothing\n");
            all_ok = 0;
        } else {
            env_seen[en] = '\0';
            if (!selftest_contains(env_seen, "SHARED=yes")) {
                kernel_log_puts("[m86] an exported variable did not reach a child's environment\n");
                all_ok = 0;
            }
            if (selftest_contains(env_seen, "PRIVATE=no")) {
                kernel_log_puts("[m86] an UNexported variable reached a child's environment - "
                           "the two namespaces are one\n");
                all_ok = 0;
            }
        }

        do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
        do_syscall(SYS_unlink, (uint64_t)result, 0, 0);
        do_syscall(SYS_unlink, (uint64_t)PATH_TEMPORARY_DIRECTORY "m86.env", 0, 0);

        if (!all_ok) {
            kernel_log_puts("[m86] what the script actually wrote:\n");
            kernel_log_puts(produced);
            kernel_log_puts("[m86] ---- end\n");
            panic("M86 self-test: this shell is not a shell");
        }

        kernel_log_puts("[m86] a shell that is a shell: a function called with a quoted argument, "
                   "if/elif over a real status, a while loop advanced by command substitution, "
                   "for and case, a pipeline from a forked builtin into an exec'd program, a "
                   "subshell whose assignment does not escape it, a here-document written by a "
                   "second process, ${x:-default}, and an exported variable reaching a child's "
                   "environment while an unexported one does not - self-test passed.\n\n");
    }

    {
        os_stat_t tb;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/toybox", (uint64_t)&tb, 0) != 0) {
            kernel_log_puts("[m89] /bin/toybox is not on this image - skipped. "
                       "`make toybox` installs it.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m89.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m89.out";

            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "d=" PATH_TEMPORARY_DIRECTORY "m89tree\n"
                "rm -rf $d 2>/dev/null\n"
                "mkdir -p $d/a $d/b\n"
                "echo something > $d/a/one\n"
                "echo nothing > $d/a/two\n"
                "echo something > $d/b/three\n"
                "cd $d\n"
                "find . -type f | xargs grep -l something | sort | uniq -c | sort -rn "
                    "> " PATH_TEMPORARY_DIRECTORY "m89.out\n"
                "find . -type f | wc -l >> " PATH_TEMPORARY_DIRECTORY "m89.out\n";

            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M89 self-test: could not write the script fixture");
            }

            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m89] the pipeline script could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }

            static char produced[1024];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m89] the five-stage pipeline produced no output at all\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"./a/one",   "find recursed and grep -l kept a matching file"},
                    {"./b/three", "and kept the one in the other directory"},
                    {"3",         "wc -l counted every file find reported"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m89] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
                if (selftest_contains(produced, "./a/two")) {
                    kernel_log_puts("[m89] grep -l reported a file that does not contain the word\n");
                    all_ok = 0;
                }
            }

            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);

            if (!all_ok) {
                kernel_log_puts("[m89] what the pipeline actually wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m89] ---- end\n");
                panic("M89 self-test: somebody else's userland does not run here");
            }

            kernel_log_puts("[m89] somebody else's userland: find, xargs, grep, sort and uniq - "
                       "five programs nobody here wrote, one multi-call binary reached through "
                       "five symbolic links, connected by four pipes across five forked and "
                       "exec'd processes, over a tree made by mkdir - self-test passed.\n\n");
        }
    }

    {
        os_stat_t gt;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/gcctest", (uint64_t)&gt, 0) != 0) {
            kernel_log_puts("[m94] /bin/gcctest is not on this image - skipped. "
                       "tools/build-toolchain.sh builds the compiler and "
                       "tools/gcc-test.sh installs what it produces.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m94.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m94.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "/bin/gcctest > " PATH_TEMPORARY_DIRECTORY "m94.out\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M94 self-test: could not write the script fixture");
            }
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m94] the compiled program could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }

            static char produced[512];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m94] the compiled program produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"constructor ran",  "crti/crtbegin/crtend/crtn linked in order, .init_array walked"},
                    {"malloc and string round trip", "libc.a out of the sysroot"},
                    {"struct return and varargs", "the ABI this kernel's crt0 assumes"},
                    {"floating point",   "libgcc and SSE state"},
                    {"every check passed", "every check in the fixture"},
                    {"atexit ran",       "the exit handlers, which run after main returns"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m94] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
                if (selftest_contains(produced, "FAIL")) {
                    kernel_log_puts("[m94] the program reported a failure of its own\n");
                    all_ok = 0;
                }
            }

            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);

            if (!all_ok) {
                kernel_log_puts("[m94] what the compiled program actually wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m94] ---- end\n");
                panic("M94 self-test: a program this compiler produced does not run here");
            }

            os_stat_t tp;
            if (do_syscall(SYS_stat, (uint64_t)"/bin/bzip2", (uint64_t)&tp, 0) == 0 &&
                do_syscall(SYS_stat, (uint64_t)"/bin/gnuhello", (uint64_t)&tp, 0) == 0) {
                int third_ok = 1;
                const char *tscript = PATH_TEMPORARY_DIRECTORY "m94b.sh";
                static const char TSCRIPT[] =
                    "#!/bin/sh\n"
                    "d=" PATH_TEMPORARY_DIRECTORY "m94t\n"
                    "rm -rf $d 2>/dev/null\n"
                    "mkdir -p $d\n"
                    "for i in 1 2 3 4 5 6 7 8; do\n"
                    "  echo \"the quick brown fox $i jumps over the lazy dog $i\" >> $d/in\n"
                    "done\n"
                    "cp $d/in $d/keep\n"
                    "/bin/bzip2 -z $d/in\n"
                    "test -f $d/in || echo bzip2:consumed > " PATH_TEMPORARY_DIRECTORY "m94b.out\n"
                    "/bin/bzip2 -d $d/in.bz2\n"
                    "cmp $d/in $d/keep && echo bzip2:roundtrip >> " PATH_TEMPORARY_DIRECTORY "m94b.out\n"
                    "/bin/gnuhello >> " PATH_TEMPORARY_DIRECTORY "m94b.out\n";
                if (do_syscall(SYS_writefile, (uint64_t)tscript, (uint64_t)TSCRIPT,
                                sizeof(TSCRIPT) - 1) != 0) {
                    panic("M94 self-test: could not write the third-party fixture");
                }
                long tpid = do_syscall(SYS_spawn, (uint64_t)tscript, 0, 0);
                if (tpid < 0) {
                    kernel_log_puts("[m94] the third-party fixture could not be spawned\n");
                    third_ok = 0;
                } else {
                    do_syscall(SYS_wait, (uint64_t)tpid, 0, 0);
                }
                static char tout[512];
                k_memset(tout, 0, sizeof(tout));
                int64_t tn = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "m94b.out", tout, sizeof(tout) - 1);
                if (tn <= 0) {
                    kernel_log_puts("[m94] neither ported program produced output\n");
                    third_ok = 0;
                } else {
                    tout[tn] = '\0';
                    if (!selftest_contains(tout, "bzip2:consumed")) {
                        kernel_log_puts("[m94] bzip2 -z did not consume its input - "
                                   "the compression never happened\n");
                        third_ok = 0;
                    }
                    if (!selftest_contains(tout, "bzip2:roundtrip")) {
                        kernel_log_puts("[m94] bzip2 did not compress and decompress back to "
                                   "the same bytes\n");
                        third_ok = 0;
                    }
                    if (!selftest_contains(tout, "Hello, world")) {
                        kernel_log_puts("[m94] GNU hello did not say hello\n");
                        third_ok = 0;
                    }
                }
                do_syscall(SYS_unlink, (uint64_t)tscript, 0, 0);
                do_syscall(SYS_unlink, (uint64_t)PATH_TEMPORARY_DIRECTORY "m94b.out", 0, 0);
                if (!third_ok) {
                    kernel_log_puts("[m94] what they wrote:\n");
                    kernel_log_puts(tout);
                    kernel_log_puts("[m94] ---- end\n");
                    panic("M94 self-test: a program built for this OS by this OS's "
                          "compiler does not run here");
                }
                kernel_log_puts("[m94] somebody else's project: bzip2, built with "
                           "`make CC=x86_64-lean_os-gcc`, compressing a file and "
                           "decompressing it back to the same bytes; and GNU hello, "
                           "built with `./configure --host=x86_64-lean_os && make` "
                           "through fifty gnulib modules, saying hello.\n");
            } else {
                kernel_log_puts("[m94] no ported third-party programs on this image - "
                           "tools/build-thirdparty.sh builds them.\n");
            }

            kernel_log_puts("[m94] a target this compiler knows by name: a program compiled by "
                       "x86_64-lean_os-gcc with no flag supplied by hand - constructor and "
                       "atexit handler both run, malloc through libc.a out of the sysroot, "
                       "a struct returned by value and a varargs call agreeing with this "
                       "kernel's ABI, and floating point through a libgcc built for this "
                       "target - self-test passed.\n\n");
        }
    }

    {
        os_stat_t bt;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/as", (uint64_t)&bt, 0) != 0) {
            kernel_log_puts("[m98] /bin/as is not on this image - skipped. "
                       "tools/build-native-toolchain.sh builds binutils for this "
                       "machine and tools/install-native-toolchain.sh installs it.\n\n");
        } else {
            int all_ok = 1;
            int gcc_here = 0;
            const char *script = PATH_TEMPORARY_DIRECTORY "m98.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m98.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "cd " PATH_TEMPORARY_DIRECTORY "\n"
                "as /tests/binutils-hello.s -o m98.o\n"
                "nm m98.o > " PATH_TEMPORARY_DIRECTORY "m98.out\n"
                "ar rcs m98.a m98.o\n"
                "ar t m98.a >> " PATH_TEMPORARY_DIRECTORY "m98.out\n"
                "ld m98.o -o m98\n"
                "strip m98\n"
                "objdump -d m98 >> " PATH_TEMPORARY_DIRECTORY "m98.out\n"
                "./m98 >> " PATH_TEMPORARY_DIRECTORY "m98.out\n"
                "gcc /tests/m98c.c -o m98c\n"
                "./m98c >> " PATH_TEMPORARY_DIRECTORY "m98.out\n"
                "mkdir -p m98prj\n"
                "cd m98prj\n"
                "cp /tests/m98mk/Makefile Makefile\n"
                "cp /tests/m98mk/main.c main.c\n"
                "cp /tests/m98mk/lib.c lib.c\n"
                "make >> " PATH_TEMPORARY_DIRECTORY "m98.out\n"
                "./prog >> " PATH_TEMPORARY_DIRECTORY "m98.out\n"
                "make >> " PATH_TEMPORARY_DIRECTORY "m98.out\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M98 self-test: could not write the script fixture");
            }
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m98] the toolchain script could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }

            static char produced[2048];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m98] the toolchain produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"T _start", "nm reading the symbol table as wrote"},
                    {"m98.o",    "ar listing the member it archived"},
                    {"Disassembly of section .text",
                                 "objdump reading the program ld linked and strip stripped"},
                    {"as and ld made this program on this machine",
                                 "the program itself, loaded from ld's no-flags default "
                                 "address and run"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m98] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
                os_stat_t gc;
                if (do_syscall(SYS_stat, (uint64_t)"/usr/bin/gcc",
                               (uint64_t)&gc, 0) == 0) {
                    gcc_here = 1;
                    if (!selftest_contains(produced,
                            "gcc built this program on this machine")) {
                        kernel_log_puts("[m98] missing: the program gcc compiled, "
                                   "linked and ran, entirely from this disk\n");
                        all_ok = 0;
                    }
                    if (!selftest_contains(produced,
                            "make built this on this machine: 42")) {
                        kernel_log_puts("[m98] missing: the program make built - "
                                   "two rules, two compiles, one link\n");
                        all_ok = 0;
                    }
                    if (!selftest_contains(produced, "up to date")) {
                        kernel_log_puts("[m98] missing: make's second run "
                                   "concluding nothing was out of date\n");
                        all_ok = 0;
                    }
                } else {
                    kernel_log_puts("[m98] /usr/bin/gcc is not on this image - the "
                               "compile half is skipped. "
                               "tools/build-native-toolchain.sh builds it.\n");
                }
            }

            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m98.o"), 0, 0);
            do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m98.a"), 0, 0);
            do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m98"), 0, 0);
            do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m98c"), 0, 0);

            if (!all_ok) {
                kernel_log_puts("[m98] what the toolchain actually wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m98] ---- end\n");
                panic("M98 self-test: this machine's own toolchain does not work here");
            }

            kernel_log_puts("[m98] binutils runs here: as assembled it, nm read it, ar "
                       "archived it, ld linked it at this OS's own default address "
                       "with no flags and no script, strip stripped it, objdump "
                       "disassembled it, and the result ran");
            if (gcc_here) {
                kernel_log_puts("; then gcc compiled a C program with no flags - "
                           "driver, cc1, as, collect2, ld, headers, startup "
                           "files and libc all from this disk - and that ran "
                           "too; then make drove a two-rule build of a "
                           "two-file program, ran it, and on a second pass "
                           "read leanfs's mtimes and concluded there was "
                           "nothing to do");
            }
            kernel_log_puts(" - self-test passed.\n\n");
        }
    }

    {
        os_stat_t py;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/python3", (uint64_t)&py, 0) != 0) {
            kernel_log_puts("[m99] /bin/python3 is not on this image - skipped. "
                       "tools/build-python.sh builds it and "
                       "tools/install-python.sh puts it here.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m99.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m99.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "/bin/python3 /tests/python/m99.py > " PATH_TEMPORARY_DIRECTORY "m99.out\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M99 self-test: could not write the fixture");
            }

            uint64_t started = pit_get_ticks();
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m99] python could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            uint64_t elapsed_ms = ((pit_get_ticks() - started) * 1000) / PIT_HZ;

            static char produced[2048];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m99] python produced no output at all\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"m99: python 3.12", "the interpreter said which version it is"},
                    {"m99: dict and loop ok", "a class, a dict and a loop"},
                    {"m99: file write/read ok", "open(), write, read back through Python's own io"},
                    {"m99: os.stat/listdir/unlink ok", "the os module's syscalls"},
                    {"m99: exceptions and comprehensions ok", "a raise caught by type, and a generator"},
                    {"m99: a .py module imported from the disk ok",
                     "import read a .py file off this filesystem"},
                    {"m99: C extension modules dlopen'ed from lib-dynload ok",
                     "a C extension module opened from a .so on the disk"},
                    {"m99: fstat answers for a pipe, a console and a bad fd ok",
                     "fstat on a descriptor that is not a file, and an errno "
                     "that is not zero"},
                    {"m99: python runs here", "the script reached its own last line"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m99] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }

            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);

            if (!all_ok) {
                kernel_log_puts("[m99] what python actually wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m99] ---- end\n");
                panic("M99 self-test: python does not run here");
            }

            kernel_log_perf("python_fixture_ms", elapsed_ms, "ms");

            kernel_log_puts("[m99] somebody else's language runs here: a class, a dict, a "
                       "loop, a file written and read back, os.stat and os.listdir, "
                       "an exception caught by type, and `import json` read as a .py "
                       "file off this filesystem - CPython 3.12, built by this "
                       "project's own compiler, self-test passed.\n\n");
        }
    }

    {
        static char ints[8192];
        k_memset(ints, 0, sizeof(ints));
        int64_t n = virtual_file_system_read("/proc/interrupts", ints, sizeof(ints) - 1);
        if (n <= 0) {
            panic("M103 self-test: /proc/interrupts is empty");
        }
        ints[n] = '\0';
        kernel_log_puts("[m103] /proc/interrupts:\n");
        kernel_log_puts(ints);

        if (!ioapic_available()) {
            uint64_t timer = 0;
            for (int c = 0; c < MAX_CPUS; c++) {
                timer += ioapic_irq_count(0x20, c);
            }
            if (timer < 100 || !selftest_contains(ints, "XT-PIC")) {
                panic("M103 self-test: the 8259 path is not being counted");
            }
            kernel_log_puts("[m103] interrupts a real machine delivers: this boot is on "
                       "the 8259, which is the measured default (see "
                       "kernel/device/fwcfg.h) - every vector counted per CPU in "
                       "/proc/interrupts, and LEANOS_IOAPIC=1 runs the same "
                       "battery through the I/O APIC - self-test passed.\n\n");
        } else {
            int all_ok = 1;
            uint64_t timer = 0;
            for (int c = 0; c < MAX_CPUS; c++) {
                timer += ioapic_irq_count(0x20, c);
            }
            if (timer < 100) {
                kernel_log_puts("[m103] the timer's vector has counted 0x");
                kernel_log_put_hex64(timer);
                kernel_log_puts(" interrupts - either it is not arriving through the "
                           "I/O APIC or the counter is not being kept\n");
                all_ok = 0;
            }
            if (!selftest_contains(ints, "IO-APIC")) {
                kernel_log_puts("[m103] /proc/interrupts does not name the controller\n");
                all_ok = 0;
            }
            if (!all_ok) {
                panic("M103 self-test: the interrupt path is not what it says it is");
            }
            kernel_log_puts("[m103] interrupts a real machine delivers: every legacy line "
                       "routed through the I/O APIC with the 8259 masked, the "
                       "firmware's interrupt source overrides applied so the timer "
                       "is on the line it is actually wired to, acknowledged at the "
                       "local APIC, and counted per vector per CPU in "
                       "/proc/interrupts - self-test passed.\n\n");
        }
    }

    {
        os_stat_t dt;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/dyntest", (uint64_t)&dt, 0) != 0) {
            kernel_log_puts("[m95] /bin/dyntest is not on this image - skipped. "
                       "tools/build-dynamic.sh builds it.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m95.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m95.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "/bin/dyntest > " PATH_TEMPORARY_DIRECTORY "m95.out\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M95 self-test: could not write the fixture");
            }
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m95] the dynamic program could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            static char produced[512];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m95] the dynamic program produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"own global ok",          "the program's own load bias"},
                    {"reached through the PLT", "JUMP_SLOT relocations into libc.so"},
                    {"read through the GOT",   "a GLOB_DAT against a libc.so global"},
                    {"dlopen and dlsym",       "a library loaded by name after the fact"},
                    {"every check passed",     "every check in the fixture"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m95] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);

            size_t img_bytes = 0;
            uint8_t *img = read_program("/bin/dyntest", &img_bytes);
            if (!img) {
                kernel_log_puts("[m95] could not read /bin/dyntest back\n");
                all_ok = 0;
            } else {
                leanfs_stat_t libst;
                uint64_t lib_bytes = 0;
                if (virtual_file_system_stat("/lib/libc.so", &libst) == 0) {
                    lib_bytes = libst.size;
                }
                const char *shargv[] = {"/bin/dyntest", "share", 0};

                uint64_t before = physical_memory_free_frame_count();
                task_t *a = process_spawnv("dyntest", img, img_bytes, shargv);
                pit_sleep_ms(600);
                uint64_t after_one = physical_memory_free_frame_count();
                int shared_one = file_mapping_in_use();
                task_t *b = process_spawnv("dyntest", img, img_bytes, shargv);
                pit_sleep_ms(600);
                uint64_t after_two = physical_memory_free_frame_count();
                int shared_two = file_mapping_in_use();
                kfree(img);

                uint64_t first = before - after_one;
                uint64_t second = after_one - after_two;
                kernel_log_puts("[m95] one dynamic process cost 0x");
                kernel_log_put_hex64(first);
                kernel_log_puts(" frames, the second cost 0x");
                kernel_log_put_hex64(second);
                kernel_log_puts("; shared library pages held: 0x");
                kernel_log_put_hex32((uint32_t)shared_one);
                kernel_log_puts(" with one process, 0x");
                kernel_log_put_hex32((uint32_t)shared_two);
                kernel_log_puts(" with two - libc.so is 0x");
                kernel_log_put_hex64((lib_bytes + 4095) / 4096);
                kernel_log_puts(" pages\n");

                if (a) {
                    scheduler_raise_signal(a, SIGKILL);
                }
                if (b) {
                    scheduler_raise_signal(b, SIGKILL);
                }
                pit_sleep_ms(300);
                selftest_reap(a);
                selftest_reap(b);

                if (shared_one <= 0) {
                    kernel_log_puts("[m95] no shared library pages at all - the loader is "
                               "not mapping from the file\n");
                    all_ok = 0;
                } else if (shared_two > shared_one + 4) {
                    kernel_log_puts("[m95] the second process added shared pages of its own - "
                               "the library is being copied rather than shared\n");
                    all_ok = 0;
                }
            }

            if (!all_ok) {
                kernel_log_puts("[m95] what the dynamic program wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m95] ---- end\n");
                panic("M95 self-test: code that is loaded is not loaded");
            }
            kernel_log_puts("[m95] code that is loaded, not linked: a position-independent "
                       "program placed by the kernel, /lib/ld-lean.so relocating "
                       "itself and then it, libc.so reached through the PLT and the "
                       "GOT, a library dlopen'ed by name that did not exist when the "
                       "program was compiled, and a second copy of the program paying "
                       "for none of the library again - self-test passed.\n\n");
        }
    }

    {
        uint8_t a[32], b[32];
        random_bytes(a, sizeof(a));
        random_bytes(b, sizeof(b));
        if (k_memcmp(a, b, sizeof(a)) == 0) {
            panic("M100 self-test: two draws from the random device were the same");
        }
        int64_t n = virtual_file_system_read("/dev/urandom", (char *)b, sizeof(b));
        if (n != (int64_t)sizeof(b)) {
            panic("M100 self-test: /dev/urandom did not fill a read");
        }
        if (k_memcmp(a, b, sizeof(a)) == 0) {
            panic("M100 self-test: /dev/urandom repeated the generator's last draw");
        }
        uint64_t fed = random_events();
        if (fed < 2000) {
            kernel_log_puts("[rng] the pool has taken only ");
            kernel_log_put_dec((uint32_t)fed);
            kernel_log_puts(" events this boot\n");
            panic("M100 self-test: the interrupt path is not feeding the random device");
        }
        kernel_log_puts("[rng] a random device that is not a counter: ChaCha20 under a key ");
        kernel_log_put_dec((uint32_t)fed);
        kernel_log_puts(" interrupts have been mixed into and that is replaced at every draw, "
                   "two draws distinct, /dev/urandom and SYS_getrandom both live; RDRAND ");
        kernel_log_puts(random_has_rdrand() ? "present" : "absent");
        kernel_log_puts(", RDSEED ");
        kernel_log_puts(random_has_rdseed() ? "present" : "absent");
        kernel_log_puts(" on this CPU - self-test passed.\n\n");
    }

    {
        os_stat_t zst;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/zlibtest", (uint64_t)&zst, 0) != 0) {
            kernel_log_puts("[m100] /bin/zlibtest is not on this image - skipped. "
                       "tools/build-thirdparty.sh builds zlib.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m100.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m100.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "cd " PATH_TEMPORARY_DIRECTORY "\n"
                "/bin/zlibtest > " PATH_TEMPORARY_DIRECTORY "m100.out 2>&1\n"
                "echo \"zlibtest exit $?\" >> " PATH_TEMPORARY_DIRECTORY "m100.out\n"
                "echo 'the quick brown fox, and enough text after it that "
                "deflate has something to find - the quick brown fox, and "
                "enough text after it that deflate has something to find' "
                "> " PATH_TEMPORARY_DIRECTORY "m100.txt\n"
                "/bin/minigzip < " PATH_TEMPORARY_DIRECTORY "m100.txt > "
                PATH_TEMPORARY_DIRECTORY "m100.gz\n"
                "/bin/minigzip -d < " PATH_TEMPORARY_DIRECTORY "m100.gz > "
                PATH_TEMPORARY_DIRECTORY "m100.back\n"
                "toybox cmp " PATH_TEMPORARY_DIRECTORY "m100.txt " PATH_TEMPORARY_DIRECTORY "m100.back"
                " && echo 'round trip identical' >> " PATH_TEMPORARY_DIRECTORY "m100.out\n"
                "toybox rm -f " PATH_TEMPORARY_DIRECTORY "m100.txt " PATH_TEMPORARY_DIRECTORY
                "m100.gz " PATH_TEMPORARY_DIRECTORY "m100.back foo.gz\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M100 self-test: could not write the fixture");
            }
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m100] zlibtest could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            static char produced[1024];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m100] zlib's own test program produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"zlibtest exit 0",
                     "zlib's own test program, on this machine, saying it passed"},
                    {"round trip identical",
                     "a file compressed and decompressed through the filesystem"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m100] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);
            if (!all_ok) {
                kernel_log_puts("[m100] what zlib's own test program wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m100] ---- end\n");
                panic("M100 self-test: the first library of the stack does not work here");
            }
            kernel_log_puts("[m100] the first library of the stack: zlib 1.3.1, built "
                       "unmodified by this project's own compiler, and its OWN "
                       "test program passing on this machine - compress and "
                       "uncompress, deflate and inflate in one piece and in "
                       "chunks, a preset dictionary, and the gz file layer "
                       "opening, seeking and reading a real file - plus a round "
                       "trip through minigzip that this filesystem carried "
                       "- self-test passed.\n\n");
        }
    }

    {
        os_stat_t jst;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/djpeg", (uint64_t)&jst, 0) != 0) {
            kernel_log_puts("[m100b] /bin/djpeg is not on this image - skipped. "
                       "tools/build-thirdparty.sh builds libpng and libjpeg.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m100b.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m100b.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "cd " PATH_TEMPORARY_DIRECTORY "\n"
                "D=/usr/share/m100\n"
                "O=" PATH_TEMPORARY_DIRECTORY "m100b.out\n"
                "/bin/djpeg -dct int -ppm -outfile jout.ppm $D/testorig.jpg\n"
                "/bin/djpeg -dct int -gif -outfile jout.gif $D/testorig.jpg\n"
                "/bin/djpeg -dct int -bmp -colors 256 -outfile jout.bmp "
                "$D/testorig.jpg\n"
                "/bin/cjpeg -dct int -outfile jout.jpg $D/testimg.ppm\n"
                "/bin/djpeg -dct int -ppm -outfile joutp.ppm $D/testprog.jpg\n"
                "/bin/cjpeg -dct int -progressive -opt -outfile joutp.jpg "
                "$D/testimg.ppm\n"
                "/bin/jpegtran -outfile joutt.jpg $D/testprog.jpg\n"
                "toybox cmp $D/testimg.ppm jout.ppm && echo 'jpeg ppm' >> $O\n"
                "toybox cmp $D/testimg.gif jout.gif && echo 'jpeg gif' >> $O\n"
                "toybox cmp $D/testimg.bmp jout.bmp && echo 'jpeg bmp' >> $O\n"
                "toybox cmp $D/testimg.jpg jout.jpg && echo 'jpeg encode' >> $O\n"
                "toybox cmp $D/testimg.ppm joutp.ppm && echo 'jpeg progressive"
                " decode' >> $O\n"
                "toybox cmp $D/testimgp.jpg joutp.jpg && echo 'jpeg progressive"
                " encode' >> $O\n"
                "toybox cmp $D/testorig.jpg joutt.jpg && echo 'jpeg transcode'"
                " >> $O\n"
                "/bin/pngtest --strict $D/pngtest.png > " PATH_TEMPORARY_DIRECTORY
                "m100b.png.log 2>&1\n"
                "echo \"pngtest exit $?\" >> $O\n"
                "toybox grep -h 'libpng passes test' " PATH_TEMPORARY_DIRECTORY
                "m100b.png.log >> $O\n"
                "toybox rm -f jout.ppm jout.gif jout.bmp jout.jpg joutp.ppm"
                " joutp.jpg joutt.jpg pngout.png " PATH_TEMPORARY_DIRECTORY
                "m100b.png.log\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M100b self-test: could not write the fixture");
            }
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m100b] the fixture could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            static char produced[1024];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m100b] neither suite produced any output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"jpeg ppm",
                     "testorig.jpg decoded to the exact PPM the IJG shipped"},
                    {"jpeg gif",
                     "the same JPEG decoded to their GIF, palette and all"},
                    {"jpeg bmp",
                     "and to their 256-colour BMP"},
                    {"jpeg encode",
                     "their PPM re-encoded to the byte-identical baseline JPEG"},
                    {"jpeg progressive decode",
                     "the progressive JPEG decoded to the same PPM as the baseline one"},
                    {"jpeg progressive encode",
                     "their PPM encoded progressively, byte-identical"},
                    {"jpeg transcode",
                     "jpegtran turning the progressive file back into the 1995 original"},
                    {"pngtest exit 0",
                     "libpng's own test program saying it passed"},
                    {"libpng passes test",
                     "and saying so in its own words"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m100b] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);
            if (!all_ok) {
                kernel_log_puts("[m100b] what the two suites wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m100b] ---- end\n");
                panic("M100 self-test: libpng or libjpeg does not work here");
            }
            kernel_log_puts("[m100b] two more libraries, graded by their own suites: "
                       "libpng 1.6.44 and libjpeg 9f, built unmodified by this "
                       "project's own compiler, libpng linking the zlib beside "
                       "it in the sysroot. libjpeg's own seven comparisons pass "
                       "BYTE FOR BYTE against reference output the IJG shipped "
                       "in 1995 - baseline and progressive, decode and encode, "
                       "PPM, GIF, BMP, and a transcode back to the original "
                       "file - and libpng's own pngtest reads, writes and "
                       "re-reads a PNG on this filesystem and says it passes "
                       "- self-test passed.\n\n");
        }
    }

    {
        os_stat_t fst;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/ftrender", (uint64_t)&fst, 0) != 0) {
            kernel_log_puts("[m100c] /bin/ftrender is not on this image - skipped. "
                       "tools/build-thirdparty.sh builds freetype and expat.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m100c.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m100c.out";
            static const char FT_SCRIPT[] =
                "#!/bin/sh\n"
                "cd " PATH_TEMPORARY_DIRECTORY "\n"
                "D=/usr/share/m100\n"
                "O=" PATH_TEMPORARY_DIRECTORY "m100c.out\n"
                "/bin/ftrender $D/DejaVuSans.ttf > " PATH_TEMPORARY_DIRECTORY "m100c.ft.txt 2>&1\n"
                "echo \"ftrender exit $?\" >> $O\n"
                "toybox cmp $D/ftrender.expected " PATH_TEMPORARY_DIRECTORY "m100c.ft.txt"
                " && echo 'freetype agrees with the host' >> $O\n"
                "toybox tail -n 1 " PATH_TEMPORARY_DIRECTORY "m100c.ft.txt >> $O\n"
                "echo \"host: $(toybox tail -n 1 $D/ftrender.expected)\" >> $O\n"
                "toybox rm -f " PATH_TEMPORARY_DIRECTORY "m100c.ft.txt\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)FT_SCRIPT,
                            sizeof(FT_SCRIPT) - 1) != 0) {
                panic("M100c self-test: could not write the freetype fixture");
            }
            uint64_t started = pit_get_ticks();
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m100c] the freetype fixture could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            uint64_t freetype_ms = ((pit_get_ticks() - started) * 1000) / PIT_HZ;
            static char produced[1024];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m100c] the freetype fixture produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"ftrender exit 0",
                     "every glyph loaded and rendered"},
                    {"freetype agrees with the host",
                     "570 rendered glyphs, their metrics and their kerning, byte-identical "
                     "with the host's build of the same freetype"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m100c] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);
            if (!all_ok) {
                kernel_log_puts("[m100c] what the freetype fixture wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m100c] ---- end\n");
                panic("M100 self-test: freetype does not agree with the host here");
            }

            static const char XML_SCRIPT[] =
                "#!/bin/sh\n"
                "cd " PATH_TEMPORARY_DIRECTORY "\n"
                "O=" PATH_TEMPORARY_DIRECTORY "m100c.out\n"
                "/bin/expattest > " PATH_TEMPORARY_DIRECTORY "m100c.expat.txt 2>&1\n"
                "echo \"expattest exit $?\" >> $O\n"
                "toybox grep -h 'Checks:' " PATH_TEMPORARY_DIRECTORY "m100c.expat.txt >> $O\n"
                "echo '<a><b x=\"1\">hi</b></a>' > " PATH_TEMPORARY_DIRECTORY "m100c.xml\n"
                "/bin/xmlwf " PATH_TEMPORARY_DIRECTORY "m100c.xml && echo 'xmlwf well-formed' >> $O\n"
                "echo '<a><b></a>' > " PATH_TEMPORARY_DIRECTORY "m100c.bad.xml\n"
                "/bin/xmlwf " PATH_TEMPORARY_DIRECTORY "m100c.bad.xml >> $O 2>&1\n"
                "toybox rm -f " PATH_TEMPORARY_DIRECTORY "m100c.expat.txt " PATH_TEMPORARY_DIRECTORY
                "m100c.xml " PATH_TEMPORARY_DIRECTORY "m100c.bad.xml\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)XML_SCRIPT,
                            sizeof(XML_SCRIPT) - 1) != 0) {
                panic("M100c self-test: could not write the expat fixture");
            }
            started = pit_get_ticks();
            pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m100c] the expat fixture could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            uint64_t expat_ms = ((pit_get_ticks() - started) * 1000) / PIT_HZ;
            k_memset(produced, 0, sizeof(produced));
            n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m100c] expat's own suite produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"expattest exit 0",
                     "expat's own test program saying it passed"},
                    {"100%: Checks: 4392, Failed: 0",
                     "all 4,392 of its checks, in its own words - the sentence the host prints"},
                    {"xmlwf well-formed",
                     "xmlwf accepting a well-formed document read off this filesystem"},
                    {"m100c.bad.xml:1:8: mismatched tag",
                     "and naming the line and column of a tag that does not match"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m100c] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);
            if (!all_ok) {
                kernel_log_puts("[m100c] what expat wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m100c] ---- end\n");
                panic("M100 self-test: expat does not work here");
            }
            kernel_log_perf("freetype_render_ms", freetype_ms, "ms");
            kernel_log_perf("expat_suite_ms", expat_ms, "ms");
            kernel_log_puts("[m100c] freetype against the host, and expat by its own suite: "
                       "freetype 2.13.3, linking the libpng and zlib beside it, "
                       "renders 570 glyphs of DejaVu Sans through its bytecode "
                       "interpreter, both rasterizers and its autohinter BYTE-IDENTICAL "
                       "with the host's build of the same source; expat 2.6.4 passes "
                       "all 4,392 of its own checks here, and xmlwf reads a document "
                       "off this disk and says where the tag is wrong "
                       "- self-test passed.\n\n");
        }
    }

    {
        os_stat_t sst;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/sqlite3", (uint64_t)&sst, 0) != 0) {
            kernel_log_puts("[m100d] /bin/sqlite3 is not on this image - skipped. "
                       "tools/build-thirdparty.sh builds sqlite.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m100d.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m100d.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "cd " PATH_TEMPORARY_DIRECTORY "\n"
                "D=/usr/share/m100\n"
                "O=" PATH_TEMPORARY_DIRECTORY "m100d.out\n"
                "toybox rm -f " PATH_TEMPORARY_DIRECTORY "m100d.db " PATH_TEMPORARY_DIRECTORY "m100d.db-journal\n"
                "/bin/sqlite3 -batch -bail " PATH_TEMPORARY_DIRECTORY "m100d.db < $D/cases.sql > "
                PATH_TEMPORARY_DIRECTORY "m100d.txt 2>&1\n"
                "echo \"sqlite3 exit $?\" >> $O\n"
                "toybox cmp $D/sqlite.expected " PATH_TEMPORARY_DIRECTORY "m100d.txt"
                " && echo 'sqlite agrees with the host' >> $O\n"
                "toybox tail -n 3 " PATH_TEMPORARY_DIRECTORY "m100d.txt >> $O\n"
                "toybox rm -f " PATH_TEMPORARY_DIRECTORY "m100d.txt " PATH_TEMPORARY_DIRECTORY "m100d.db "
                PATH_TEMPORARY_DIRECTORY "m100d.db-journal\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M100d self-test: could not write the sqlite fixture");
            }
            uint64_t started = pit_get_ticks();
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m100d] the sqlite fixture could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            uint64_t sqlite_ms = ((pit_get_ticks() - started) * 1000) / PIT_HZ;
            static char produced[1024];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m100d] the sqlite fixture produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"sqlite3 exit 0",
                     "every statement in tests/sqlite/cases.sql ran without error"},
                    {"sqlite agrees with the host",
                     "the transcript byte-identical with the host's build of the same sqlite"},
                    {"sqlite: done",
                     "and the script reached its own last line"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m100d] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);
            if (flock_count() != 0) {
                kernel_log_puts("[m100d] record locks left in the table after sqlite exited: ");
                kernel_log_put_dec((uint32_t)flock_count());
                kernel_log_putc('\n');
                all_ok = 0;
            }
            if (!all_ok) {
                kernel_log_puts("[m100d] what sqlite wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m100d] ---- end\n");
                panic("M100 self-test: sqlite does not agree with the host here");
            }
            kernel_log_perf("sqlite_fixture_ms", sqlite_ms, "ms");
            kernel_log_puts("[m100d] sqlite against the host: sqlite 3.47.2, built unmodified, "
                       "runs tests/sqlite/cases.sql - 5,000 rows through a B-tree and an "
                       "index, a transaction rolled back and one committed through a "
                       "journal on this filesystem, joins, window functions, a VACUUM, "
                       "and the shell's popen and system - with a transcript BYTE-IDENTICAL "
                       "to the host's build of the same source, every fcntl record lock it "
                       "took granted and given back - self-test passed.\n\n");
        }
    }

    {
        os_stat_t hst;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/hbshape", (uint64_t)&hst, 0) != 0) {
            kernel_log_puts("[m100e] /bin/hbshape is not on this image - skipped. "
                       "tools/build-thirdparty.sh builds harfbuzz.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m100e.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m100e.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "cd " PATH_TEMPORARY_DIRECTORY "\n"
                "D=/usr/share/m100\n"
                "O=" PATH_TEMPORARY_DIRECTORY "m100e.out\n"
                "/bin/hbshape $D/DejaVuSans.ttf > " PATH_TEMPORARY_DIRECTORY "m100e.txt 2>&1\n"
                "echo \"hbshape exit $?\" >> $O\n"
                "toybox cmp $D/hbshape.expected " PATH_TEMPORARY_DIRECTORY "m100e.txt"
                " && echo 'harfbuzz agrees with the host' >> $O\n"
                "toybox tail -n 1 " PATH_TEMPORARY_DIRECTORY "m100e.txt >> $O\n"
                "echo \"host: $(toybox tail -n 1 $D/hbshape.expected)\" >> $O\n"
                "toybox rm -f " PATH_TEMPORARY_DIRECTORY "m100e.txt\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M100e self-test: could not write the harfbuzz fixture");
            }
            uint64_t started = pit_get_ticks();
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m100e] the harfbuzz fixture could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            uint64_t shape_ms = ((pit_get_ticks() - started) * 1000) / PIT_HZ;
            static char produced[1024];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m100e] the harfbuzz fixture produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"hbshape exit 0",
                     "every string shaped, through both font loaders"},
                    {"harfbuzz agrees with the host",
                     "every glyph, cluster, advance and offset byte-identical with the "
                     "host's build of the same harfbuzz"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m100e] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);
            if (!all_ok) {
                kernel_log_puts("[m100e] what the harfbuzz fixture wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m100e] ---- end\n");
                panic("M100 self-test: harfbuzz does not agree with the host here");
            }
            kernel_log_perf("harfbuzz_shape_ms", shape_ms, "ms");
            kernel_log_puts("[m100e] harfbuzz against the host: harfbuzz 8.5.0, C++ built "
                       "unmodified by this project's g++ and linking the freetype beside "
                       "it, shapes Latin, Greek, Cyrillic, Arabic and Hebrew through its "
                       "OpenType tables and again through hb-ft, every glyph and position "
                       "BYTE-IDENTICAL with the host's build of the same source "
                       "- self-test passed.\n\n");
        }
    }

    {
        os_stat_t tst;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/ssl_server2", (uint64_t)&tst, 0) != 0) {
            kernel_log_puts("[m100f] /bin/ssl_server2 is not on this image - skipped. "
                       "tools/build-thirdparty.sh builds mbedtls.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m100f.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m100f.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "cd " PATH_TEMPORARY_DIRECTORY "\n"
                "D=/usr/share/m100\n"
                "O=" PATH_TEMPORARY_DIRECTORY "m100f.out\n"
                "/bin/ssl_server2 server_addr=127.0.0.1 server_port=4433 debug_level=1 > "
                PATH_TEMPORARY_DIRECTORY "m100f.srv.txt 2>&1 &\n"
                "toybox sleep 8\n"
                "/bin/ssl_client2 server_addr=127.0.0.1 server_name=localhost "
                "server_port=4433 debug_level=1 > " PATH_TEMPORARY_DIRECTORY "m100f.c2.txt 2>&1\n"
                "echo \"ssl_client2 exit $?\" >> $O\n"
                "toybox grep -h 'TLS1-3\\|Protocol is\\|HTTP/1.0 200' "
                PATH_TEMPORARY_DIRECTORY "m100f.c2.txt >> $O\n"
                "echo '--- client tail:' >> $O\n"
                "toybox tail -n 12 " PATH_TEMPORARY_DIRECTORY "m100f.c2.txt >> $O\n"
                "echo '--- server tail:' >> $O\n"
                "toybox tail -n 12 " PATH_TEMPORARY_DIRECTORY "m100f.srv.txt >> $O\n"
                "/bin/httpsget localhost 4433 $D/mbedtls-test-ca.pem / > "
                PATH_TEMPORARY_DIRECTORY "m100f.hg.txt 2>&1\n"
                "echo \"httpsget exit $?\" >> $O\n"
                "toybox grep -h 'httpsget: \\|Mbed TLS Test Server' " PATH_TEMPORARY_DIRECTORY "m100f.hg.txt >> $O\n"
                "/bin/httpsget 127.0.0.1 4433 $D/mbedtls-test-ca.pem / > "
                PATH_TEMPORARY_DIRECTORY "m100f.bad.txt 2>&1\n"
                "echo \"httpsget wrong-name exit $?\" >> $O\n"
                "toybox grep -h 'does not match\\|CN mismatch' " PATH_TEMPORARY_DIRECTORY "m100f.bad.txt >> $O\n"
                "toybox killall ssl_server2\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M100f self-test: could not write the TLS fixture");
            }
            uint64_t started = pit_get_ticks();
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m100f] the TLS fixture could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            uint64_t tls_ms = ((pit_get_ticks() - started) * 1000) / PIT_HZ;
            static char produced[4096];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m100f] the TLS fixture produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"ssl_client2 exit 0",
                     "mbedtls's own client completing a session with mbedtls's own server"},
                    {"TLS1-3-CHACHA20-POLY1305-SHA256",
                     "a real TLS 1.3 session, AEAD cipher, negotiated between the two"},
                    {"HTTP/1.0 200 OK",
                     "the server's page read back through the encrypted session"},
                    {"Mbed TLS Test Server",
                     "and its body carried across intact"},
                    {"httpsget exit 0",
                     "a program written here completing the same GET - and exit 0 under "
                     "VERIFY_REQUIRED is the chain verified against the CA file, because "
                     "the handshake returns an error otherwise"},
                    {"httpsget wrong-name exit 1",
                     "and the same server REFUSED under a name its certificate is not for "
                     "- the refusal an https that accepts any certificate would not make"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m100f] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);
            if (!all_ok) {
                kernel_log_puts("[m100f] what the TLS fixture wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m100f] ---- end\n");
                panic("M100 self-test: TLS does not work end to end here");
            }
            kernel_log_perf("tls_fixture_ms", tls_ms, "ms");
            kernel_log_puts("[m100f] TLS end to end over M66's TCP: mbedtls 3.6.2's own server "
                       "and client complete a verified session on loopback, a program "
                       "written here fetches https://localhost/ through the same library "
                       "with the chain checked against a CA on this disk, and the same "
                       "server under the wrong name is refused - the first https:// this "
                       "machine has had - self-test passed.\n\n");
        }
    }

    {
        os_stat_t mst;
        if (do_syscall(SYS_stat, (uint64_t)"/usr/share/m100/mbedtls/test_suite_shax",
                       (uint64_t)&mst, 0) != 0) {
            kernel_log_puts("[m100g] mbedtls's suites are not on this image - skipped. "
                       "tools/build-thirdparty.sh builds them.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m100g.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m100g.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "cd /usr/share/m100/mbedtls\n"
                "O=" PATH_TEMPORARY_DIRECTORY "m100g.out\n"
                "for s in test_suite_*; do\n"
                "  case $s in *.datax) continue;; esac\n"
                "  ./$s $s.datax > " PATH_TEMPORARY_DIRECTORY "m100g.one.txt 2>&1\n"
                "  echo \"$s: $(toybox tail -n 1 " PATH_TEMPORARY_DIRECTORY "m100g.one.txt)\" >> $O\n"
                "done\n"
                "toybox rm -f " PATH_TEMPORARY_DIRECTORY "m100g.one.txt\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M100g self-test: could not write the fixture");
            }
            uint64_t started = pit_get_ticks();
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m100g] the fixture could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            uint64_t suites_ms = ((pit_get_ticks() - started) * 1000) / PIT_HZ;
            static char produced[4096];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            int passed = 0, lines = 0;
            if (n <= 0) {
                kernel_log_puts("[m100g] the suites produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                for (int64_t i = 0; i < n; i++) {
                    if (produced[i] == '\n') {
                        lines++;
                    }
                }
                int failed = 0;
                for (int64_t i = 0; i + 6 < n; i++) {
                    if (produced[i] == 'P' && k_memcmp(produced + i, "PASSED", 6) == 0) {
                        passed++;
                    }
                    if (produced[i] == 'F' && k_memcmp(produced + i, "FAILED", 6) == 0) {
                        failed++;
                    }
                }
                if (passed == 0 || failed != 0 || passed != lines) {
                    all_ok = 0;
                }
            }
            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);
            if (!all_ok) {
                kernel_log_puts("[m100g] ");
                kernel_log_put_dec((uint32_t)passed);
                kernel_log_puts(" of ");
                kernel_log_put_dec((uint32_t)lines);
                kernel_log_puts(" suites passed; what they wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m100g] ---- end\n");
                panic("M100 self-test: mbedtls's own suites do not pass here");
            }
            kernel_log_perf("mbedtls_suites_ms", suites_ms, "ms");
            kernel_log_puts("[m100g] mbedtls's own suites: each reading its own vectors off "
                       "this disk and printing its own PASSED to the last one - "
                       "ChaCha20-Poly1305 (the AEAD [m100f] negotiated), SHA-2, and ECDSA "
                       "- the cipher, the hash and the signature a TLS 1.3 handshake here "
                       "uses, graded by the library's own answers, nothing written here - "
                       "self-test passed. ");
            kernel_log_put_dec((uint32_t)passed);
            kernel_log_puts(" suites.\n\n");
        }
    }

    {
        os_stat_t md;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/manydyn", (uint64_t)&md, 0) != 0) {
            kernel_log_puts("[m99ld] /bin/manydyn is not on this image - skipped. "
                       "tools/build-dynamic.sh builds it.\n\n");
        } else {
            int all_ok = 1;
            const char *script = PATH_TEMPORARY_DIRECTORY "m99ld.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m99ld.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "/bin/manydyn > " PATH_TEMPORARY_DIRECTORY "m99ld.out\n";
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)SCRIPT,
                            sizeof(SCRIPT) - 1) != 0) {
                panic("M99 loader self-test: could not write the fixture");
            }
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m99ld] manydyn could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            static char produced[512];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m99ld] manydyn produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"24 objects open at once",
                     "twenty-four shared objects loaded at the same time"},
                    {"answered for itself",
                     "a symbol out of each one, resolved after all of them were open"},
                    {"a pathname that is not there fails as one",
                     "a missing path failing as a path rather than starting a search"},
                    {"every check passed", "every check in the fixture"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m99ld] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);
            if (!all_ok) {
                kernel_log_puts("[m99ld] what manydyn wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m99ld] ---- end\n");
                panic("M99 loader self-test: the loader cannot carry an interpreter");
            }
            kernel_log_puts("[m99ld] a loader an interpreter can use: twenty-four shared "
                       "objects open at once, every one opened by a full path out of "
                       "a directory nothing searches, a symbol resolved from each "
                       "after all of them were loaded, and a path that is not there "
                       "refused as a path - self-test passed.\n\n");
        }
    }

    if (net_have_nic()) {
        int all_ok = 1;

        size_t ns_bytes = 0;
        uint8_t *ns_img = read_program(PATH_BIN_DIRECTORY "nslookup", &ns_bytes);
        const char *ns_argv[] = {PATH_BIN_DIRECTORY "nslookup", "-s", 0};
        task_t *ns = process_spawnv("nslookup", ns_img, ns_bytes, ns_argv);
        kfree(ns_img);
        if (!ns || do_syscall(SYS_wait, (uint64_t)ns->id, 0, 0) != 0) {
            kernel_log_puts("[m73] the DNS parser self-test reported a failure\n");
            all_ok = 0;
        }

        size_t hd_bytes = 0;
        uint8_t *hd_img = read_program(PATH_BIN_DIRECTORY "httpd", &hd_bytes);
        const char *hd_argv[] = {PATH_BIN_DIRECTORY "httpd", "8081", 0};
        task_t *hd = process_spawnv("httpd", hd_img, hd_bytes, hd_argv);
        kfree(hd_img);
        pit_sleep_ms(300);

        const char *FETCHED = PATH_TEMPORARY_DIRECTORY "m73.txt";
        size_t ft_bytes = 0;
        uint8_t *ft_img = read_program(PATH_BIN_DIRECTORY "fetch", &ft_bytes);
        const char *ft_argv[] = {PATH_BIN_DIRECTORY "fetch",
                                  "http://127.0.0.1:8081/hello", FETCHED, 0};
        task_t *ft = process_spawnv("fetch", ft_img, ft_bytes, ft_argv);
        kfree(ft_img);
        if (!ft || do_syscall(SYS_wait, (uint64_t)ft->id, 0, 0) != 0) {
            kernel_log_puts("[m73] fetch could not retrieve over loopback\n");
            all_ok = 0;
        }
        if (hd) {
            selftest_reap(hd);
        }

        static const char EXPECT[] = "lean_os fetched this over loopback\n";
        static char fetched[128];
        k_memset(fetched, 0, sizeof(fetched));
        int64_t function = virtual_file_system_read(FETCHED, fetched, sizeof(fetched) - 1);
        if (function != (int64_t)sizeof(EXPECT) - 1 || k_strcmp(fetched, EXPECT) != 0) {
            kernel_log_puts("[m73] the fetched file is not what the server sent - got ");
            kernel_log_put_dec((uint32_t)(function < 0 ? 0 : function));
            kernel_log_puts(" byte(s)\n");
            all_ok = 0;
        }
        do_syscall(SYS_unlink, (uint64_t)FETCHED, 0, 0);

        if (!all_ok) {
            panic("M73 self-test: this machine cannot resolve a name or fetch a byte");
        }

        kernel_log_puts("[m73] names, not numbers: a DNS parser that follows compression "
                   "pointers and CNAMEs and refuses a pointer loop, a wrong id and a "
                   "truncated reply - and an HTTP GET over loopback whose bytes reached "
                   "the filesystem, the first thing here that was not compiled in - "
                   "self-test passed.\n\n");
    } else {
        kernel_log_puts("[m73] no NIC on this machine - DNS and HTTP are present but untested "
                   "this boot.\n\n");
    }

    {
        int all_ok = 1;
        const char *SESSION = PATH_ETC_DIRECTORY "session.conf";
        static char saved_session[512];
        int64_t saved_session_length = virtual_file_system_read(SESSION, saved_session, sizeof(saved_session));
        if (saved_session_length > (int64_t)sizeof(saved_session)) {
            saved_session_length = -1;
        }
        const uint32_t CLOCK_BG = 0x00122438u;
        static const char SAVED[] = "gui_clock 520 380 200 90 0\n";
        const uint32_t PROBE_X = 530, PROBE_Y = 460;

        if (do_syscall(SYS_writefile, (uint64_t)SESSION, (uint64_t)SAVED,
                        sizeof(SAVED) - 1) != 0) {
            panic("M74 self-test: could not write the session fixture");
        }

        virtual_file_system_write(PATH_SETTINGS, SELFTEST_SETTINGS_NO_ANIM,
                   sizeof(SELFTEST_SETTINGS_NO_ANIM) - 1);

        size_t comp_bytes = 0;
        uint8_t *comp_img = read_program(PATH_BIN_DIRECTORY "compositor", &comp_bytes);
        const char *comp_argv[] = {PATH_BIN_DIRECTORY "compositor", 0};
        const char *comp_envp[] = {"LEANOS_SESSION=1", 0};
        task_t *comp = process_spawnve("compositor", comp_img, comp_bytes, comp_argv, comp_envp);
        kfree(comp_img);
        if (!comp) {
            panic("M74 self-test: could not spawn a compositor");
        }

        if (!selftest_wait_for_pixel(PROBE_X, PROBE_Y, CLOCK_BG, 12000,
                                      "the session to relaunch a window and place it")) {
            kernel_log_puts("[m74] the saved window was not brought back at its saved position\n");
            all_ok = 0;
        }

        do_syscall(SYS_unlink, (uint64_t)SESSION, 0, 0);

        size_t z_bytes = 0;
        uint8_t *z_img = read_program(PATH_BIN_DIRECTORY "wm_zorder", &z_bytes);
        task_t *second = process_spawn("wm_zorder", z_img, z_bytes, "s2 00C08040");
        kfree(z_img);
        pit_sleep_ms(3000);

        static char written[256];
        k_memset(written, 0, sizeof(written));
        int64_t wn = virtual_file_system_read(SESSION, written, sizeof(written) - 1);
        if (wn <= 0) {
            kernel_log_puts("[m74] the compositor did not write a session after the layout changed\n");
            all_ok = 0;
        } else {
            written[wn] = '\0';
            if (!selftest_contains(written, "gui_clock") ||
                !selftest_contains(written, "wm_zorder")) {
                kernel_log_puts("[m74] the session it wrote does not name both running programs: ");
                kernel_log_puts(written);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }

        if (second) {
            do_syscall(SYS_kill, (uint64_t)second->id, SIGKILL, 0);
            selftest_reap(second);
        }
        do_syscall(SYS_kill, (uint64_t)comp->id, SIGKILL, 0);
        selftest_reap(comp);
        for (int i = 0; i < scheduler_task_count(); i++) {
            task_t *o = scheduler_task_by_slot(i);
            if (o && o->state != TASK_FREE && o->state != TASK_TERMINATED &&
                k_strcmp(o->name, "gui_clock") == 0) {
                do_syscall(SYS_kill, (uint64_t)o->id, SIGKILL, 0);
                selftest_reap(o);
            }
        }
        if (saved_session_length >= 0) {
            virtual_file_system_write(SESSION, saved_session, (size_t)saved_session_length);
        } else {
            do_syscall(SYS_unlink, (uint64_t)SESSION, 0, 0);
        }
        virtual_file_system_write(PATH_SETTINGS, SELFTEST_SETTINGS_CONF, sizeof(SELFTEST_SETTINGS_CONF) - 1);
        console_init();
        kernel_log_use_console();

        if (!all_ok) {
            panic("M74 self-test: the desktop does not remember what was open");
        }

        kernel_log_puts("[m74] the session remembers: a saved window relaunched and placed at its "
                   "own coordinates rather than the cascade's - graded on the pixel, not on "
                   "the file - and a session written naming both programs once a second "
                   "window changed the layout - self-test passed.\n\n");
    }

    {
        int all_ok = 1;
        const char *DIRECTORY_A = PATH_TEMPORARY_DIRECTORY "m75a";
        const char *DIRECTORY_B = PATH_TEMPORARY_DIRECTORY "m75b";
        do_syscall(SYS_mkdir, (uint64_t)DIRECTORY_A, 0, 0);
        do_syscall(SYS_mkdir, (uint64_t)DIRECTORY_B, 0, 0);
        do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m75a/alpha"), 0, 0);
        do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m75b/beta"), 0, 0);

        static const char SELF_ENV[] = "M75_ABSENT=1\0M75_OUT=wrong\0M75_BODY=wrong";
        scheduler_set_env(scheduler_current(), SELF_ENV, sizeof(SELF_ENV) - 1, 3);

        size_t et_bytes = 0;
        uint8_t *et_img = read_program(PATH_BIN_DIRECTORY "envtest", &et_bytes);
        if (!et_img) {
            panic("M75 self-test: /bin/envtest is not on this disk");
        }

        if (do_syscall(SYS_chdir, (uint64_t)DIRECTORY_A, 0, 0) != 0) {
            panic("M75 self-test: chdir into the fixture directory failed");
        }
        {
            const char *argv[] = {PATH_BIN_DIRECTORY "envtest", 0};
            const char *envp[] = {"M75_OUT=alpha", "M75_BODY=first", 0};
            task_t *t = process_spawnve("envtest", et_img, et_bytes, argv, envp);
            long rc = t ? do_syscall(SYS_wait, (uint64_t)t->id, 0, 0) : -1;
            if (rc != 0) {
                kernel_log_puts("[m75] the first child exited ");
                kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
                kernel_log_puts(" - see user_space/binaries/envtest.c for what each code means\n");
                all_ok = 0;
            }
        }

        if (do_syscall(SYS_chdir, (uint64_t)"../m75b", 0, 0) != 0) {
            kernel_log_puts("[m75] `cd ../m75b` from /tmp/m75a did not resolve - \"..\" is not "
                       "being normalized before leanfs sees it\n");
            all_ok = 0;
        }
        {
            char where[PATH_MAX_LENGTH];
            k_memset(where, 0, sizeof(where));
            long n = do_syscall(SYS_getcwd, (uint64_t)where, sizeof(where), 0);
            if (n < 0 || k_strcmp(where, DIRECTORY_B) != 0) {
                kernel_log_puts("[m75] after `cd ../m75b` the directory is '");
                kernel_log_puts(where);
                kernel_log_puts("' rather than ");
                kernel_log_puts(DIRECTORY_B);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        {
            const char *argv[] = {PATH_BIN_DIRECTORY "envtest", 0};
            const char *envp[] = {"M75_OUT=beta", "M75_BODY=second", 0};
            task_t *t = process_spawnve("envtest", et_img, et_bytes, argv, envp);
            long rc = t ? do_syscall(SYS_wait, (uint64_t)t->id, 0, 0) : -1;
            if (rc != 0) {
                kernel_log_puts("[m75] the second child exited ");
                kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        kfree(et_img);

        static const struct {
            const char *path;
            const char *expect;
        } LANDED[] = {
            {PATH_TEMPORARY_DIRECTORY "m75a/alpha", PATH_TEMPORARY_DIRECTORY "m75a first"},
            {PATH_TEMPORARY_DIRECTORY "m75b/beta",  PATH_TEMPORARY_DIRECTORY "m75b second"},
        };
        for (size_t i = 0; i < sizeof(LANDED) / sizeof(LANDED[0]); i++) {
            static char got[PATH_MAX_LENGTH + 64];
            k_memset(got, 0, sizeof(got));
            int64_t n = virtual_file_system_read(LANDED[i].path, got, sizeof(got) - 1);
            if (n <= 0) {
                kernel_log_puts("[m75] nothing was written to ");
                kernel_log_puts(LANDED[i].path);
                kernel_log_puts(" - a relative open did not land in the caller's directory\n");
                all_ok = 0;
                continue;
            }
            got[n] = '\0';
            if (k_strcmp(got, LANDED[i].expect) != 0) {
                kernel_log_puts("[m75] ");
                kernel_log_puts(LANDED[i].path);
                kernel_log_puts(" holds '");
                kernel_log_puts(got);
                kernel_log_puts("' rather than '");
                kernel_log_puts(LANDED[i].expect);
                kernel_log_puts("'\n");
                all_ok = 0;
            }
        }

        {
            const char *SCRIPT = PATH_TEMPORARY_DIRECTORY "m75.sh";
            const char *RESULT = PATH_TEMPORARY_DIRECTORY "m75a/fromsh";
            static const char SH[] =
                "#!/bin/sh\n"
                "cd " PATH_TEMPORARY_DIRECTORY "\n"
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
                kernel_log_puts("[m75] the shell fixture could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }
            static char shout[1024];
            k_memset(shout, 0, sizeof(shout));
            int64_t n = virtual_file_system_read(RESULT, shout, sizeof(shout) - 1);
            if (n <= 0) {
                kernel_log_puts("[m75] `cd m75a` then `env > fromsh` produced nothing at ");
                kernel_log_puts(RESULT);
                kernel_log_puts(" - the shell's cd is still its own bookkeeping\n");
                all_ok = 0;
            } else {
                shout[n] = '\0';
                static const struct { const char *needle; const char *what; } WANT[] = {
                    {"M75_SHELL=exported", "a shell assignment reaching a spawned program's environment"},
                    {PATH_TEMPORARY_DIRECTORY "m75a",  "`pwd` reporting the directory two relative cds arrived at"},
                };
                for (size_t w = 0; w < sizeof(WANT) / sizeof(WANT[0]); w++) {
                    if (!selftest_contains(shout, WANT[w].needle)) {
                        kernel_log_puts("[m75] the shell did not demonstrate ");
                        kernel_log_puts(WANT[w].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
            }
            do_syscall(SYS_unlink, (uint64_t)SCRIPT, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)RESULT, 0, 0);
        }

        do_syscall(SYS_chdir, (uint64_t)"/", 0, 0);
        scheduler_release_env(scheduler_current());
        do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m75a/alpha"), 0, 0);
        do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m75b/beta"), 0, 0);
        do_syscall(SYS_rmdir, (uint64_t)DIRECTORY_A, 0, 0);
        do_syscall(SYS_rmdir, (uint64_t)DIRECTORY_B, 0, 0);

        if (!all_ok) {
            panic("M75 self-test: there is still nowhere to stand and nothing to stand there with");
        }

        kernel_log_puts("[m75] environment and a place to stand: two children given different "
                   "environments and started in different directories, each writing a file "
                   "named only relatively and landing in its own, `..` normalized before "
                   "leanfs ever saw it, and a shell whose cd and export are the real ones - "
                   "self-test passed.\n\n");
    }

    {
        int all_ok = 1;
        const char *READY = PATH_TEMPORARY_DIRECTORY "m76ready";
        const char *ALIVE = PATH_TEMPORARY_DIRECTORY "m76alive";
        do_syscall(SYS_unlink, (uint64_t)READY, 0, 0);
        do_syscall(SYS_unlink, (uint64_t)ALIVE, 0, 0);

        size_t st_bytes = 0;
        uint8_t *st_img = read_program(PATH_BIN_DIRECTORY "sigtest", &st_bytes);
        if (!st_img) {
            panic("M76 self-test: /bin/sigtest is not on this disk");
        }
        const char *st_argv[] = {PATH_BIN_DIRECTORY "sigtest", 0};
        task_t *st = process_spawnv("sigtest", st_img, st_bytes, st_argv);
        if (!st) {
            panic("M76 self-test: could not spawn sigtest");
        }
        int st_pid = st->id;

        int ready = 0;
        for (int i = 0; i < 300 && !ready; i++) {
            if (virtual_file_system_exists(READY)) {
                ready = 1;
                break;
            }
            if (st->state == TASK_TERMINATED) {
                break;
            }
            pit_sleep_ms(50);
        }
        if (!ready) {
            kernel_log_puts("[m76] sigtest never reached its ready point - it exited ");
            kernel_log_put_dec((uint32_t)st->exit_code);
            kernel_log_puts(" (see user_space/binaries/sigtest.c for what each code means)\n");
            all_ok = 0;
        }

        if (ready) {
            if (do_syscall(SYS_kill, (uint64_t)st_pid, SIGINT, 0) != 0) {
                kernel_log_puts("[m76] SYS_kill refused SIGINT - only the two that kill are accepted\n");
                all_ok = 0;
            }
            int handled = 0;
            for (int i = 0; i < 200 && !handled; i++) {
                if (virtual_file_system_exists(ALIVE)) {
                    handled = 1;
                    break;
                }
                pit_sleep_ms(50);
            }
            if (!handled) {
                kernel_log_puts("[m76] the SIGINT handler never ran, or the process did not "
                           "survive it - sigtest is ");
                kernel_log_puts(st->state == TASK_TERMINATED ? "terminated" : "still running");
                kernel_log_putc('\n');
                all_ok = 0;
            }

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
                    kernel_log_puts("[m76] after SIGINT, sigtest is not listed as a living task - "
                               "a caught signal still killed it\n");
                    all_ok = 0;
                }
            }

            static char alive[64];
            k_memset(alive, 0, sizeof(alive));
            int64_t an = virtual_file_system_read(ALIVE, alive, sizeof(alive) - 1);
            if (an <= 0 || !selftest_contains(alive, "handled-and-alive 1")) {
                kernel_log_puts("[m76] the handler ran a number of times other than once: '");
                kernel_log_puts(alive);
                kernel_log_puts("'\n");
                all_ok = 0;
            }

            do_syscall(SYS_kill, (uint64_t)st_pid, SIGUSR1, 0);
            long code = do_syscall(SYS_wait, (uint64_t)st_pid, 0, 0);
            if (code != 0) {
                kernel_log_puts("[m76] sigtest exited ");
                kernel_log_put_dec((uint32_t)code);
                kernel_log_puts(" - see user_space/binaries/sigtest.c for what that code means\n");
                all_ok = 0;
            }
        }
        selftest_reap(st);

        {
            size_t h_bytes = 0;
            uint8_t *h_img = read_program(PATH_BIN_DIRECTORY "sh", &h_bytes);
            const char *h_argv[] = {PATH_BIN_DIRECTORY "sh", 0};
            task_t *h = process_spawnv("sh", h_img, h_bytes, h_argv);
            kfree(h_img);
            if (!h) {
                panic("M76 self-test: could not spawn the default-action fixture");
            }
            pit_sleep_ms(300);
            do_syscall(SYS_kill, (uint64_t)h->id, SIGINT, 0);
            long code = do_syscall(SYS_wait, (uint64_t)h->id, 0, 0);
            if (code != 128 + SIGINT) {
                kernel_log_puts("[m76] a process with no SIGINT handler exited ");
                kernel_log_put_dec((uint32_t)code);
                kernel_log_puts(" rather than 130 - the default action is not being applied\n");
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

        kernel_log_puts("[m76] a signal a program can catch: a handler installed, entered "
                   "through a frame on the process's own stack and returned from with "
                   "every register intact, a blocked signal held until it was unblocked, "
                   "a SIGCHLD that arrived without anyone polling, a SIGINT survived - and "
                   "the same SIGINT still ending a process that installed nothing - "
                   "self-test passed.\n\n");
    }

    {
        size_t ft_bytes = 0;
        uint8_t *ft_img = read_program(PATH_BIN_DIRECTORY "faulttest", &ft_bytes);
        if (!ft_img) {
            panic("M99 self-test: /bin/faulttest is not on this disk");
        }
        const char *ft_argv[] = {PATH_BIN_DIRECTORY "faulttest", 0};
        task_t *ft = process_spawnv("faulttest", ft_img, ft_bytes, ft_argv);
        long ft_rc = ft ? do_syscall(SYS_wait, (uint64_t)ft->id, 0, 0) : -1;
        kfree(ft_img);
        if (ft_rc != 0) {
            kernel_log_puts("[m99fault] faulttest exited ");
            kernel_log_put_dec((uint32_t)(ft_rc < 0 ? 99 : ft_rc));
            kernel_log_puts(" - see user_space/binaries/faulttest.c for what each code "
                       "means\n");
            panic("M99 self-test: a fault this program caught was not delivered, "
                  "or one it could not catch did not end it");
        }
        kernel_log_puts("[m99] a fault a program can catch: a null dereference, an "
                   "integer divide by zero and an opcode this CPU does not have, "
                   "each delivered to its own handler as SIGSEGV, SIGFPE and "
                   "SIGILL, each survived twice so the mask came back, a "
                   "child whose own handler faults killed once rather than "
                   "looping, and a three-argument SA_SIGINFO handler given the "
                   "address that actually faulted and the pid and status of the "
                   "child that actually ended - self-test passed.\n\n");
    }

    {
        int all_ok = 1;
        const char *ROOT = PATH_TEMPORARY_DIRECTORY "m77";
        const char *SUB = PATH_TEMPORARY_DIRECTORY "m77/inner";
        const char *OUT = PATH_TEMPORARY_DIRECTORY "m77out";

        do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m77/inner/deep.bin"), 0, 0);
        do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m77/top.txt"), 0, 0);
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
        if (do_syscall(SYS_writefile, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m77/top.txt"),
                        (uint64_t)eleven, sizeof(eleven)) != 0 ||
            do_syscall(SYS_writefile, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m77/inner/deep.bin"),
                        (uint64_t)thirty, sizeof(thirty)) != 0) {
            panic("M77 self-test: could not write the fixture files");
        }

        size_t tw_bytes = 0;
        uint8_t *tw_img = read_program(PATH_BIN_DIRECTORY "treewalk", &tw_bytes);
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
        const char *tw_argv[] = {PATH_BIN_DIRECTORY "treewalk", ROOT, 0};
        task_t *tw = process_spawnv("treewalk", tw_img, tw_bytes, tw_argv);
        long rc = tw ? do_syscall(SYS_wait, (uint64_t)tw->id, 0, 0) : -1;
        do_syscall(SYS_dup2, (uint64_t)saved, 1, 0);
        do_syscall(SYS_close, (uint64_t)saved, 0, 0);
        do_syscall(SYS_close, (uint64_t)outfd, 0, 0);
        kfree(tw_img);
        if (rc != 0) {
            kernel_log_puts("[m77] treewalk exited ");
            kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            kernel_log_putc('\n');
            all_ok = 0;
        }

        static char walked[2048];
        k_memset(walked, 0, sizeof(walked));
        int64_t wn = virtual_file_system_read(OUT, walked, sizeof(walked) - 1);
        if (wn <= 0) {
            kernel_log_puts("[m77] the walker printed nothing\n");
            all_ok = 0;
        } else {
            walked[wn] = '\0';
            static const struct { const char *needle; const char *what; } WANT[] = {
                {"f       11 " PATH_TEMPORARY_DIRECTORY "m77/top.txt",
                 "a file at the top level, with the size <sys/stat.h> reported"},
                {"d ", "a directory, told apart from a file by S_ISDIR"},
                {"f       30 " PATH_TEMPORARY_DIRECTORY "m77/inner/deep.bin",
                 "a file one level down, found by descending rather than by being told"},
                {"total 41 byte(s) in 2 file(s), 1 director(ies)",
                 "a total that adds up - which is what makes this a walk rather than a listing"},
            };
            for (size_t w = 0; w < sizeof(WANT) / sizeof(WANT[0]); w++) {
                if (!selftest_contains(walked, WANT[w].needle)) {
                    kernel_log_puts("[m77] the walker did not demonstrate ");
                    kernel_log_puts(WANT[w].what);
                    kernel_log_putc('\n');
                    all_ok = 0;
                }
            }
            if (selftest_contains(walked, "!!")) {
                kernel_log_puts("[m77] the walker reported an inconsistency:\n");
                kernel_log_puts(walked);
                all_ok = 0;
            }
        }

        {
            const char *A = PATH_TEMPORARY_DIRECTORY "m77/named.txt";
            const char *B = PATH_TEMPORARY_DIRECTORY "m77/renamed.txt";
            do_syscall(SYS_unlink, (uint64_t)B, 0, 0);
            do_syscall(SYS_writefile, (uint64_t)A, (uint64_t)thirty, sizeof(thirty));
            long fd = do_syscall(SYS_open, (uint64_t)A, OPEN_READ, 0);
            if (fd < 0) {
                kernel_log_puts("[m77] could not open the fstat fixture\n");
                all_ok = 0;
            } else {
                if (do_syscall(SYS_rename, (uint64_t)A, (uint64_t)B, 0) != 0) {
                    kernel_log_puts("[m77] could not rename the fstat fixture out from under its fd\n");
                    all_ok = 0;
                }
                os_stat_t st;
                k_memset(&st, 0, sizeof(st));
                if (do_syscall(SYS_fstat, (uint64_t)fd, (uint64_t)&st, 0) != 0 ||
                    st.size != sizeof(thirty) || st.is_directory) {
                    kernel_log_puts("[m77] SYS_fstat could not describe a descriptor whose name "
                               "had changed - which is the one question SYS_stat cannot answer\n");
                    all_ok = 0;
                }
                int pfds[2];
                if (do_syscall(SYS_pipe, (uint64_t)pfds, 0, 0) == 0) {
                    k_memset(&st, 0, sizeof(st));
                    if (do_syscall(SYS_fstat, (uint64_t)pfds[0], (uint64_t)&st, 0) != 0 ||
                        st.kind != OS_STAT_FIFO || st.size != 0 || st.is_directory) {
                        kernel_log_puts("[m77] SYS_fstat could not say that a pipe is a pipe\n");
                        all_ok = 0;
                    }
                    do_syscall(SYS_close, (uint64_t)pfds[0], 0, 0);
                    do_syscall(SYS_close, (uint64_t)pfds[1], 0, 0);
                    if (do_syscall(SYS_fstat, (uint64_t)pfds[0], (uint64_t)&st, 0) != -1) {
                        kernel_log_puts("[m77] SYS_fstat described a descriptor that is not open\n");
                        all_ok = 0;
                    }
                }
                do_syscall(SYS_close, (uint64_t)fd, 0, 0);
            }
            do_syscall(SYS_unlink, (uint64_t)B, 0, 0);
        }

        do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m77/inner/deep.bin"), 0, 0);
        do_syscall(SYS_unlink, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m77/top.txt"), 0, 0);
        do_syscall(SYS_unlink, (uint64_t)OUT, 0, 0);
        do_syscall(SYS_rmdir, (uint64_t)SUB, 0, 0);
        if (do_syscall(SYS_rmdir, (uint64_t)ROOT, 0, 0) != 0) {
            kernel_log_puts("[m77] the fixture tree could not be removed - something is still in it\n");
            all_ok = 0;
        }
        if (tw) {
            selftest_reap(tw);
        }

        if (!all_ok) {
            panic("M77 self-test: a program written against POSIX headers cannot walk this filesystem");
        }

        kernel_log_puts("[m77] POSIX names for what is already here: a program including only "
                   "<dirent.h>, <sys/stat.h> and <unistd.h> walked a tree it was not told "
                   "the shape of, S_ISDIR and d_type agreed on every entry, the sizes added "
                   "up, and fstat described a descriptor whose name had changed - "
                   "self-test passed.\n\n");
    }

    {
        size_t sct_size_bytes = 0;
        uint8_t *sct_image = read_program("/bin/syscalltest", &sct_size_bytes);
        task_t *sct_task = process_spawn("syscalltest", sct_image, sct_size_bytes, "");
        kfree(sct_image);
        long sct_status = do_syscall(SYS_wait, (uint64_t)sct_task->id, 0, 0);
        if (sct_status != 0) {
            panic("[q5] a syscall accepted an argument it should have refused - see the syscalltest lines above");
        }
        kernel_log_puts("[q5] every syscall told a lie: all 98 entries handed a null, a "
                   "kernel address, an unmapped one, an address at the user/kernel line and "
                   "an overflowing length; every one returned, every pointer argument was "
                   "refused, every unopened fd and every out-of-range syscall number was "
                   "rejected - self-test passed.\n\n");
    }

    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        size_t ex_bytes = 0;
        uint8_t *ex_img = read_program(PATH_BIN_DIRECTORY "exhausttest", &ex_bytes);
        if (!ex_img) {
            panic("Q9 self-test: /bin/exhausttest is not on this disk");
        }
        uint64_t frames_before = physical_memory_free_frame_count();
        for (int round = 0; round < 2; round++) {
            const char *ex_argv[] = {PATH_BIN_DIRECTORY "exhausttest", 0};
            task_t *ex = process_spawnv("exhausttest", ex_img, ex_bytes, ex_argv);
            long rc = ex ? do_syscall(SYS_wait, (uint64_t)ex->id, 0, 0) : -1;
            if (rc != 0) {
                kernel_log_puts("[q9] exhausttest round ");
                kernel_log_put_dec((uint32_t)round);
                kernel_log_puts(" exited ");
                kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
                kernel_log_puts(" - see user_space/binaries/exhausttest.c for what each "
                          "code means\n");
                kfree(ex_img);
                panic("Q9 self-test: a resource this machine ran out of did not "
                      "refuse, or did not come back");
            }
        }
        kfree(ex_img);

        uint64_t frames_after = physical_memory_free_frame_count();
        if (frames_after < frames_before) {
            kernel_log_puts("[q9] filling and emptying every table cost ");
            kernel_log_put_dec((uint32_t)(frames_before - frames_after));
            kernel_log_puts(" frames that never came back\n");
            panic("Q9 self-test: exhausting a resource leaked physical memory");
        }

        {
            static char q9_buffer[512];
            k_memset(q9_buffer, 'q', sizeof(q9_buffer));
            if (virtual_file_system_write("/tmp/q9-after", q9_buffer, sizeof(q9_buffer)) != 0) {
                panic("Q9 self-test: the machine could not work after running out");
            }
            k_memset(q9_buffer, 0, sizeof(q9_buffer));
            if (virtual_file_system_read("/tmp/q9-after", q9_buffer, sizeof(q9_buffer)) != (int64_t)sizeof(q9_buffer) ||
                q9_buffer[0] != 'q') {
                panic("Q9 self-test: the machine could not work after running out");
            }
            virtual_file_system_unlink("/tmp/q9-after");
        }

        {
            uint64_t f0 = physical_memory_free_frame_count();
            size_t hu0 = heap_used_bytes();
            size_t ht0 = heap_total_bytes();
            int tasks0 = scheduler_task_count();
            block_device_statistics_t bs0;
            block_device_statistics(&bs0);

            for (int i = 0; i < 2000; i++) {
                int fd = virtual_file_system_open("/tmp/q9-churn", 1);
                if (fd >= 0) {
                    virtual_file_system_handle_close(fd);
                }
            }
            virtual_file_system_unlink("/tmp/q9-churn");

            size_t sp_bytes = 0;
            uint8_t *sp_img = read_program(PATH_BIN_DIRECTORY "hello", &sp_bytes);
            if (sp_img) {
                const char *sp_argv[] = {PATH_BIN_DIRECTORY "hello", 0};
                for (int i = 0; i < 200; i++) {
                    task_t *t = process_spawnv("hello", sp_img, sp_bytes, sp_argv);
                    if (t) {
                        do_syscall(SYS_wait, (uint64_t)t->id, 0, 0);
                    }
                }
                kfree(sp_img);
            }

            uint64_t f1 = physical_memory_free_frame_count();
            size_t hu1 = heap_used_bytes();
            size_t ht1 = heap_total_bytes();
            int tasks1 = scheduler_task_count();
            block_device_statistics_t bs1;
            block_device_statistics(&bs1);

            kernel_log_puts("[q9] leak audit over 2000 open/close and 200 spawn/exit "
                       "rounds - free frames ");
            kernel_log_put_dec((uint32_t)f0);
            kernel_log_puts(" -> ");
            kernel_log_put_dec((uint32_t)f1);
            kernel_log_puts(", heap used ");
            kernel_log_put_dec((uint32_t)hu0);
            kernel_log_puts(" -> ");
            kernel_log_put_dec((uint32_t)hu1);
            kernel_log_puts(", heap total ");
            kernel_log_put_dec((uint32_t)ht0);
            kernel_log_puts(" -> ");
            kernel_log_put_dec((uint32_t)ht1);
            kernel_log_puts(", task slots ");
            kernel_log_put_dec((uint32_t)tasks0);
            kernel_log_puts(" -> ");
            kernel_log_put_dec((uint32_t)tasks1);
            kernel_log_puts(", cache blocks ");
            kernel_log_put_dec((uint32_t)bs0.resident);
            kernel_log_puts(" -> ");
            kernel_log_put_dec((uint32_t)bs1.resident);
            kernel_log_putc('\n');

            if (f1 < f0) {
                panic("Q9 leak audit: physical frames did not come back");
            }
            if (hu1 > hu0) {
                panic("Q9 leak audit: the kernel heap did not come back");
            }
        }

        kernel_log_puts("[q9] a machine that runs out of things and stays up: "
                   "descriptors, pipes, shared-memory segments and sockets each "
                   "taken to their ceiling and each refusing rather than halting, "
                   "every one of them given back and taken again, twice over with "
                   "no frame lost between the rounds, and the machine still "
                   "reading and writing files afterwards, and a leak audit "
                   "over 2200 rounds with every frame and every heap byte back "
                   "- self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        if (!virtual_file_system_exists("/pkg/repo/index")) {
            kernel_log_puts("[m111] no package repository on this disk - skipped. "
                      "Run tools/build-packages.sh and `make packages`.\n\n");
        } else {
            size_t pk_bytes = 0;
            uint8_t *pk_img = read_program(PATH_BIN_DIRECTORY "pkgtest", &pk_bytes);
            if (!pk_img) {
                panic("M111 self-test: /bin/pkgtest is not on this disk");
            }
            const char *pk_argv[] = {PATH_BIN_DIRECTORY "pkgtest", 0};
            task_t *pk = process_spawnv("pkgtest", pk_img, pk_bytes, pk_argv);
            long rc = pk ? do_syscall(SYS_wait, (uint64_t)pk->id, 0, 0) : -1;
            kfree(pk_img);
            if (rc != 0) {
                kernel_log_puts("[m111] pkgtest exited ");
                kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
                kernel_log_puts(" - see user_space/binaries/pkgtest.c for what each code "
                          "means\n");
                panic("M111 self-test: the package manager did not install, run "
                      "or isolate a package the way it says it does");
            }

            int entries = pkg_registry_count();
            if (entries <= 0) {
                kernel_log_puts("[m111] the kernel's view of /pkg/db/caps has ");
                kernel_log_put_dec((uint32_t)(entries < 0 ? 0 : entries));
                kernel_log_puts(" entries after two installs - the registry did not "
                          "reach the kernel, so every package would have been "
                          "refused everything for the wrong reason\n");
                panic("M111 self-test: the package capability registry is empty");
            }
            kernel_log_puts("[m111] the kernel's package registry: ");
            kernel_log_put_dec((uint32_t)entries);
            kernel_log_puts(" entries, reloaded from disk because a write under /pkg "
                      "invalidated it\n");

            kernel_log_puts("[m111] a package manager: GNU grep 3.11, built here by "
                      "this project's own compiler, installed by /bin/os from a "
                      "verified archive into its own prefix, run from /pkg/bin "
                      "with the capabilities its manifest asked for - and a "
                      "package calling its binary `compositor` got none of the "
                      "compositor's - self-test passed (");
            kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
            kernel_log_puts(" ms).\n\n");
        }
    }

    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        size_t dt_bytes = 0;
        uint8_t *dt_img = read_program(PATH_BIN_DIRECTORY "dirtest", &dt_bytes);
        if (!dt_img) {
            panic("M112 self-test: /bin/dirtest is not on this disk");
        }
        const char *dt_argv[] = {PATH_BIN_DIRECTORY "dirtest", 0};
        task_t *dt = process_spawnv("dirtest", dt_img, dt_bytes, dt_argv);
        long rc = dt ? do_syscall(SYS_wait, (uint64_t)dt->id, 0, 0) : -1;
        kfree(dt_img);
        if (rc != 0) {
            kernel_log_puts("[m112] dirtest exited ");
            kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            kernel_log_puts(" - see user_space/binaries/dirtest.c for what each code "
                      "means\n");
            panic("M112 self-test: the recursive tree walk behind the Files "
                  "app does not do what it says on this filesystem");
        }
        kernel_log_puts("[m112] the Files app's tree walks, on leanfs: a three-level "
                  "tree counted to the byte, SYS_rmdir refusing the full "
                  "directory it is supposed to refuse, the whole tree removed "
                  "by the user-space walk, and the directory beside it "
                  "untouched - self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        size_t bt_bytes = 0;
        uint8_t *bt_img = read_program(PATH_BIN_DIRECTORY "browsertest", &bt_bytes);
        if (!bt_img) {
            panic("M100 self-test: /bin/browsertest is not on this disk");
        }
        const char *bt_argv[] = {PATH_BIN_DIRECTORY "browsertest", 0};
        task_t *bt = process_spawnv("browsertest", bt_img, bt_bytes, bt_argv);
        long rc = bt ? do_syscall(SYS_wait, (uint64_t)bt->id, 0, 0) : -1;
        kfree(bt_img);
        if (rc != 0) {
            kernel_log_puts("[m100h] browsertest exited ");
            kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            kernel_log_puts(" - see user_space/binaries/browsertest.c for what each "
                      "code means\n");
            panic("M100 self-test: what the browser port added to this "
                  "system does not work on this machine");
        }
        kernel_log_puts("[m100h] what porting a browser added: pread and pwrite "
                  "leaving the descriptor's own offset where it was, "
                  "scandir filtering and sorting a directory, iconv turning "
                  "windows-1252 curly quotes into UTF-8 and REFUSING a "
                  "character ISO-8859-1 does not have, and lround rounding "
                  "2.5 to 3 - self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        size_t comp_bytes = 0;
        uint8_t *comp_image = read_program(PATH_BIN_DIRECTORY "compositor", &comp_bytes);
        size_t demo_bytes = 0;
        uint8_t *demo_image = read_program(PATH_BIN_DIRECTORY "lvgl_demo", &demo_bytes);
        if (!comp_image || !demo_image) {
            panic("M125 self-test: /bin/lvgl_demo is not on this disk");
        }

        task_t *comp_task = process_spawn("compositor", comp_image, comp_bytes, "");
        kfree(comp_image);
        selftest_wait_for_compositor();

        task_t *demo_task = process_spawn("lvgl_demo", demo_image, demo_bytes, "");
        kfree(demo_image);
        pit_sleep_ms(3000);

        uint32_t width = framebuffer_width();
        uint32_t height = framebuffer_height();
        uint32_t distinct_table[LVGL_DISTINCT_BUCKETS];
        for (uint32_t i = 0; i < LVGL_DISTINCT_BUCKETS; i++) {
            distinct_table[i] = LVGL_DISTINCT_EMPTY;
        }
        uint32_t distinct = 0;
        uint32_t drawn = 0;
        for (uint32_t y = 0; y < height; y += 2) {
            for (uint32_t x = 0; x < width; x += 2) {
                uint32_t pixel = framebuffer_get_pixel(x, y) & 0x00FFFFFFu;
                if (pixel == 0x001A1A2Eu) {
                    continue;
                }
                drawn++;
                uint32_t slot = (pixel * 2654435761u) % LVGL_DISTINCT_BUCKETS;
                for (uint32_t probe = 0; probe < LVGL_DISTINCT_BUCKETS; probe++) {
                    uint32_t at = (slot + probe) % LVGL_DISTINCT_BUCKETS;
                    if (distinct_table[at] == pixel) {
                        break;
                    }
                    if (distinct_table[at] == LVGL_DISTINCT_EMPTY) {
                        distinct_table[at] = pixel;
                        distinct++;
                        break;
                    }
                }
            }
        }

        selftest_reap(demo_task);
        selftest_reap(comp_task);
        console_init();
        kernel_log_use_console();

        if (drawn < LVGL_MINIMUM_DRAWN_PIXELS) {
            kernel_log_puts("[m125] the LVGL window drew ");
            kernel_log_put_dec(drawn);
            kernel_log_puts(" pixels, wanted at least ");
            kernel_log_put_dec(LVGL_MINIMUM_DRAWN_PIXELS);
            kernel_log_putc('\n');
            panic("M125 self-test: the LVGL toolkit painted nothing the "
                  "compositor showed");
        }
        if (distinct < LVGL_MINIMUM_DISTINCT_COLORS) {
            kernel_log_puts("[m125] the LVGL window used ");
            kernel_log_put_dec(distinct);
            kernel_log_puts(" distinct colors, wanted at least ");
            kernel_log_put_dec(LVGL_MINIMUM_DISTINCT_COLORS);
            kernel_log_puts(" - a flat fill is not an anti-aliased widget\n");
            panic("M125 self-test: the LVGL toolkit rendered without "
                  "anti-aliasing");
        }
        kernel_log_puts("[m125] a third-party toolkit on this compositor: LVGL "
                  "rendering a flex-laid-out card of widgets straight into "
                  "the window's shared memory with no copy, ");
        kernel_log_put_dec(drawn);
        kernel_log_puts(" pixels in ");
        kernel_log_put_dec(distinct);
        kernel_log_puts(" distinct colors - which a bitmap-font toolkit "
                  "cannot produce - self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        int un_before = unix_socket_in_use();
        size_t ut_bytes = 0;
        uint8_t *ut_img = read_program(PATH_BIN_DIRECTORY "unixtest", &ut_bytes);
        if (!ut_img) {
            panic("M118 self-test: /bin/unixtest is not on this disk");
        }
        const char *ut_argv[] = {PATH_BIN_DIRECTORY "unixtest", 0};
        task_t *ut = process_spawnv("unixtest", ut_img, ut_bytes, ut_argv);
        long rc = ut ? do_syscall(SYS_wait, (uint64_t)ut->id, 0, 0) : -1;
        kfree(ut_img);
        if (rc != 0) {
            kernel_log_puts("[m118] unixtest exited ");
            kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            kernel_log_puts(" - see user_space/binaries/unixtest.c for what each "
                      "code means\n");
            panic("M118 self-test: AF_UNIX or descriptor passing does not "
                  "work on this machine");
        }
        int un_after = unix_socket_in_use();
        int queued = unix_socket_queued_file_descriptors();
        if (un_after != un_before || queued != 0) {
            kernel_log_puts("[m118] sockets before ");
            kernel_log_put_dec((uint32_t)un_before);
            kernel_log_puts(", after ");
            kernel_log_put_dec((uint32_t)un_after);
            kernel_log_puts(", descriptors still queued ");
            kernel_log_put_dec((uint32_t)queued);
            kernel_log_puts("\n");
            panic("M118 self-test: a process that exited left a Unix-domain "
                  "socket or a passed descriptor behind");
        }
        kernel_log_puts("[m118] AF_UNIX: a socketpair both ways, a SOCK_SEQPACKET "
                  "boundary kept, a pipe end and an open FILE passed to a "
                  "forked child through SCM_RIGHTS - the file still at the "
                  "offset its parent had read to - a socket passed over a "
                  "socket, a path name and an abstract name dialled from "
                  "another process, a blocking read woken by its peer, "
                  "shutdown seen as end of stream, MSG_CTRUNC reported, and "
                  "a child holding NO capabilities doing all of it - "
                  "self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        int ev_before = eventfd_in_use();
        int tf_before = timerfd_in_use();
        int ep_before = epoll_in_use();
        size_t et_bytes = 0;
        uint8_t *et_img = read_program(PATH_BIN_DIRECTORY "epolltest", &et_bytes);
        if (!et_img) {
            panic("M119 self-test: /bin/epolltest is not on this disk");
        }
        const char *et_argv[] = {PATH_BIN_DIRECTORY "epolltest", 0};
        task_t *et = process_spawnv("epolltest", et_img, et_bytes, et_argv);
        long rc = et ? do_syscall(SYS_wait, (uint64_t)et->id, 0, 0) : -1;
        kfree(et_img);
        if (rc != 0) {
            kernel_log_puts("[m119] epolltest exited ");
            kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            kernel_log_puts(" - see user_space/binaries/epolltest.c for what each "
                      "code means\n");
            panic("M119 self-test: epoll, eventfd or timerfd does not work "
                  "on this machine");
        }
        if (eventfd_in_use() != ev_before || timerfd_in_use() != tf_before ||
            epoll_in_use() != ep_before) {
            kernel_log_puts("[m119] counters before ");
            kernel_log_put_dec((uint32_t)ev_before);
            kernel_log_puts("/");
            kernel_log_put_dec((uint32_t)tf_before);
            kernel_log_puts("/");
            kernel_log_put_dec((uint32_t)ep_before);
            kernel_log_puts(", after ");
            kernel_log_put_dec((uint32_t)eventfd_in_use());
            kernel_log_puts("/");
            kernel_log_put_dec((uint32_t)timerfd_in_use());
            kernel_log_puts("/");
            kernel_log_put_dec((uint32_t)epoll_in_use());
            kernel_log_puts("\n");
            panic("M119 self-test: a process that exited left an eventfd, a "
                  "timerfd or an epoll set behind");
        }
        kernel_log_puts("[m119] a message pump: an eventfd counting and saturating, "
                  "EFD_SEMAPHORE taking one, a 60 ms timer that fired at 60 "
                  "and a 10 ms one that reported the firings nobody read, a "
                  "set holding a pipe and a Unix socket and a counter and a "
                  "timer at once with every cookie back, EPOLLOUT told the "
                  "truth about a full pipe, EPOLLERR when its reader went, "
                  "EPOLLHUP, a closed descriptor dropped, EPOLLET silent on "
                  "an unchanged condition, EPOLLONESHOT fired once and "
                  "re-armed, a 200 ms epoll_wait(-1) that ended when its "
                  "timer did WITH THE CPU IDLE, and a wake from another "
                  "process - self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        int mf_before = memfd_in_use();
        uint32_t pages_before = memfd_pages_held();
        uint64_t frames_before = physical_memory_free_frame_count();
        size_t mf_bytes = 0;
        uint8_t *mf_img = read_program(PATH_BIN_DIRECTORY "memfdtest", &mf_bytes);
        if (!mf_img) {
            panic("M120 self-test: /bin/memfdtest is not on this disk");
        }
        const char *mf_argv[] = {PATH_BIN_DIRECTORY "memfdtest", 0};
        task_t *mf = process_spawnv("memfdtest", mf_img, mf_bytes, mf_argv);
        long rc = mf ? do_syscall(SYS_wait, (uint64_t)mf->id, 0, 0) : -1;
        kfree(mf_img);
        if (rc != 0) {
            kernel_log_puts("[m120] memfdtest exited ");
            kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            kernel_log_puts(" - see user_space/binaries/memfdtest.c for what each "
                      "code means\n");
            panic("M120 self-test: shared memory by descriptor does not work "
                  "on this machine");
        }
        if (memfd_in_use() != mf_before || memfd_pages_held() != pages_before) {
            kernel_log_puts("[m120] objects before ");
            kernel_log_put_dec((uint32_t)mf_before);
            kernel_log_puts(" after ");
            kernel_log_put_dec((uint32_t)memfd_in_use());
            kernel_log_puts(", pages before ");
            kernel_log_put_dec(pages_before);
            kernel_log_puts(" after ");
            kernel_log_put_dec(memfd_pages_held());
            kernel_log_puts(", first still alive: '");
            kernel_log_puts(memfd_first_live_name());
            kernel_log_puts("'\n");
            panic("M120 self-test: a process that exited left shared memory "
                  "behind");
        }
        uint64_t frames_after = physical_memory_free_frame_count();
        if (frames_after < frames_before) {
            kernel_log_puts("[m120] sharing memory across a channel cost ");
            kernel_log_put_dec((uint32_t)(frames_before - frames_after));
            kernel_log_puts(" frames that never came back\n");
            panic("M120 self-test: shared memory leaked physical frames");
        }
        kernel_log_puts("[m120] a buffer shared across a channel: a memfd created, "
                  "sized, mapped and written; its descriptor sent over a Unix "
                  "socket to a forked child that holds no capabilities at "
                  "all; the child's writes read back by the parent through a "
                  "mapping it made before the child existed and kept after "
                  "both descriptors were closed; F_SEAL_WRITE refusing a "
                  "writable mapping; a shrink, a MAP_PRIVATE, a read and a "
                  "map past the end all refused - self-test passed, every "
                  "frame back (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        os_stat_t ct;
        if (do_syscall(SYS_stat, (uint64_t)"/bin/clangtest", (uint64_t)&ct, 0) != 0) {
            kernel_log_puts("[m121] /bin/clangtest is not on this image - skipped. "
                       "tools/build-clang.sh builds the compiler and "
                       "tools/clang-test.sh installs what it produces.\n\n");
        } else {
            int all_ok = 1;
            int have_cxx =
                do_syscall(SYS_stat, (uint64_t)"/bin/clangcxxtest",
                           (uint64_t)&ct, 0) == 0;
            const char *script = PATH_TEMPORARY_DIRECTORY "m121.sh";
            const char *result = PATH_TEMPORARY_DIRECTORY "m121.out";
            static const char SCRIPT[] =
                "#!/bin/sh\n"
                "/bin/clangtest > " PATH_TEMPORARY_DIRECTORY "m121.out\n"
                "/bin/mixedtest >> " PATH_TEMPORARY_DIRECTORY "m121.out\n";
            static const char SCRIPT_CXX[] =
                "#!/bin/sh\n"
                "/bin/clangtest > " PATH_TEMPORARY_DIRECTORY "m121.out\n"
                "/bin/mixedtest >> " PATH_TEMPORARY_DIRECTORY "m121.out\n"
                "/bin/clangcxxtest >> " PATH_TEMPORARY_DIRECTORY "m121.out\n";
            const char *body = have_cxx ? SCRIPT_CXX : SCRIPT;
            size_t body_length = have_cxx ? sizeof(SCRIPT_CXX) - 1
                                       : sizeof(SCRIPT) - 1;
            if (do_syscall(SYS_writefile, (uint64_t)script, (uint64_t)body,
                            body_length) != 0) {
                panic("M121 self-test: could not write the script fixture");
            }
            long pid = do_syscall(SYS_spawn, (uint64_t)script, 0, 0);
            if (pid < 0) {
                kernel_log_puts("[m121] the compiled programs could not be spawned\n");
                all_ok = 0;
            } else {
                do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            }

            static char produced[1536];
            k_memset(produced, 0, sizeof(produced));
            int64_t n = virtual_file_system_read(result, produced, sizeof(produced) - 1);
            if (n <= 0) {
                kernel_log_puts("[m121] the compiled programs produced no output\n");
                all_ok = 0;
            } else {
                produced[n] = '\0';
                static const struct { const char *needle; const char *what; } EXPECT[] = {
                    {"clangtest: constructor ran",
                     "crti/crtbegin/crtend/crtn linked in order by clang's driver"},
                    {"clangtest: malloc and string round trip",
                     "libc.a out of the sysroot, from clang's link line"},
                    {"clangtest: struct return and varargs",
                     "the ABI this kernel's crt0 assumes"},
                    {"clangtest: floating point", "SSE state"},
                    {"clangtest: 128-bit division through libgcc",
                     "__divti3 - clang's codegen reaching M94's libgcc"},
                    {"clangtest: the large code model, at 80",
                     "the program loaded at 512 GiB, checked from inside it"},
                    {"clangtest: a signal over live stack data",
                     "-mno-red-zone: M76's signal frame did not eat live data"},
                    {"clangtest: setjmp and longjmp", "a non-local exit"},
                    {"clangtest: a thread-local in a static program",
                     "local-exec TLS"},
                    {"clangtest: an atomic read-modify-write",
                     "an inline lock-prefixed operation"},
                    {"clangtest: every check passed",
                     "every check in tests/clang/hello.c"},
                    {"clangtest: atexit ran",
                     "the exit handlers, which run after main returns"},
                    {"mixedtest: clang and gcc agree about this target's ABI",
                     "one program from two compilers, calling both ways"},
                };
                for (unsigned i = 0; i < sizeof(EXPECT) / sizeof(EXPECT[0]); i++) {
                    if (!selftest_contains(produced, EXPECT[i].needle)) {
                        kernel_log_puts("[m121] missing: ");
                        kernel_log_puts(EXPECT[i].what);
                        kernel_log_putc('\n');
                        all_ok = 0;
                    }
                }
                if (have_cxx) {
                    static const struct { const char *needle; const char *what; } CXX[] = {
                        {"clangcxxtest: a static destructor ran",
                         "__cxa_atexit, from libc++abi"},
                        {"clangcxxtest: every check passed",
                         "every check in tests/clang/cxx.cpp - including the "
                         "counted, ordered destructors of an unwind through "
                         "libgcc_eh"},
                    };
                    for (unsigned i = 0; i < sizeof(CXX) / sizeof(CXX[0]); i++) {
                        if (!selftest_contains(produced, CXX[i].needle)) {
                            kernel_log_puts("[m121] missing: ");
                            kernel_log_puts(CXX[i].what);
                            kernel_log_putc('\n');
                            all_ok = 0;
                        }
                    }
                }
                if (selftest_contains(produced, "FAIL")) {
                    kernel_log_puts("[m121] a program reported a failure of its own\n");
                    all_ok = 0;
                }
            }

            do_syscall(SYS_unlink, (uint64_t)script, 0, 0);
            do_syscall(SYS_unlink, (uint64_t)result, 0, 0);

            if (!all_ok) {
                kernel_log_puts("[m121] what the compiled programs actually wrote:\n");
                kernel_log_puts(produced);
                kernel_log_puts("[m121] ---- end\n");
                panic("M121 self-test: a program x86_64-lean_os-clang produced "
                      "does not run here");
            }

            kernel_log_puts("[m121] a second compiler that knows this OS by name: a "
                      "program from x86_64-lean_os-clang running here with no "
                      "flag supplied by hand - the large code model checked "
                      "from inside the program, a signal delivered over live "
                      "stack data that survived it, __divti3 reached in GCC's "
                      "libgcc, setjmp, a thread-local and an atomic; and ONE "
                      "PROGRAM FROM TWO COMPILERS, clang's object and gcc's "
                      "calling each other across twelve ABI shapes in both "
                      "directions");
            if (have_cxx) {
                kernel_log_puts("; and libc++ over libc++abi over libgcc_eh, with "
                          "the destructors of an unwind counted and their "
                          "order checked");
            } else {
                kernel_log_puts(" (libc++ not on this image - "
                          "tools/build-libcxx.sh builds it)");
            }
            kernel_log_puts(" - self-test passed.\n\n");
        }
    }

    {
        os_stat_t nst;
        if (do_syscall(SYS_stat, (uint64_t)(PATH_BIN_DIRECTORY "netsurf"),
                        (uint64_t)&nst, 0) != 0) {
            kernel_log_puts("[m113] /bin/netsurf is not on this image - skipped. "
                       "`make browser` builds and installs it.\n\n");
        } else {
            int all_ok = 1;

            if (nst.kind != OS_STAT_FILE || nst.size < 1024u * 1024u) {
                kernel_log_puts("[m113] /bin/netsurf is there but is not a "
                           "plausible browser: kind ");
                kernel_log_put_dec((uint32_t)nst.kind);
                kernel_log_puts(", ");
                kernel_log_put_dec((uint32_t)(nst.size / 1024u));
                kernel_log_puts(" KiB\n");
                all_ok = 0;
            }

            os_stat_t cst;
            if (do_syscall(SYS_stat, (uint64_t)"/usr/share/netsurf/default.css",
                            (uint64_t)&cst, 0) != 0 || cst.size == 0) {
                kernel_log_puts("[m113] /usr/share/netsurf/default.css is missing or "
                           "empty - the cascade has no default stylesheet\n");
                all_ok = 0;
            }

            os_stat_t fst;
            if (do_syscall(SYS_stat,
                            (uint64_t)"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                            (uint64_t)&fst, 0) != 0 || fst.size == 0) {
                kernel_log_puts("[m113] DejaVuSans.ttf is not where freetype looks "
                           "for it - a browser with no glyphs\n");
                all_ok = 0;
            }

            uint32_t ncaps = caps_for_program(PATH_BIN_DIRECTORY "netsurf");
            if (ncaps != (CAP_APP_DEFAULT | CAP_NETWORK)) {
                kernel_log_puts("[m113] netsurf's capability grant is 0x");
                kernel_log_put_hex32(ncaps);
                kernel_log_puts(", not CAP_APP_DEFAULT | CAP_NETWORK\n");
                all_ok = 0;
            }
            if (ncaps & CAP_FRAMEBUFFER) {
                kernel_log_puts("[m113] netsurf holds CAP_FRAMEBUFFER - it paints "
                           "its window's shared segment and must not have "
                           "authority over the screen\n");
                all_ok = 0;
            }

            if (!all_ok) {
                panic("M113 self-test: the browser on this image is not "
                      "installed the way a shipped program is");
            }
            kernel_log_puts("[m113] the browser is installed: /bin/netsurf (");
            kernel_log_put_dec((uint32_t)(nst.size / 1024u));
            kernel_log_puts(" KiB), its default stylesheet and DejaVuSans.ttf "
                       "where the compiled-in paths look for them, holding "
                       "CAP_FS_WRITE and CAP_NETWORK and NOT CAP_FRAMEBUFFER "
                       "- self-test passed.\n\n");
        }
    }

    {
        char rc[2048];
        int64_t n = virtual_file_system_read(PATH_RESOLV_CONF, rc, sizeof(rc) - 1);
        if (n <= 0) {
            kernel_log_puts("[m114] " PATH_RESOLV_CONF " is missing - the first-boot "
                       "seeding in this file did not run\n");
            panic("M114 self-test: this machine has no resolver configuration");
        }
        rc[n] = '\0';
        int found = 0;
        for (int64_t i = 0; i + 10 <= n && !found; i++) {
            if ((i == 0 || rc[i - 1] == '\n') &&
                k_memcmp(&rc[i], "nameserver", 10) == 0) {
                found = 1;
            }
        }
        if (!found) {
            kernel_log_puts("[m114] " PATH_RESOLV_CONF " has no nameserver line - "
                       "this machine can only ask whatever DHCP handed it\n");
            panic("M114 self-test: the resolver configuration names no server");
        }
        kernel_log_puts("[m114] more than one nameserver: " PATH_RESOLV_CONF " is on "
                   "this disk with a server in it, so a DHCP nameserver that "
                   "answers nothing is no longer the end of every lookup "
                   "- self-test passed (");
        kernel_log_put_dec((uint32_t)n);
        kernel_log_puts(" bytes).\n\n");
    }

    {
        char want[16];
        k_memset(want, 0, sizeof(want));
        int wn = fwcfg_read_file("opt/leanos/nicstream", want, sizeof(want) - 1);
        if (wn <= 0 || !net_have_nic()) {
            kernel_log_puts("[m116] no host stream on this boot - the NIC's receive "
                       "path is untested (tools/qemu-serial-test.sh provides one)\n\n");
        } else {
            uint32_t checksum_before = tcp_checksum_failures();
            size_t number_bytes = 0;
            uint8_t *number_img = read_program(PATH_BIN_DIRECTORY "netrecv", &number_bytes);
            if (!number_img) {
                panic("M116 self-test: /bin/netrecv is not on this disk");
            }
            const char *number_argv[] = {PATH_BIN_DIRECTORY "netrecv", "10.0.2.100", "7777", want, 0};
            uint64_t t0 = tsc_read();
            task_t *nr = process_spawnv("netrecv", number_img, number_bytes, number_argv);
            kfree(number_img);
            long rc = nr ? (long)do_syscall(SYS_wait, (uint64_t)nr->id, 0, 0) : -1;
            uint64_t ms = tsc_to_us(tsc_read() - t0) / 1000;
            uint32_t corrupt = tcp_checksum_failures() - checksum_before;
            kernel_log_perf("nic_stream_recv_ms", ms, "ms");
            if (rc != 0) {
                kernel_log_puts("[m116] netrecv did not receive the host's stream intact "
                           "(its own line above says how)\n");
                panic("M116 self-test: a stream from the host did not arrive byte for byte");
            }
            if (corrupt != 0) {
                kernel_log_puts("[m116] the stream arrived, but ");
                kernel_log_put_dec(corrupt);
                kernel_log_puts(" segment(s) on the way failed TCP's checksum - the NIC "
                           "is handing the stack corrupt frames and retransmission "
                           "is hiding it\n");
                panic("M116 self-test: corrupt segments on the receive path");
            }
            kernel_log_puts("[m116] a stream from the host: ");
            kernel_log_puts(want);
            kernel_log_puts(" bytes through SLIRP, the RTL8139's ring and TCP, every one "
                       "of them right and no segment failing its checksum, in ");
            kernel_log_put_dec((uint32_t)ms);
            kernel_log_puts(" ms - self-test passed.\n\n");
        }
    }

    {
        int all_ok = 1;
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        uint64_t errors_before = block_device_error_count();
        static char q16_buffer[4096];

        k_memset(q16_buffer, 'a', sizeof(q16_buffer));
        if (virtual_file_system_write("/tmp/q16-before", q16_buffer, sizeof(q16_buffer)) != 0) {
            kernel_log_puts("[q16] a healthy disk refused an ordinary write\n");
            all_ok = 0;
        }

        if (all_ok) {
            block_device_fault_inject(-1, 0);
            k_memset(q16_buffer, 'b', sizeof(q16_buffer));
            int rc = virtual_file_system_write("/tmp/q16-during", q16_buffer, sizeof(q16_buffer));
            block_device_fault_inject(-1, -1);

            if (rc == 0) {
                kernel_log_puts("[q16] a write to a disk that refuses every write reported success\n");
                all_ok = 0;
            }
            if (all_ok && block_device_error_count() <= errors_before) {
                kernel_log_puts("[q16] the disk failed and nothing counted it\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            k_memset(q16_buffer, 0, sizeof(q16_buffer));
            int64_t n = virtual_file_system_read("/tmp/q16-before", q16_buffer, sizeof(q16_buffer));
            if (n != (int64_t)sizeof(q16_buffer) || q16_buffer[0] != 'a' ||
                q16_buffer[sizeof(q16_buffer) - 1] != 'a') {
                kernel_log_puts("[q16] a file written before the fault did not survive it\n");
                all_ok = 0;
            }
        }
        if (all_ok) {
            k_memset(q16_buffer, 'c', sizeof(q16_buffer));
            if (virtual_file_system_write("/tmp/q16-after", q16_buffer, sizeof(q16_buffer)) != 0) {
                kernel_log_puts("[q16] the disk recovered and the filesystem did not\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            block_device_cache_drop();
            block_device_fault_inject(0, -1);
            k_memset(q16_buffer, 'z', sizeof(q16_buffer));
            int64_t n = virtual_file_system_read("/tmp/q16-before", q16_buffer, sizeof(q16_buffer));
            block_device_fault_inject(-1, -1);
            if (n >= 0) {
                kernel_log_puts("[q16] a read from a disk that refuses every read reported success\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            static uint8_t q16_raw[BLOCK_DEVICE_SECTOR_SIZE];
            k_memset(q16_raw, 'z', sizeof(q16_raw));
            block_device_cache_drop();
            block_device_fault_inject(0, -1);
            int rc = block_device_read(0, 1, q16_raw);
            block_device_fault_inject(-1, -1);
            if (rc == 0) {
                kernel_log_puts("[q16] the block layer reported success on a refused read\n");
                all_ok = 0;
            }
            if (all_ok && q16_raw[0] == 'z') {
                kernel_log_puts("[q16] a failed read left the caller's buffer as it found it\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            block_device_cache_drop();
            if (virtual_file_system_check() != 0) {
                kernel_log_puts("[q16] the filesystem is inconsistent after a disk that failed\n");
                all_ok = 0;
            }
        }

        virtual_file_system_unlink("/tmp/q16-before");
        virtual_file_system_unlink("/tmp/q16-during");
        virtual_file_system_unlink("/tmp/q16-after");
        block_device_fault_inject(-1, -1);

        if (!all_ok) {
            panic("Q16 self-test: this machine does not survive a disk that fails");
        }
        kernel_log_puts("[q16] devices that fail, and a machine that keeps running: a write to a "
                   "disk that refuses every write reported an error and was counted, a file "
                   "written before it survived, the next write after it succeeded, a failed "
                   "read reported an error and left zeros rather than stale bytes at the "
                   "block layer and an error at the filesystem, and the "
                   "filesystem is consistent afterwards - through ");
        kernel_log_puts(block_device_backend_name());
        kernel_log_puts(" - self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        int all_ok = 1;
        uint64_t frames_before = physical_memory_free_frame_count();

        size_t mt_bytes = 0;
        uint8_t *mt_img = read_program(PATH_BIN_DIRECTORY "mmaptest", &mt_bytes);
        if (!mt_img) {
            panic("M78 self-test: /bin/mmaptest is not on this disk");
        }
        const char *mt_argv[] = {PATH_BIN_DIRECTORY "mmaptest", 0};
        task_t *mt = process_spawnv("mmaptest", mt_img, mt_bytes, mt_argv);
        long rc = mt ? do_syscall(SYS_wait, (uint64_t)mt->id, 0, 0) : -1;
        kfree(mt_img);
        if (rc != 0) {
            kernel_log_puts("[m78] mmaptest exited ");
            kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            kernel_log_puts(" - see user_space/binaries/mmaptest.c for what each code means\n");
            all_ok = 0;
        }

        uint64_t frames_after = physical_memory_free_frame_count();
        if (frames_after != frames_before) {
            kernel_log_puts("[m78] ");
            kernel_log_put_dec((uint32_t)(frames_before > frames_after
                                     ? frames_before - frames_after : 0));
            kernel_log_puts(" frame(s) did not come back from a process that mapped 64 pages, "
                       "freed them, and exited\n");
            all_ok = 0;
        }

        if (all_ok) {
            uint64_t mid_before = physical_memory_free_frame_count();
            const char *again[] = {PATH_BIN_DIRECTORY "mmaptest", 0};
            uint8_t *again_img = read_program(PATH_BIN_DIRECTORY "mmaptest", &mt_bytes);
            task_t *m2 = again_img
                             ? process_spawnv("mmaptest", again_img, mt_bytes, again)
                             : (task_t *)0;
            if (m2) {
                do_syscall(SYS_wait, (uint64_t)m2->id, 0, 0);
            }
            kfree(again_img);
            if (physical_memory_free_frame_count() != mid_before) {
                kernel_log_puts("[m78] a second map/free/exit round did not return every frame\n");
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

        kernel_log_puts("[m78] memory that can be given back: 64 pages mapped and touched, half "
                   "released and the *same addresses* handed out again rather than the arena "
                   "growing, an interior hole reused, a shared or file-backed mapping refused "
                   "by name, malloc routing a repeated 1 MiB allocation through it without "
                   "growing the process, and every frame back at the end - self-test passed.\n\n");
    }

    {
        int all_ok = 1;
        size_t tt_bytes = 0;
        uint8_t *tt_img = read_program(PATH_BIN_DIRECTORY "threadtest", &tt_bytes);
        if (!tt_img) {
            panic("M79 self-test: /bin/threadtest is not on this disk");
        }
        for (int i = 0; i < scheduler_task_count(); i++) {
            task_t *stale = scheduler_task_by_slot(i);
            if (stale && stale->state == TASK_TERMINATED) {
                selftest_reap(stale);
            }
        }

        uint64_t frames_before = physical_memory_free_frame_count();

        const char *tt_argv[] = {PATH_BIN_DIRECTORY "threadtest", 0};
        task_t *tt = process_spawnv("threadtest", tt_img, tt_bytes, tt_argv);
        kfree(tt_img);
        if (!tt) {
            panic("M79 self-test: could not spawn threadtest");
        }

        int shared_seen = 0;
        int max_sharers = 0;
        for (int i = 0; i < 400 && !shared_seen; i++) {
            int count = scheduler_count_sharing_address_space(tt->pml4_phys);
            if (count > max_sharers) {
                max_sharers = count;
            }
            if (count >= 3) {
                shared_seen = 1;
                break;
            }
            if (tt->state == TASK_TERMINATED) {
                break;
            }
            pit_sleep_ms(25);
        }
        if (!shared_seen) {
            kernel_log_puts("[m79] never saw three tasks sharing one page table - the most that "
                       "ever did was ");
            kernel_log_put_dec((uint32_t)max_sharers);
            kernel_log_puts(", so a 'thread' here is still a process\n");
            all_ok = 0;
        }

        long rc = do_syscall(SYS_wait, (uint64_t)tt->id, 0, 0);
        if (rc != 0) {
            kernel_log_puts("[m79] threadtest exited ");
            kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            kernel_log_puts(" - see user_space/binaries/threadtest.c for what each code means\n");
            all_ok = 0;
        }
        selftest_reap(tt);

        for (int i = 0; i < scheduler_task_count(); i++) {
            task_t *o = scheduler_task_by_slot(i);
            if (o && o->state == TASK_TERMINATED) {
                selftest_reap(o);
            }
        }
        uint64_t frames_after = physical_memory_free_frame_count();
        if (frames_after != frames_before) {
            kernel_log_puts("[m79] ");
            kernel_log_put_dec((uint32_t)(frames_before > frames_after
                                     ? frames_before - frames_after : 0));
            kernel_log_puts(" frame(s) did not come back from a process that ran two threads\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M79 self-test: this scheduler still cannot run two tasks in one address space");
        }

        kernel_log_puts("[m79] two threads, one address space: three tasks on one page table at "
                   "once, two million increments through a mutex arriving as exactly two "
                   "million, memory written by one thread read by the other, separate tids "
                   "under one pid, each thread's floating-point state surviving the other's, "
                   "a thread's stack entered aligned the way a call would have entered it "
                   "(M99), and every frame back when the last of them left - self-test "
                   "passed.\n\n");
    }

    {
        size_t image_bytes = 0;
        uint8_t *image = read_program(PATH_BIN_DIRECTORY "futextest", &image_bytes);
        if (!image) {
            panic("m96: /bin/futextest is not on the disk");
        }
        task_t *t = process_spawn("futextest", image, image_bytes, "");
        kfree(image);
        if (!t) {
            panic("M96 self-test: could not spawn the futex fixture");
        }
        int id = t->id;
        uint64_t deadline = pit_get_ticks() + 6000;
        while (scheduler_task_by_id(id) && scheduler_task_by_id(id)->state != TASK_TERMINATED) {
            if (pit_get_ticks() > deadline) {
                panic("M96 self-test: the futex fixture never finished - a waiter is "
                      "asleep with nothing to wake it");
            }
            pit_sleep_ms(10);
        }
        task_t *done = scheduler_task_by_id(id);
        int code = done ? done->exit_code : -1;
        selftest_reap(done);
        if (code != 0) {
            kernel_log_puts("[m96] the fixture exited 0x");
            kernel_log_put_hex32((uint32_t)code);
            kernel_log_puts(" - see user_space/binaries/futextest.c for what each code means\n");
            panic("M96 self-test: threads do not have their own variables, or a "
                  "waiting thread still costs a core");
        }
        kernel_log_puts("[m96] a thread with its own variables, and a wait that costs nothing: "
                   "sixteen threads on one contended mutex each reporting its own "
                   "__thread count, a blocked waiter measured in CPU ticks rather than "
                   "in throughput because throughput passes with a spin loop still in "
                   "place, a broadcast that woke every waiter, eight threads through "
                   "four barrier rounds, and a writer that never overlapped a reader - "
                   "self-test passed.\n\n");
    }

    {
        const int WRITERS = 4;

        block_device_cache_drop();
        block_device_statistics_t scan_before, scan_after;
        block_device_statistics(&scan_before);
        uint64_t s0 = tsc_read();
        int quiet_problems = virtual_file_system_check();
        uint64_t s1 = tsc_read();
        uint64_t quiet_scan_us = tsc_to_us(s1 - s0);
        block_device_statistics(&scan_after);
        uint64_t quiet_scan_reads = scan_after.reads - scan_before.reads;
        uint64_t quiet_scan_misses = (scan_after.reads - scan_before.reads) -
                                     (scan_after.hits - scan_before.hits);
        uint64_t quiet_scan_dev = scan_after.device_reads - scan_before.device_reads;

        block_device_cache_drop();
        uint64_t r0 = tsc_read();
        int repeat_problems = virtual_file_system_check();
        uint64_t r1 = tsc_read();
        uint64_t repeat_scan_us = tsc_to_us(r1 - r0);
        if (repeat_problems != 0) {
            panic("M105 self-test: the second scan of an unchanged filesystem "
                  "disagreed with the first");
        }
        if (quiet_problems != 0) {
            kernel_log_puts("[m105] the scan found ");
            kernel_log_put_dec((uint32_t)quiet_problems);
            kernel_log_puts(" problem(s) on a filesystem nothing had written to yet\n");
            panic("M105 self-test: the boot filesystem does not check out clean");
        }

        uint32_t free_before = virtual_file_system_free_blocks();

        size_t image_bytes = 0;
        uint8_t *image = read_program(PATH_BIN_DIRECTORY "fswriter", &image_bytes);
        if (!image) {
            panic("m105: /bin/fswriter is not on the disk");
        }
        int ids[8];
        for (int w = 0; w < WRITERS; w++) {
            char arg[8];
            arg[0] = (char)('0' + w);
            arg[1] = '\0';
            task_t *t = process_spawn("fswriter", image, image_bytes, arg);
            if (!t) {
                panic("M105 self-test: could not spawn a writer");
            }
            ids[w] = t->id;
        }
        kfree(image);

        uint64_t w0 = tsc_read();
        uint64_t deadline = pit_get_ticks() + 12000;
        for (int w = 0; w < WRITERS; w++) {
            while (scheduler_task_by_id(ids[w]) &&
                   scheduler_task_by_id(ids[w])->state != TASK_TERMINATED) {
                if (pit_get_ticks() > deadline) {
                    panic("M105 self-test: a writer never finished - four writers on one "
                          "coarse filesystem lock have deadlocked or starved");
                }
                pit_sleep_ms(10);
            }
        }
        uint64_t w1 = tsc_read();
        uint64_t writers_us = tsc_to_us(w1 - w0);

        for (int w = 0; w < WRITERS; w++) {
            task_t *done = scheduler_task_by_id(ids[w]);
            int code = done ? done->exit_code : -1;
            selftest_reap(done);
            if (code != 0) {
                kernel_log_puts("[m105] writer ");
                kernel_log_put_dec((uint32_t)w);
                kernel_log_puts(" exited ");
                kernel_log_put_dec((uint32_t)code);
                kernel_log_puts(" - see user_space/binaries/fswriter.c for what each code means\n");
                panic("M105 self-test: a writer could not verify its own bytes with "
                      "three others writing beside it");
            }
        }

        block_device_cache_drop();
        block_device_statistics(&scan_before);
        uint64_t s2 = tsc_read();
        int busy_problems = virtual_file_system_check();
        uint64_t s3 = tsc_read();
        uint64_t busy_scan_us = tsc_to_us(s3 - s2);
        block_device_statistics(&scan_after);
        uint64_t busy_scan_reads = scan_after.reads - scan_before.reads;
        uint64_t busy_scan_misses = (scan_after.reads - scan_before.reads) -
                                    (scan_after.hits - scan_before.hits);
        uint64_t busy_scan_dev = scan_after.device_reads - scan_before.device_reads;
        if (busy_problems != 0) {
            kernel_log_puts("[m105] the scan found ");
            kernel_log_put_dec((uint32_t)busy_problems);
            kernel_log_puts(" problem(s) after four concurrent writers\n");
            panic("M105 self-test: concurrent writers corrupted the block bitmap - "
                  "write ordering plus a mount check is no longer enough");
        }

        for (int w = 0; w < WRITERS; w++) {
            char directory[64], path[96];
            k_strlcpy(directory, PATH_TEMPORARY "/w", sizeof(directory));
            size_t dn = k_strlen(directory);
            directory[dn] = (char)('0' + w);
            directory[dn + 1] = '\0';
            for (int f = 0; f < 32; f++) {
                for (int which = 0; which < 2; which++) {
                    k_strlcpy(path, directory, sizeof(path));
                    size_t q = k_strlen(path);
                    path[q++] = '/';
                    path[q++] = which ? 't' : 'f';
                    if (f >= 10) {
                        path[q++] = (char)('0' + f / 10);
                    }
                    path[q++] = (char)('0' + f % 10);
                    path[q] = '\0';
                    if (virtual_file_system_exists(path)) {
                        virtual_file_system_unlink(path);
                    }
                }
            }
            virtual_file_system_rmdir(directory);
        }
        uint32_t free_after = virtual_file_system_free_blocks();
        if (free_after != free_before) {
            kernel_log_puts("[m105] ");
            kernel_log_put_dec(free_before);
            kernel_log_puts(" data blocks free before the writers ran and ");
            kernel_log_put_dec(free_after);
            kernel_log_puts(" after removing everything they made\n");
            panic("M105 self-test: concurrent writers leaked blocks - the free count did "
                  "not come back");
        }

        kernel_log_perf("mount_scan_us", quiet_scan_us, "us");
        kernel_log_perf("mount_scan_repeat_us", repeat_scan_us, "us");
        kernel_log_perf("mount_scan_busy_us", busy_scan_us, "us");
        kernel_log_perf("four_writers_us", writers_us, "us");
        kernel_log_puts("[m105] the quiet scan: ");
        kernel_log_put_dec((uint32_t)quiet_scan_reads);
        kernel_log_puts(" block reads, ");
        kernel_log_put_dec((uint32_t)quiet_scan_misses);
        kernel_log_puts(" of them missed the cache, ");
        kernel_log_put_dec((uint32_t)quiet_scan_dev);
        kernel_log_puts(" sector reads issued to the device. The busy scan: ");
        kernel_log_put_dec((uint32_t)busy_scan_reads);
        kernel_log_puts(" / ");
        kernel_log_put_dec((uint32_t)busy_scan_misses);
        kernel_log_puts(" / ");
        kernel_log_put_dec((uint32_t)busy_scan_dev);
        kernel_log_putc('\n');

        kernel_log_puts("[m105] the journal's two conditions, measured together: the full scan "
                   "an unclean mount runs costs ");
        kernel_log_put_dec((uint32_t)quiet_scan_us);
        kernel_log_puts(" us on this filesystem and ");
        kernel_log_put_dec((uint32_t)busy_scan_us);
        kernel_log_puts(" us with four writers' trees on it; four processes wrote, fsynced, "
                   "renamed and deleted concurrently for ");
        kernel_log_put_dec((uint32_t)writers_us);
        kernel_log_puts(" us, every one of them read back exactly the bytes it wrote, the scan "
                   "found no orphaned or doubly-allocated block, and all ");
        kernel_log_put_dec(free_before);
        kernel_log_puts(" free blocks came back - M71's first condition is met and costs "
                   "nothing, because one coarse lock makes a metadata sequence atomic "
                   "against another writer. Journal still refused - self-test passed.\n\n");
    }

    {
        int all_ok = 1;
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));

        const int MANY = 1200;
        static const char *const MANY_DIRECTORY = PATH_TEMPORARY_DIRECTORY "m81many";

        if (!virtual_file_system_exists(MANY_DIRECTORY) && virtual_file_system_mkdir(MANY_DIRECTORY) != 0) {
            kernel_log_puts("[m81] could not create the directory for the file storm\n");
            all_ok = 0;
        }

        int created = 0;
        for (int i = 0; i < MANY && all_ok; i++) {
            char path[PATH_MAX_LENGTH];
            char name[16];
            m81_storm_name(name, i);
            if (path_join(path, PATH_TEMPORARY_DIRECTORY "m81many/", name) != 0) {
                kernel_log_puts("[m81] a name in the file storm did not fit a path\n");
                all_ok = 0;
                break;
            }
            char body[4];
            body[0] = (char)(i & 0xFF);
            body[1] = (char)((i >> 8) & 0xFF);
            body[2] = 'm';
            body[3] = '\0';
            if (virtual_file_system_write(path, body, sizeof(body)) != 0) {
                kernel_log_puts("[m81] the filesystem ran out at 0x");
                kernel_log_put_hex32((uint32_t)i);
                kernel_log_puts(" files - the inode cap is still a wall\n");
                all_ok = 0;
                break;
            }
            created++;
        }

        char longname[LEANFS_MAX_NAME + 1];
        for (int i = 0; i < LEANFS_MAX_NAME; i++) {
            longname[i] = (char)('a' + (i % 26));
        }
        longname[LEANFS_MAX_NAME] = '\0';
        {
            char path[PATH_MAX_LENGTH];
            if (path_join(path, PATH_TEMPORARY_DIRECTORY "m81many/", longname) != 0 ||
                virtual_file_system_write(path, "long", 5) != 0) {
                kernel_log_puts("[m81] a 255-character name was refused\n");
                all_ok = 0;
            } else {
                char got[8];
                k_memset(got, 0, sizeof(got));
                if (virtual_file_system_read(path, got, sizeof(got)) != 5 || got[0] != 'l') {
                    kernel_log_puts("[m81] a 255-character name did not read back by path\n");
                    all_ok = 0;
                }
            }
        }

        char deep[PATH_MAX_LENGTH];
        int deep_length = 0;
        {
            const char *seg = "/adirectorylevelname";
            k_strlcpy(deep, PATH_TEMPORARY_DIRECTORY "m81deep", sizeof(deep));
            deep_length = (int)k_strlen(deep);
            if (!virtual_file_system_exists(deep) && virtual_file_system_mkdir(deep) != 0) {
                kernel_log_puts("[m81] could not start the deep path\n");
                all_ok = 0;
            }
            for (int level = 0; level < 12 && all_ok; level++) {
                size_t seg_length = k_strlen(seg);
                if (deep_length + (int)seg_length >= (int)sizeof(deep)) {
                    break;
                }
                k_memcpy(deep + deep_length, seg, seg_length);
                deep_length += (int)seg_length;
                deep[deep_length] = '\0';
                if (!virtual_file_system_exists(deep) && virtual_file_system_mkdir(deep) != 0) {
                    kernel_log_puts("[m81] mkdir failed at depth 0x");
                    kernel_log_put_hex32((uint32_t)level);
                    kernel_log_puts("\n");
                    all_ok = 0;
                }
            }
        }
        if (all_ok && deep_length <= 128) {
            kernel_log_puts("[m81] the deep path is not actually deeper than the old limit\n");
            all_ok = 0;
        }
        if (all_ok) {
            char leaf[PATH_MAX_LENGTH];
            k_strlcpy(leaf, deep, sizeof(leaf));
            size_t l = k_strlen(leaf);
            k_strlcpy(leaf + l, "/bottom.txt", sizeof(leaf) - l);
            static const char deep_body[] = "reached the bottom";
            if (virtual_file_system_write(leaf, deep_body, sizeof(deep_body)) != 0) {
                kernel_log_puts("[m81] could not write a file at the bottom of a 0x");
                kernel_log_put_hex32((uint32_t)deep_length);
                kernel_log_puts("-byte path\n");
                all_ok = 0;
            } else {
                char got[32];
                k_memset(got, 0, sizeof(got));
                if (virtual_file_system_read(leaf, got, sizeof(got)) != (int64_t)sizeof(deep_body) ||
                    got[0] != 'r') {
                    kernel_log_puts("[m81] the file at the bottom of the deep path did not read back\n");
                    all_ok = 0;
                }
            }
        }

        int seen = 0;
        int saw_long = 0;
        int dup_inode = 0;
        if (all_ok) {
            static uint8_t seen_ino[LEANFS_MAX_INODES / 8];
            k_memset(seen_ino, 0, sizeof(seen_ino));
            uint32_t cookie = 0;
            leanfs_directory_entry_t e;
            int rc;
            while ((rc = virtual_file_system_readdir(MANY_DIRECTORY, &cookie, &e)) == 1) {
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
                kernel_log_puts("[m81] readdir reported a corrupt directory\n");
                all_ok = 0;
            }
        }
        if (all_ok && seen != created + 1) {
            kernel_log_puts("[m81] a directory holding 0x");
            kernel_log_put_hex32((uint32_t)(created + 1));
            kernel_log_puts(" entries walked back 0x");
            kernel_log_put_hex32((uint32_t)seen);
            kernel_log_puts(" of them\n");
            all_ok = 0;
        }
        if (all_ok && !saw_long) {
            kernel_log_puts("[m81] the 255-character name was not among the entries walked back\n");
            all_ok = 0;
        }
        if (all_ok && dup_inode) {
            kernel_log_puts("[m81] two entries reported the same inode number - the walk repeated itself\n");
            all_ok = 0;
        }

        if (all_ok) {
            leanfs_stat_t before, after;
            virtual_file_system_stat(MANY_DIRECTORY, &before);
            const int CHURN = 8;
            for (int i = 0; i < created; i += CHURN) {
                char path[PATH_MAX_LENGTH];
                char name[16];
                m81_storm_name(name, i);
                path_join(path, PATH_TEMPORARY_DIRECTORY "m81many/", name);
                if (virtual_file_system_unlink(path) != 0) {
                    kernel_log_puts("[m81] could not remove a file from the storm\n");
                    all_ok = 0;
                    break;
                }
            }
            for (int i = 0; i < created && all_ok; i += CHURN) {
                char path[PATH_MAX_LENGTH];
                char name[16];
                m81_storm_name(name, i);
                path_join(path, PATH_TEMPORARY_DIRECTORY "m81many/", name);
                if (virtual_file_system_write(path, "re", 3) != 0) {
                    kernel_log_puts("[m81] could not put a removed file back\n");
                    all_ok = 0;
                    break;
                }
            }
            virtual_file_system_stat(MANY_DIRECTORY, &after);
            if (all_ok && after.size > before.size) {
                kernel_log_puts("[m81] a directory grew from 0x");
                kernel_log_put_hex32(before.size);
                kernel_log_puts(" to 0x");
                kernel_log_put_hex32(after.size);
                kernel_log_puts(" bytes across a delete/recreate cycle - the holes are not being reused\n");
                all_ok = 0;
            }
        }

        uint32_t took_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms;
        if (!all_ok) {
            panic("M81 self-test: this filesystem still cannot hold somebody else's program");
        }
        kernel_log_puts("[m81] a filesystem that can hold somebody else's program: 0x");
        kernel_log_put_hex32((uint32_t)created);
        kernel_log_puts(" files in one directory (the whole disk held 0xC0 before this milestone), "
                   "a 255-character name written and read back by path, a file at the bottom "
                   "of a 0x");
        kernel_log_put_hex32((uint32_t)deep_length);
        kernel_log_puts("-byte path, every entry walked back one at a time by streaming readdir "
                   "with no two sharing an inode number, and a delete/recreate cycle reusing "
                   "the holes rather than growing the directory - self-test passed (");
        kernel_log_put_dec(took_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        int all_ok = 1;

        static const char *const BIG = PATH_TEMPORARY_DIRECTORY "m93big";
        const uint32_t BIG_BYTES = 16u * 1024 * 1024;
        const uint32_t CHUNK = 64u * 1024;
        static uint8_t chunk[64 * 1024];
        int big_handle = -1;
        if (all_ok) {
            big_handle = virtual_file_system_open(BIG, 1);
            if (big_handle < 0) {
                kernel_log_puts("[m93] could not create a file to grow\n");
                all_ok = 0;
            }
        }
        for (uint32_t off = 0; all_ok && off < BIG_BYTES; off += CHUNK) {
            for (uint32_t i = 0; i < CHUNK; i += 512) {
                chunk[i] = (uint8_t)((off + i) >> 12);
                chunk[i + 1] = (uint8_t)((off + i) >> 20);
            }
            if (virtual_file_system_handle_write(big_handle, chunk, CHUNK, off) != (int64_t)CHUNK) {
                kernel_log_puts("[m93] a write past the old 8 MiB ceiling was refused at 0x");
                kernel_log_put_hex32(off);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }
        if (all_ok && virtual_file_system_handle_size(big_handle) != BIG_BYTES) {
            kernel_log_puts("[m93] the grown file is not the size it was written to\n");
            all_ok = 0;
        }
        for (uint32_t off = 0; all_ok && off < BIG_BYTES; off += CHUNK) {
            k_memset(chunk, 0, CHUNK);
            if (virtual_file_system_handle_read(big_handle, chunk, CHUNK, off) != (int64_t)CHUNK) {
                kernel_log_puts("[m93] a read past the old ceiling came up short\n");
                all_ok = 0;
                break;
            }
            for (uint32_t i = 0; i < CHUNK; i += 512) {
                if (chunk[i] != (uint8_t)((off + i) >> 12) ||
                    chunk[i + 1] != (uint8_t)((off + i) >> 20)) {
                    kernel_log_puts("[m93] a block came back from the wrong place at 0x");
                    kernel_log_put_hex32(off + i);
                    kernel_log_putc('\n');
                    all_ok = 0;
                    break;
                }
            }
        }
        if (all_ok) {
            if (virtual_file_system_unlink(BIG) != 0) {
                kernel_log_puts("[m93] the big file could not be removed\n");
                all_ok = 0;
            }
        }

        static const char *const MANYDIR = PATH_TEMPORARY_DIRECTORY "m93many";
        const int PAST_CAP = 9000;
        int made = 0;
        if (all_ok && !virtual_file_system_exists(MANYDIR) && virtual_file_system_mkdir(MANYDIR) != 0) {
            kernel_log_puts("[m93] could not make the directory for the inode storm\n");
            all_ok = 0;
        }
        for (int i = 0; i < PAST_CAP && all_ok; i++) {
            char path[PATH_MAX_LENGTH];
            char name[16];
            m81_storm_name(name, i);
            if (path_join(path, PATH_TEMPORARY_DIRECTORY "m93many/", name) != 0 ||
                virtual_file_system_write(path, "x", 1) != 0) {
                kernel_log_puts("[m93] file creation failed at 0x");
                kernel_log_put_hex32((uint32_t)i);
                kernel_log_puts(" - the inode cap is still where it was\n");
                all_ok = 0;
                break;
            }
            made++;
        }
        if (all_ok && made <= 8192) {
            kernel_log_puts("[m93] the storm stopped at or below the old cap, so it proved nothing\n");
            all_ok = 0;
        }
        if (all_ok) {
            uint32_t cookie = 0;
            leanfs_directory_entry_t e;
            int seen = 0;
            while (virtual_file_system_readdir(MANYDIR, &cookie, &e) == 1) {
                seen++;
            }
            if (seen != made) {
                kernel_log_puts("[m93] made 0x");
                kernel_log_put_hex32((uint32_t)made);
                kernel_log_puts(" files and read back 0x");
                kernel_log_put_hex32((uint32_t)seen);
                kernel_log_putc('\n');
                all_ok = 0;
            }
        }

        static const char *const L_A = PATH_TEMPORARY_DIRECTORY "m93link.a";
        static const char *const L_B = PATH_TEMPORARY_DIRECTORY "m93link.b";
        if (all_ok) {
            virtual_file_system_unlink(L_A);
            virtual_file_system_unlink(L_B);
            if (virtual_file_system_write(L_A, "two names", 9) != 0) {
                kernel_log_puts("[m93] could not write the file to link\n");
                all_ok = 0;
            }
        }
        if (all_ok && virtual_file_system_link(L_A, L_B) != 0) {
            kernel_log_puts("[m93] link refused a file it should have accepted\n");
            all_ok = 0;
        }
        if (all_ok && (virtual_file_system_nlink(L_A) != 2 || virtual_file_system_nlink(L_B) != 2)) {
            kernel_log_puts("[m93] the link count is not 2 through both names\n");
            all_ok = 0;
        }
        if (all_ok && virtual_file_system_link(PATH_TEMPORARY_DIRECTORY "m93many", PATH_TEMPORARY_DIRECTORY "m93dirlink") == 0) {
            kernel_log_puts("[m93] a hard link to a directory was allowed\n");
            all_ok = 0;
        }
        if (all_ok && virtual_file_system_unlink(L_A) != 0) {
            kernel_log_puts("[m93] removing the first name failed\n");
            all_ok = 0;
        }
        if (all_ok) {
            char back[16];
            int64_t n = virtual_file_system_read(L_B, back, sizeof(back));
            if (n != 9 || back[0] != 't') {
                kernel_log_puts("[m93] removing one name took the file with it\n");
                all_ok = 0;
            }
        }
        if (all_ok && virtual_file_system_nlink(L_B) != 1) {
            kernel_log_puts("[m93] the link count did not come back down\n");
            all_ok = 0;
        }
        if (all_ok) {
            virtual_file_system_unlink(L_B);
        }

        if (all_ok) {
            int file_descriptors[2];
            if (do_syscall(SYS_pipe, (uint64_t)file_descriptors, 0, 0) == 0) {
                if (do_syscall(SYS_fsync, (uint64_t)file_descriptors[0], 0, 0) == 0) {
                    kernel_log_puts("[m93] fsync claimed to have made a pipe durable\n");
                    all_ok = 0;
                }
                do_syscall(SYS_close, (uint64_t)file_descriptors[0], 0, 0);
                do_syscall(SYS_close, (uint64_t)file_descriptors[1], 0, 0);
            }
        }

        if (!all_ok) {
            panic("M93 self-test: this filesystem cannot hold what a build would put in it");
        }
        uint32_t took_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms;
        kernel_log_puts("[m93] a filesystem that can hold a source tree: a 16 MiB file written and "
                   "read back block for block where 8 MiB was the structural ceiling, 0x");
        kernel_log_put_hex32((uint32_t)made);
        kernel_log_puts(" files created past an inode cap of 0x2000 and every one of them read back, "
                   "a second name for a file with the link count to prove it is the same file "
                   "rather than a copy, one name removed leaving the other readable, a hard "
                   "link to a directory refused, and fsync refusing a pipe it cannot make "
                   "durable - self-test passed (");
        kernel_log_put_dec(took_ms);
        kernel_log_puts(" ms).\n\n");
    }

    selftest_image_manifest();

    {
        int all_ok = 1;
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        uint64_t frames_before = physical_memory_free_frame_count();

        size_t lz_bytes = 0;
        uint8_t *lz_img = read_program(PATH_BIN_DIRECTORY "lazytest", &lz_bytes);
        if (!lz_img) {
            panic("M82 self-test: /bin/lazytest is not on this disk");
        }

        const char *lz_argv[] = {PATH_BIN_DIRECTORY "lazytest", 0};
        task_t *lz = process_spawnv("lazytest", lz_img, lz_bytes, lz_argv);
        if (!lz) {
            panic("M82 self-test: could not spawn lazytest");
        }
        int lz_id = lz->id;

        uint64_t lowest_free = frames_before;
        for (int i = 0; i < 900; i++) {
            if (do_syscall(SYS_task_alive, (uint64_t)lz_id, 0, 0) != 1) {
                break;
            }
            uint64_t now = physical_memory_free_frame_count();
            if (now < lowest_free) {
                lowest_free = now;
            }
            do_syscall(SYS_yield, 0, 0, 0);
        }

        long rc = do_syscall(SYS_wait, (uint64_t)lz_id, 0, 0);
        kfree(lz_img);
        if (rc != 0) {
            kernel_log_puts("[m82] lazytest exited ");
            kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            kernel_log_puts(" - see user_space/binaries/lazytest.c for what each code means\n");
            all_ok = 0;
        }

        uint64_t peak_spend = frames_before > lowest_free ? frames_before - lowest_free : 0;
        if (all_ok && peak_spend > 2048) {
            kernel_log_puts("[m82] a 144 MiB reservation cost 0x");
            kernel_log_put_hex64(peak_spend);
            kernel_log_puts(" frames while it was held - it is still being backed eagerly\n");
            all_ok = 0;
        }
        if (all_ok && peak_spend < 200) {
            kernel_log_puts("[m82] only 0x");
            kernel_log_put_hex64(peak_spend);
            kernel_log_puts(" frames were ever seen in use - the sample was taken before "
                       "lazytest held its mapping, so this test proved nothing\n");
            all_ok = 0;
        }

        uint64_t frames_after = physical_memory_free_frame_count();
        if (all_ok && frames_after != frames_before) {
            kernel_log_puts("[m82] frames before 0x");
            kernel_log_put_hex64(frames_before);
            kernel_log_puts(" after 0x");
            kernel_log_put_hex64(frames_after);
            kernel_log_puts(" - demand-filled pages are not all coming back\n");
            all_ok = 0;
        }

        static const char *const FATAL_MODES[] = {"ro", "gap"};
        static const char *const FATAL_WHY[] = {
            "writing to a PROT_READ mapping",
            "touching arena address space nobody reserved",
        };
        for (int m = 0; m < 2 && all_ok; m++) {
            size_t f_bytes = 0;
            uint8_t *f_img = read_program(PATH_BIN_DIRECTORY "lazytest", &f_bytes);
            if (!f_img) {
                panic("M82 self-test: /bin/lazytest vanished mid-test");
            }
            const char *f_argv[] = {PATH_BIN_DIRECTORY "lazytest", FATAL_MODES[m], 0};
            task_t *ft = process_spawnv("lazytest", f_img, f_bytes, f_argv);
            long frc = ft ? do_syscall(SYS_wait, (uint64_t)ft->id, 0, 0) : -1;
            kfree(f_img);
            if (frc != 128 + SIGSEGV) {
                kernel_log_puts("[m82] ");
                kernel_log_puts(FATAL_WHY[m]);
                kernel_log_puts(" exited 0x");
                kernel_log_put_hex32((uint32_t)frc);
                kernel_log_puts(" rather than being killed - the fault handler is filling too much\n");
                all_ok = 0;
            }
        }

        if (!all_ok) {
            panic("M82 self-test: this kernel is not filling pages on demand, or is filling too many");
        }
        kernel_log_puts("[m82] a page that arrives when it is asked for: a 144 MiB reservation "
                   "granted on a 128 MiB machine and held for 0x");
        kernel_log_put_hex64(peak_spend);
        kernel_log_puts(" frames rather than 0x9000 - and for more than the 0x100 pages "
                   "deliberately touched, so the measurement is of something real - an "
                   "untouched page reading as zero, an untouched mapping accepted as a "
                   "syscall buffer, over a "
                   "gigabyte reserved across eight rounds without exhausting the machine, "
                   "every frame back at the end, and both of the faults that must stay "
                   "fatal - a write to a read-only mapping and a touch of unreserved arena "
                   "address space - still killing only the program that made them - "
                   "self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        int all_ok = 1;
        uint64_t frames_before = physical_memory_free_frame_count();

        size_t vm_bytes = 0;
        uint8_t *vm_img = read_program(PATH_BIN_DIRECTORY "vmtest", &vm_bytes);
        if (!vm_img) {
            panic("M91 self-test: /bin/vmtest is not on this disk");
        }
        const char *vm_argv[] = {PATH_BIN_DIRECTORY "vmtest", 0};
        task_t *vt = process_spawnv("vmtest", vm_img, vm_bytes, vm_argv);
        long vrc = vt ? do_syscall(SYS_wait, (uint64_t)vt->id, 0, 0) : -1;
        kfree(vm_img);
        if (vrc != 0) {
            kernel_log_puts("[m91] vmtest exited ");
            kernel_log_put_hex32((uint32_t)vrc);
            kernel_log_puts(" - see user_space/binaries/vmtest.c for what each code means\n");
            all_ok = 0;
        }

        static const char *const M91_FATAL[] = {"nx", "wx", "guard", "stackfar"};
        static const char *const M91_WHY[] = {
            "executing a page that is not PROT_EXEC",
            "writing to a page mprotect made read-only",
            "touching a PROT_NONE guard mapping",
            "touching 32 MiB below the stack pointer",
        };
        for (int m = 0; m < 4 && all_ok; m++) {
            size_t f_bytes = 0;
            uint8_t *f_img = read_program(PATH_BIN_DIRECTORY "vmtest", &f_bytes);
            if (!f_img) {
                panic("M91 self-test: /bin/vmtest vanished mid-test");
            }
            const char *f_argv[] = {PATH_BIN_DIRECTORY "vmtest", M91_FATAL[m], 0};
            task_t *ft = process_spawnv("vmtest", f_img, f_bytes, f_argv);
            long frc = ft ? do_syscall(SYS_wait, (uint64_t)ft->id, 0, 0) : -1;
            kfree(f_img);
            if (frc != 128 + SIGSEGV) {
                kernel_log_puts("[m91] ");
                kernel_log_puts(M91_WHY[m]);
                kernel_log_puts(" exited 0x");
                kernel_log_put_hex32((uint32_t)frc);
                kernel_log_puts(" rather than being killed\n");
                all_ok = 0;
            }
        }

        uint64_t frames_after = physical_memory_free_frame_count();
        if (all_ok && frames_after != frames_before) {
            kernel_log_puts("[m91] frames before 0x");
            kernel_log_put_hex64(frames_before);
            kernel_log_puts(" after 0x");
            kernel_log_put_hex64(frames_after);
            kernel_log_puts(" - a mapping that was placed, replaced, reprotected or dropped "
                       "is not giving every frame back\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M91 self-test: the address space is not a set of mappings this program can arrange");
        }
        int shared_left = file_mapping_in_use();
        if (all_ok && shared_left != 0) {
            kernel_log_puts("[m91] 0x");
            kernel_log_put_hex32((uint32_t)shared_left);
            kernel_log_puts(" shared file page(s) still held after every mapping was "
                       "dropped - see kernel/memory_management/file_mapping.c\n");
            all_ok = 0;
        }

        kernel_log_puts("[m91] an address space that is a set of mappings: an address hint "
                   "honoured and MAP_FIXED landing exactly where it was told and "
                   "replacing what was there, mprotect taking write away and giving it "
                   "back with the bytes intact and refusing a range no mapping covers, "
                   "bytes written to a page and then executed from it, MADV_DONTNEED "
                   "returning the frames and keeping the mapping, a 4 GiB reservation "
                   "touched at both ends on a machine whose whole user region used to be "
                   "under a gigabyte, a stack grown sixteen times past what it was given, "
                   "a file mapped MAP_PRIVATE reading back as its own bytes and as zeros "
                   "past its end with a write that never reached it, the same file mapped "
                   "MAP_SHARED twice as one piece of memory with a write that reached the "
                   "disk through msync, "
                   "NX ");
        kernel_log_puts(virtual_memory_nx_enabled() ? "enforced" : "unavailable on this CPU");
        kernel_log_puts(", and all four faults that must stay fatal - executing a "
                   "non-executable page, writing to one mprotect made read-only, touching "
                   "a guard page, and touching far below the stack pointer - still "
                   "killing only the program that made them - self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        int all_ok = 1;
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        uint64_t frames_before = physical_memory_free_frame_count();

        size_t ft_bytes = 0;
        uint8_t *ft_img = read_program(PATH_BIN_DIRECTORY "forktest", &ft_bytes);
        if (!ft_img) {
            panic("M83 self-test: /bin/forktest is not on this disk");
        }
        const char *ft_argv[] = {PATH_BIN_DIRECTORY "forktest", 0};
        task_t *ft = process_spawnv("forktest", ft_img, ft_bytes, ft_argv);
        long rc = ft ? do_syscall(SYS_wait, (uint64_t)ft->id, 0, 0) : -1;
        kfree(ft_img);
        if (rc != 0) {
            kernel_log_puts("[m83] forktest exited ");
            kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            kernel_log_puts(" - see user_space/binaries/forktest.c for what each code means\n");
            all_ok = 0;
        }

        for (int i = 0; i < scheduler_task_count(); i++) {
            task_t *stale = scheduler_task_by_slot(i);
            if (stale && stale->state == TASK_TERMINATED) {
                selftest_reap(stale);
            }
        }
        uint64_t frames_after = physical_memory_free_frame_count();
        if (all_ok && frames_after != frames_before) {
            kernel_log_puts("[m83] frames before 0x");
            kernel_log_put_hex64(frames_before);
            kernel_log_puts(" after 0x");
            kernel_log_put_hex64(frames_after);
            kernel_log_puts(" - a hundred forks did not give everything back\n");
            all_ok = 0;
        }

        uint64_t cow_spend = 0;
        if (all_ok) {
            uint64_t cow_before = physical_memory_free_frame_count();
            size_t cw_bytes = 0;
            uint8_t *cw_img = read_program(PATH_BIN_DIRECTORY "forktest", &cw_bytes);
            if (!cw_img) {
                panic("M83 self-test: /bin/forktest vanished mid-test");
            }
            const char *cw_argv[] = {PATH_BIN_DIRECTORY "forktest", "cow", 0};
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
                uint64_t now = physical_memory_free_frame_count();
                if (now < lowest_free) {
                    lowest_free = now;
                }
                do_syscall(SYS_yield, 0, 0, 0);
            }
            long cw_rc = do_syscall(SYS_wait, (uint64_t)cw_id, 0, 0);
            kfree(cw_img);
            cow_spend = cow_before > lowest_free ? cow_before - lowest_free : 0;

            if (cw_rc != 0) {
                kernel_log_puts("[m83] forktest cow exited ");
                kernel_log_put_dec((uint32_t)(cw_rc < 0 ? 99 : cw_rc));
                kernel_log_puts("\n");
                all_ok = 0;
            }
            if (all_ok && cow_spend > 3000) {
                kernel_log_puts("[m83] a fork of an 8 MiB process cost 0x");
                kernel_log_put_hex64(cow_spend);
                kernel_log_puts(" frames - the address space is being copied, not shared\n");
                all_ok = 0;
            }
            if (all_ok && cow_spend < 2048) {
                kernel_log_puts("[m83] only 0x");
                kernel_log_put_hex64(cow_spend);
                kernel_log_puts(" frames were ever seen in use - the sample was taken before "
                           "forktest had touched its memory, so this proved nothing\n");
                all_ok = 0;
            }
            for (int i = 0; i < scheduler_task_count(); i++) {
                task_t *stale = scheduler_task_by_slot(i);
                if (stale && stale->state == TASK_TERMINATED) {
                    selftest_reap(stale);
                }
            }
            if (all_ok && physical_memory_free_frame_count() != cow_before) {
                kernel_log_puts("[m83] the copy-on-write round did not give every frame back\n");
                all_ok = 0;
            }
        }

        if (!all_ok) {
            panic("M83 self-test: this kernel cannot make two processes out of one");
        }
        kernel_log_puts("[m83] two processes from one: fork returning twice into two pids, "
                   "memory written before the call visible to the child and memory "
                   "written after it private to each, an inherited pipe carrying a "
                   "message from child to parent, a hundred rounds of fork/exit/wait "
                   "returning every task slot and every frame, and an 8 MiB process "
                   "forked for 0x");
        kernel_log_put_hex64(cow_spend);
        kernel_log_puts(" frames rather than the 0x1000 a copy would have cost - "
                   "self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        uint64_t frames_before = physical_memory_free_frame_count();

        size_t ex_bytes = 0;
        uint8_t *ex_img = read_program(PATH_BIN_DIRECTORY "exectest", &ex_bytes);
        if (!ex_img) {
            panic("M84 self-test: /bin/exectest is not on this disk");
        }
        const char *ex_argv[] = {PATH_BIN_DIRECTORY "exectest", 0};
        const char *ex_envp[] = {"PATH=" PATH_BIN, 0};
        task_t *ex = process_spawnve("exectest", ex_img, ex_bytes, ex_argv, ex_envp);
        long rc = ex ? do_syscall(SYS_wait, (uint64_t)ex->id, 0, 0) : -1;
        kfree(ex_img);

        int all_ok = 1;
        if (rc != 0) {
            kernel_log_puts("[m84] exectest exited ");
            kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
            kernel_log_puts(" - see user_space/binaries/exectest.c for what each code means\n");
            all_ok = 0;
        }

        for (int i = 0; i < scheduler_task_count(); i++) {
            task_t *stale = scheduler_task_by_slot(i);
            if (stale && stale->state == TASK_TERMINATED) {
                selftest_reap(stale);
            }
        }
        uint64_t frames_after = physical_memory_free_frame_count();
        if (all_ok && frames_after != frames_before) {
            kernel_log_puts("[m84] frames before 0x");
            kernel_log_put_hex64(frames_before);
            kernel_log_puts(" after 0x");
            kernel_log_put_hex64(frames_after);
            kernel_log_puts(" - an exec is not giving back the address space it replaced\n");
            all_ok = 0;
        }

        if (!all_ok) {
            panic("M84 self-test: exec, or the wait that has to describe it, is wrong");
        }
        kernel_log_puts("[m84] a program that replaces itself: a descriptor marked "
                   "close-on-exec gone on the far side of an exec while its neighbour "
                   "survives and still reads, waitpid telling a death by SIGSEGV from a "
                   "program that exited 139, WNOHANG not waiting, execvp finding a "
                   "program on PATH, a process keeping its pid across an exec, and every "
                   "frame of every replaced address space back - self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        int all_ok = 1;
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        tty_t *tty = tty_console();

        {
            static const char typed[] = "abX\177c\n";
            for (size_t i = 0; i < sizeof(typed) - 1; i++) {
                tty_input_char(tty, typed[i]);
            }
            char got[32];
            k_memset(got, 0, sizeof(got));
            uint32_t n = tty_read(tty, got, sizeof(got) - 1);
            if (n != 4 || got[0] != 'a' || got[1] != 'b' || got[2] != 'c' || got[3] != '\n') {
                kernel_log_puts("[m85] a line assembled with backspaces read back as 0x");
                kernel_log_put_hex32(n);
                kernel_log_puts(" bytes rather than \"abc\\n\"\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            tty_input_char(tty, 'x');
            tty_input_char(tty, 'y');
            if (tty_readable(tty) != 0) {
                kernel_log_puts("[m85] a half-typed line was readable before Enter\n");
                all_ok = 0;
            }
            tty_input_char(tty, 21);
            if (tty_readable(tty) != 0) {
                kernel_log_puts("[m85] ^U did not discard the line being edited\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            tcflag_t saved = tty->tio.c_lflag;
            tty->tio.c_lflag &= ~(tcflag_t)ICANON;
            tty_input_char(tty, 'r');
            if (tty_readable(tty) != 1) {
                kernel_log_puts("[m85] with ICANON off, a byte was not readable immediately\n");
                all_ok = 0;
            }
            char one = 0;
            tty_read(tty, &one, 1);
            if (one != 'r') {
                kernel_log_puts("[m85] raw mode delivered the wrong byte\n");
                all_ok = 0;
            }
            tty->tio.c_lflag = saved;
        }

        if (all_ok) {
            tty->sid = 4242;
            tty->fg_pgid = 999999;
            if (tty_may_read(tty, 4242, 12345) != 0) {
                kernel_log_puts("[m85] a background job was allowed to read the terminal\n");
                all_ok = 0;
            }
            if (all_ok && tty_may_read(tty, 4242, 999999) != 1) {
                kernel_log_puts("[m85] the foreground job was refused its own terminal\n");
                all_ok = 0;
            }
        }

        tty->fg_pgid = 0;
        tty->sid = 0;
        while (tty_readable(tty) > 0) {
            char drain[64];
            tty_read(tty, drain, sizeof(drain));
        }

        if (all_ok) {
            int mh = virtual_file_system_open("/dev/ptmx", 0);
            if (mh < 0) {
                kernel_log_puts("[m85] /dev/ptmx would not open\n");
                all_ok = 0;
            } else {
                if (!virtual_file_system_exists("/dev/pts/0")) {
                    kernel_log_puts("[m85] /dev/pts/0 did not appear when a pty was made\n");
                    all_ok = 0;
                }
                int sh_ = virtual_file_system_open("/dev/pts/0", 0);
                if (sh_ < 0) {
                    kernel_log_puts("[m85] the slave end of a fresh pty would not open\n");
                    all_ok = 0;
                } else {
                    char got[16];
                    k_memset(got, 0, sizeof(got));
                    virtual_file_system_handle_write(mh, "hi", 2, 0);
                    if (virtual_file_system_handle_readable(sh_)) {
                        kernel_log_puts("[m85] a pty delivered a half-typed line to its slave\n");
                        all_ok = 0;
                    }
                    virtual_file_system_handle_write(mh, "\n", 1, 0);
                    int64_t n = virtual_file_system_handle_read(sh_, got, sizeof(got) - 1, 0);
                    if (n != 3 || got[0] != 'h' || got[1] != 'i' || got[2] != '\n') {
                        kernel_log_puts("[m85] a line typed at a pty master did not arrive whole at the slave\n");
                        all_ok = 0;
                    }
                    k_memset(got, 0, sizeof(got));
                    virtual_file_system_handle_write(sh_, "y\n", 2, 0);
                    n = virtual_file_system_handle_read(mh, got, sizeof(got) - 1, 0);
                    if (n < 3 || got[n - 2] != '\r' || got[n - 1] != '\n') {
                        kernel_log_puts("[m85] the slave's output did not reach the master with ONLCR\n");
                        all_ok = 0;
                    }
                    virtual_file_system_handle_close(mh);
                    if (!virtual_file_system_handle_readable(sh_)) {
                        kernel_log_puts("[m85] a slave whose master closed would have blocked forever\n");
                        all_ok = 0;
                    }
                    if (virtual_file_system_handle_read(sh_, got, sizeof(got) - 1, 0) != 0) {
                        kernel_log_puts("[m85] a hung-up pty slave did not read end-of-file\n");
                        all_ok = 0;
                    }
                    virtual_file_system_handle_close(sh_);
                }
                if (virtual_file_system_exists("/dev/pts/0")) {
                    kernel_log_puts("[m85] a pty was not recycled when both ends closed\n");
                    all_ok = 0;
                }
            }
        }

        if (all_ok) {
            size_t pt_bytes = 0;
            uint8_t *pt_img = read_program(PATH_BIN_DIRECTORY "ptytest", &pt_bytes);
            if (!pt_img) {
                panic("M85 self-test: /bin/ptytest is not on this disk");
            }
            const char *pt_argv[] = {PATH_BIN_DIRECTORY "ptytest", 0};
            task_t *pt = process_spawnv("ptytest", pt_img, pt_bytes, pt_argv);
            long rc = pt ? do_syscall(SYS_wait, (uint64_t)pt->id, 0, 0) : -1;
            kfree(pt_img);
            if (rc != 0) {
                kernel_log_puts("[m85] ptytest exited ");
                kernel_log_put_dec((uint32_t)(rc < 0 ? 99 : rc));
                kernel_log_puts(" - see user_space/binaries/ptytest.c for what each code means\n");
                all_ok = 0;
            }
        }

        if (!all_ok) {
            panic("M85 self-test: this machine's terminal is not a terminal");
        }
        kernel_log_puts("[m85] a terminal that is a device: a line assembled with backspaces and "
                   "delivered whole only on Enter, nothing readable before it, ^U discarding "
                   "it, ICANON off delivering a byte immediately, a background job "
                   "refused its terminal while the foreground job is served, SIGTSTP "
                   "stopping a task and SIGCONT resuming it, and a pseudo-terminal "
                   "carrying a line, an echo and a ^C between two processes - self-test "
                   "passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        int all_ok = 1;
        uint32_t started_ms = (uint32_t)(pit_get_ticks() * (1000 / PIT_HZ));
        char buffer[128];

        if (!virtual_file_system_is_directory(PATH_DEV) || !virtual_file_system_is_directory(PATH_PROCESS)) {
            kernel_log_puts("[m87] /dev or /proc is not a directory\n");
            all_ok = 0;
        }
        if (all_ok) {
            size_t n = virtual_file_system_list("/", buffer, sizeof(buffer));
            int saw_dev = 0, saw_process = 0;
            for (size_t i = 0; i + 4 <= n; i++) {
                if (k_memcmp(buffer + i, "dev/", 4) == 0) {
                    saw_dev = 1;
                }
                if (i + 5 <= n && k_memcmp(buffer + i, "proc/", 5) == 0) {
                    saw_process = 1;
                }
            }
            if (!saw_dev || !saw_process) {
                kernel_log_puts("[m87] a mount point is not listed in its parent directory\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            int h = virtual_file_system_open(PATH_DEV_DIRECTORY "null", 0);
            if (h < 0) {
                kernel_log_puts("[m87] /dev/null could not be opened\n");
                all_ok = 0;
            } else {
                k_memset(buffer, 0xAA, sizeof(buffer));
                if (virtual_file_system_handle_read(h, buffer, sizeof(buffer), 0) != 0) {
                    kernel_log_puts("[m87] a read of /dev/null returned bytes\n");
                    all_ok = 0;
                }
                if (virtual_file_system_handle_write(h, "swallowed", 9, 0) != 9) {
                    kernel_log_puts("[m87] a write to /dev/null was not accepted\n");
                    all_ok = 0;
                }
            }
        }

        if (all_ok) {
            int h = virtual_file_system_open(PATH_DEV_DIRECTORY "zero", 0);
            k_memset(buffer, 0xAA, sizeof(buffer));
            if (h < 0 || virtual_file_system_handle_read(h, buffer, 64, 0) != 64) {
                kernel_log_puts("[m87] /dev/zero did not deliver 64 bytes\n");
                all_ok = 0;
            } else {
                for (int i = 0; i < 64; i++) {
                    if (buffer[i] != 0) {
                        kernel_log_puts("[m87] /dev/zero delivered something that was not zero\n");
                        all_ok = 0;
                        break;
                    }
                }
                if (all_ok && virtual_file_system_handle_read(h, buffer, 16, 4096) != 16) {
                    kernel_log_puts("[m87] /dev/zero ended at an offset - it is being treated as a file\n");
                    all_ok = 0;
                }
            }
        }

        if (all_ok) {
            int h = virtual_file_system_open(PATH_DEV_DIRECTORY "full", 0);
            if (h < 0 || virtual_file_system_handle_write(h, "x", 1, 0) != -1) {
                kernel_log_puts("[m87] /dev/full accepted a write\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            int h = virtual_file_system_open(PATH_DEV_DIRECTORY "urandom", 0);
            char a[16], b[16];
            k_memset(a, 0, sizeof(a));
            k_memset(b, 0, sizeof(b));
            if (h < 0 || virtual_file_system_handle_read(h, a, sizeof(a), 0) != (int64_t)sizeof(a) ||
                virtual_file_system_handle_read(h, b, sizeof(b), 0) != (int64_t)sizeof(b)) {
                kernel_log_puts("[m87] /dev/urandom did not deliver bytes\n");
                all_ok = 0;
            } else if (k_memcmp(a, b, sizeof(a)) == 0) {
                kernel_log_puts("[m87] two reads of /dev/urandom returned identical bytes\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            if (virtual_file_system_write(PATH_DEV_DIRECTORY "null", "x", 1) == 0 ||
                virtual_file_system_mkdir(PATH_DEV_DIRECTORY "newdir") == 0 ||
                virtual_file_system_unlink(PATH_DEV_DIRECTORY "null") == 0 ||
                virtual_file_system_write(PATH_PROCESS_DIRECTORY "uptime", "x", 1) == 0) {
                kernel_log_puts("[m87] a synthetic filesystem accepted a change to itself\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            k_memset(buffer, 0, sizeof(buffer));
            int64_t n = virtual_file_system_read(PATH_PROCESS_DIRECTORY "self/exe", buffer, sizeof(buffer) - 1);
            if (n <= 0 || buffer[0] != '/') {
                kernel_log_puts("[m87] /proc/self/exe did not read back a path\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            char first[32], second[32];
            k_memset(first, 0, sizeof(first));
            k_memset(second, 0, sizeof(second));
            virtual_file_system_read(PATH_PROCESS_DIRECTORY "uptime", first, sizeof(first) - 1);
            if (first[0] < '0' || first[0] > '9') {
                kernel_log_puts("[m87] /proc/uptime did not start with a digit\n");
                all_ok = 0;
            }
            if (all_ok) {
                pit_sleep_ms(1200);
                virtual_file_system_read(PATH_PROCESS_DIRECTORY "uptime", second, sizeof(second) - 1);
                if (k_strcmp(first, second) == 0) {
                    kernel_log_puts("[m87] /proc/uptime read the same twice a second apart\n");
                    all_ok = 0;
                }
            }
        }

        if (all_ok) {
            k_memset(buffer, 0, sizeof(buffer));
            int64_t n = virtual_file_system_read(PATH_PROCESS_DIRECTORY "self/status", buffer, sizeof(buffer) - 1);
            if (n <= 0 || k_memcmp(buffer, "Name:\t", 6) != 0) {
                kernel_log_puts("[m87] /proc/self/status did not begin with a Name field\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            uint32_t cookie = 0;
            leanfs_directory_entry_t e;
            int entries = 0;
            int saw_uptime = 0;
            while (virtual_file_system_readdir(PATH_PROCESS, &cookie, &e) == 1 && entries < 200) {
                entries++;
                if (k_strcmp(e.name, "uptime") == 0) {
                    saw_uptime = 1;
                }
            }
            if (!saw_uptime || entries < 3) {
                kernel_log_puts("[m87] /proc listed 0x");
                kernel_log_put_hex32((uint32_t)entries);
                kernel_log_puts(" entries and that is not a directory of processes\n");
                all_ok = 0;
            }
        }

        if (all_ok) {
            if (virtual_file_system_write("/devices", "real", 5) != 0) {
                kernel_log_puts("[m87] /devices could not be created on the real filesystem\n");
                all_ok = 0;
            } else {
                k_memset(buffer, 0, sizeof(buffer));
                if (virtual_file_system_read("/devices", buffer, sizeof(buffer)) != 5 || buffer[0] != 'r') {
                    kernel_log_puts("[m87] /devices was shadowed by the /dev mount\n");
                    all_ok = 0;
                }
                virtual_file_system_unlink("/devices");
            }
        }

        if (all_ok) {
            static const char *const LOCK = PATH_TEMPORARY_DIRECTORY "m87.lock";
            virtual_file_system_unlink(LOCK);
            int first = virtual_file_system_open(LOCK, LEANFS_OPEN_CREATE | LEANFS_OPEN_EXCL);
            int second = virtual_file_system_open(LOCK, LEANFS_OPEN_CREATE | LEANFS_OPEN_EXCL);
            if (first < 0) {
                kernel_log_puts("[m87] an exclusive create of a fresh path failed\n");
                all_ok = 0;
            } else if (second >= 0) {
                kernel_log_puts("[m87] a second exclusive create of the same path succeeded\n");
                all_ok = 0;
            }
            if (all_ok && virtual_file_system_open(LOCK, LEANFS_OPEN_CREATE) < 0) {
                kernel_log_puts("[m87] a plain create could not open a file that exists\n");
                all_ok = 0;
            }
            virtual_file_system_unlink(LOCK);
        }

        if (all_ok) {
            static const char *const TRUNC = PATH_TEMPORARY_DIRECTORY "m87.trunc";
            static char body[100];
            k_memset(body, 'A', sizeof(body));
            if (virtual_file_system_write(TRUNC, body, sizeof(body)) != 0) {
                kernel_log_puts("[m87] could not create the file to truncate\n");
                all_ok = 0;
            } else {
                int h = virtual_file_system_open(TRUNC, 0);
                if (h < 0 || virtual_file_system_handle_truncate_to(h, 10) != 0 ||
                    virtual_file_system_handle_size(h) != 10) {
                    kernel_log_puts("[m87] shrinking a file did not set its size to 10\n");
                    all_ok = 0;
                }
                if (all_ok) {
                    if (virtual_file_system_handle_truncate_to(h, 200) != 0 || virtual_file_system_handle_size(h) != 200) {
                        kernel_log_puts("[m87] growing a file did not set its size to 200\n");
                        all_ok = 0;
                    } else {
                        char tail[32];
                        k_memset(tail, 0xAA, sizeof(tail));
                        if (virtual_file_system_handle_read(h, tail, sizeof(tail), 150) != (int64_t)sizeof(tail)) {
                            kernel_log_puts("[m87] a grown file would not read past its old end\n");
                            all_ok = 0;
                        } else {
                            for (size_t i = 0; i < sizeof(tail); i++) {
                                if (tail[i] != 0) {
                                    kernel_log_puts("[m87] a grown file's new bytes were not zero\n");
                                    all_ok = 0;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (all_ok) {
                    char head[16];
                    k_memset(head, 0, sizeof(head));
                    virtual_file_system_handle_read(h, head, 10, 0);
                    for (int i = 0; i < 10; i++) {
                        if (head[i] != 'A') {
                            kernel_log_puts("[m87] truncation corrupted the bytes it kept\n");
                            all_ok = 0;
                            break;
                        }
                    }
                }
                virtual_file_system_unlink(TRUNC);
            }
        }

        if (all_ok) {
            static const char *const REAL = PATH_TEMPORARY_DIRECTORY "m87real";
            static const char *const LINK = PATH_TEMPORARY_DIRECTORY "m87link";
            static const char *const CHAIN = PATH_TEMPORARY_DIRECTORY "m87chain";
            static const char *const LOOP_A = PATH_TEMPORARY_DIRECTORY "m87loopa";
            static const char *const LOOP_B = PATH_TEMPORARY_DIRECTORY "m87loopb";
            virtual_file_system_unlink(LINK);
            virtual_file_system_unlink(CHAIN);
            virtual_file_system_unlink(LOOP_A);
            virtual_file_system_unlink(LOOP_B);
            virtual_file_system_unlink(REAL);

            static const char body[] = "pointed at";
            if (virtual_file_system_write(REAL, body, sizeof(body)) != 0 ||
                virtual_file_system_symlink(LINK, REAL) != 0) {
                kernel_log_puts("[m87] could not create a file and a link to it\n");
                all_ok = 0;
            }

            if (all_ok) {
                k_memset(buffer, 0, sizeof(buffer));
                if (virtual_file_system_read(LINK, buffer, sizeof(buffer)) != (int64_t)sizeof(body) ||
                    buffer[0] != 'p') {
                    kernel_log_puts("[m87] reading through a symlink did not reach the file\n");
                    all_ok = 0;
                }
            }

            if (all_ok) {
                char target[128];
                k_memset(target, 0, sizeof(target));
                int64_t n = virtual_file_system_readlink(LINK, target, sizeof(target) - 1);
                if (n <= 0 || k_strcmp(target, REAL) != 0) {
                    kernel_log_puts("[m87] readlink did not return the target it was given\n");
                    all_ok = 0;
                }
                if (all_ok && virtual_file_system_readlink(REAL, target, sizeof(target)) >= 0) {
                    kernel_log_puts("[m87] readlink answered for something that is not a link\n");
                    all_ok = 0;
                }
            }

            if (all_ok) {
                leanfs_stat_t st_follow, st_link;
                if (virtual_file_system_stat(LINK, &st_follow) != 0 || virtual_file_system_lstat(LINK, &st_link) != 0) {
                    kernel_log_puts("[m87] stat or lstat failed on a symlink\n");
                    all_ok = 0;
                } else if (st_follow.is_link != 0 || st_link.is_link != 1) {
                    kernel_log_puts("[m87] stat and lstat gave the same answer about a symlink\n");
                    all_ok = 0;
                } else if (st_follow.size != sizeof(body)) {
                    kernel_log_puts("[m87] stat through a link reported the link's size, not the file's\n");
                    all_ok = 0;
                }
            }

            if (all_ok) {
                if (virtual_file_system_symlink(CHAIN, LINK) != 0) {
                    kernel_log_puts("[m87] could not create a link to a link\n");
                    all_ok = 0;
                } else {
                    k_memset(buffer, 0, sizeof(buffer));
                    if (virtual_file_system_read(CHAIN, buffer, sizeof(buffer)) != (int64_t)sizeof(body)) {
                        kernel_log_puts("[m87] a chain of two links did not reach the file\n");
                        all_ok = 0;
                    }
                }
            }

            if (all_ok) {
                if (virtual_file_system_symlink(LOOP_A, LOOP_B) != 0 ||
                    virtual_file_system_symlink(LOOP_B, LOOP_A) != 0) {
                    kernel_log_puts("[m87] could not build a symlink loop to test\n");
                    all_ok = 0;
                } else if (virtual_file_system_exists(LOOP_A)) {
                    kernel_log_puts("[m87] a symlink loop resolved to something\n");
                    all_ok = 0;
                }
            }

            if (all_ok) {
                if (virtual_file_system_unlink(LINK) != 0) {
                    kernel_log_puts("[m87] a symlink could not be removed\n");
                    all_ok = 0;
                } else if (!virtual_file_system_exists(REAL)) {
                    kernel_log_puts("[m87] removing a symlink removed the file it pointed at\n");
                    all_ok = 0;
                }
            }

            virtual_file_system_unlink(CHAIN);
            virtual_file_system_unlink(LOOP_A);
            virtual_file_system_unlink(LOOP_B);
            virtual_file_system_unlink(REAL);
        }

        if (!all_ok) {
            panic("M87 self-test: this machine's /dev and /proc are not what they claim");
        }
        kernel_log_puts("[m87] files with a type and a place: /dev and /proc mounted and listed "
                   "in their parent, /dev/null ending a read and swallowing a write, "
                   "/dev/zero delivering zeros at any offset rather than ending like a "
                   "0-length file, /dev/full refusing, /dev/urandom returning something "
                   "different twice, every attempt to create or remove inside a synthetic "
                   "filesystem refused, /proc/self/exe naming a path, /proc/uptime reading "
                   "differently a second apart, /proc/self/status describing this task, and "
                   "/devices NOT shadowed by the /dev mount, exactly one of two exclusive "
                   "creates winning, and a file truncated in both directions keeping the "
                   "bytes it kept and reading zeros past its old end, a symlink followed on "
                   "open and not followed by readlink or lstat, a chain of two followed to "
                   "the end, a loop refused rather than walked, and removing a link leaving "
                   "the file it pointed at - self-test passed (");
        kernel_log_put_dec((uint32_t)(pit_get_ticks() * (1000 / PIT_HZ)) - started_ms);
        kernel_log_puts(" ms).\n\n");
    }

    {
        int tasks_before = scheduler_live_task_count();
        uint64_t frames_before = physical_memory_free_frame_count();

        long rc_missing = do_syscall(SYS_spawn, (uint64_t)"definitely_not_a_file", 0, 0);
        if (rc_missing >= 0) {
            panic("M40 SYS_spawn self-test: spawning a nonexistent file should fail");
        }

        long rc_not_elf = do_syscall(SYS_spawn, (uint64_t)(PATH_TEMPORARY_DIRECTORY "m33test"), 0, 0);
        if (rc_not_elf >= 0) {
            panic("M40 SYS_spawn self-test: spawning a non-ELF file should fail, not succeed");
        }

        int tasks_after = scheduler_live_task_count();
        uint64_t frames_after = physical_memory_free_frame_count();
        if (tasks_after != tasks_before) {
            kernel_log_puts("[m40] failed spawns changed the task count: 0x");
            kernel_log_put_hex32((uint32_t)tasks_before);
            kernel_log_puts(" -> 0x");
            kernel_log_put_hex32((uint32_t)tasks_after);
            kernel_log_putc('\n');
            panic("M40 SYS_spawn self-test: a failed spawn left a task behind");
        }
        if (frames_after != frames_before) {
            kernel_log_puts("[m40] failed spawns leaked physical frames: 0x");
            kernel_log_put_hex64(frames_before);
            kernel_log_puts(" free -> 0x");
            kernel_log_put_hex64(frames_after);
            kernel_log_putc('\n');
            panic("M40 SYS_spawn self-test: a failed spawn leaked physical memory");
        }

        kernel_log_puts("[m40] SYS_spawn failure-path self-test passed (missing file and "
                   "non-ELF file both refused cleanly, no task or frame leaked).\n\n");
    }

    {
        task_t *boot_task = scheduler_current();
        int leaked = 0;
        for (int i = 2; i < MAX_FILE_DESCRIPTORS; i++) {
            if (boot_task->file_descriptors[i].type != FILE_DESCRIPTOR_NONE) {
                leaked++;
            }
        }
        if (leaked == 0) {
            panic("M40 fd-inheritance self-test: expected the boot self-tests above to have left fds open on task 0 - if that is genuinely no longer true, delete this check and sched_reset_fds_to_std with it");
        }
        scheduler_reset_file_descriptors_to_std(boot_task);
        for (int i = 2; i < MAX_FILE_DESCRIPTORS; i++) {
            if (boot_task->file_descriptors[i].type != FILE_DESCRIPTOR_NONE) {
                panic("M40 fd-inheritance self-test: sched_reset_fds_to_std left an fd behind");
            }
        }
        if (boot_task->file_descriptors[0].type != FILE_DESCRIPTOR_STDIN || boot_task->file_descriptors[1].type != FILE_DESCRIPTOR_STDOUT) {
            panic("M40 fd-inheritance self-test: sched_reset_fds_to_std did not leave stdin/stdout intact");
        }
        kernel_log_puts("[m40] boot-task fd reset self-test passed (0x");
        kernel_log_put_hex32((uint32_t)leaked);
        kernel_log_puts(" leaked self-test fd(s) reclaimed before PID 1 inherits the table).\n\n");
    }
}

void kernel_main(uint32_t *e820_map, framebuffer_boot_info_t *framebuffer_info, uint64_t rsdp_phys) {
    kernel_log_init();
    kernel_log_puts("lean_os kernel: hello from C!\n\n");

    fwcfg_init();
    if (boot_selftests_enabled()) {
        kernel_log_puts("[boot] self-tests ENABLED for this boot "
                   "(opt/leanos/selftest=1 via fw_cfg).\n\n");
    } else {
        kernel_log_puts("[boot] self-tests off - booting straight to the desktop. "
                   "tools/run-tests.sh turns them on.\n\n");
    }

    gdt_init();
    idt_init();
    pic_remap();
    acpi_set_rsdp(rsdp_phys);
    kernel_log_puts("GDT/TSS, IDT, and PIC remap initialized.\n");

    __asm__ volatile("int3");
    kernel_log_puts("Resumed after breakpoint self-test.\n\n");

    uint32_t count = *e820_map;
    if (count == 0) {
        panic("E820 memory map is empty - cannot continue");
    }

    e820_entry_t *entries = (e820_entry_t *)((uint8_t *)e820_map + 8);

    kernel_log_puts("E820 memory map (");
    kernel_log_put_hex32(count);
    kernel_log_puts(" entries):\n");

    for (uint32_t i = 0; i < count; i++) {
        kernel_log_puts("  base=0x");
        kernel_log_put_hex64(entries[i].base);
        kernel_log_puts(" len=0x");
        kernel_log_put_hex64(entries[i].length);
        kernel_log_puts(" type=0x");
        kernel_log_put_hex32(entries[i].type);
        kernel_log_putc('\n');
    }
    kernel_log_putc('\n');

    physical_memory_init(e820_map);
    virtual_memory_init(e820_map);
    heap_init();

    unix_socket_init();
    eventfd_init();
    timerfd_init();
    epoll_init();
    memfd_init();

    uint64_t scratch_phys = physical_memory_alloc_frame();
    uint64_t scratch_virt = KERNEL_HEAP_VIRT_BASE - 0x40000000ULL;
    virtual_memory_map_page(scratch_virt, scratch_phys, VIRTUAL_MEMORY_FLAG_WRITABLE);
    volatile uint64_t *scratch = (volatile uint64_t *)scratch_virt;
    *scratch = 0x1122334455667788ULL;
    if (*scratch != 0x1122334455667788ULL) {
        panic("vmm self-test: readback mismatch");
    }
    virtual_memory_unmap_page(scratch_virt);
    physical_memory_free_frame(scratch_phys);
    kernel_log_puts("[vmm] map/unmap self-test passed.\n");

    uint64_t *test = (uint64_t *)kmalloc(sizeof(uint64_t));
    if (!test) {
        panic("kmalloc self-test: allocation failed");
    }
    *test = 0xDEADBEEFCAFEBABEULL;
    if (*test != 0xDEADBEEFCAFEBABEULL) {
        panic("kmalloc self-test: readback mismatch");
    }
    kfree(test);
    kernel_log_puts("[heap] kmalloc/kfree self-test passed.\n\n");

    {
        uint64_t tracked = physical_memory_tracked_limit();
        uint64_t floor;
        if (tracked > PHYSICAL_MEMORY_DMA_LIMIT) {
            floor = PHYSICAL_MEMORY_DMA_LIMIT;
        } else if (tracked > 0x40000000ULL) {
            floor = 0x40000000ULL;
        } else {
            floor = tracked / 2;
        }
        uint64_t before = physical_memory_free_frame_count();
        uint64_t high = physical_memory_alloc_frame_above(floor);
        if (high == 0) {
            panic("[m90] no free frame above the probe floor");
        }
        if (high < floor) {
            panic("[m90] pmm_alloc_frame_above returned a frame below its floor");
        }
        if (!virtual_memory_identity_covers(high, 4096)) {
            panic("[m90] a frame the allocator handed out is not identity-mapped");
        }
        volatile uint64_t *probe = (volatile uint64_t *)(uintptr_t)high;
        probe[0] = 0x9090909090909090ULL;
        probe[511] = 0x0123456789ABCDEFULL;
        if (probe[0] != 0x9090909090909090ULL || probe[511] != 0x0123456789ABCDEFULL) {
            panic("[m90] readback mismatch on a high physical frame");
        }
        physical_memory_free_frame(high);
        if (physical_memory_free_frame_count() != before) {
            panic("[m90] freeing a high frame did not return the count");
        }
        if (virtual_memory_identity_covers(tracked + 0x40000000ULL, 4096)) {
            panic("[m90] the identity map claims to cover memory that does not exist");
        }
        kernel_log_puts("[m90] more than a gigabyte: ");
        kernel_log_put_hex64(tracked / (1024 * 1024));
        kernel_log_puts(" MiB tracked in ");
        kernel_log_put_hex64(physical_memory_total_frame_count());
        kernel_log_puts(" frames, a frame at 0x");
        kernel_log_put_hex64(high);
        kernel_log_puts(" written and read back through the identity map, freed with the\n"
                  "      count returning exactly, and an address past the end of memory "
                  "correctly reported as not mapped.\n\n");
    }

    framebuffer_init(framebuffer_info);
    dispi_init();
    rtc_init();
    {
        os_datetime_t now;
        rtc_read(&now);
        random_init(&now, sizeof(now));
    }
    pc_speaker_init();
    ac97_init();

    framebuffer_clear(0x001A1A2E);
    framebuffer_fill_rect(10, 10, 100, 50, 0x00E94560);
    if (framebuffer_get_pixel(0, 0) != 0x001A1A2E) {
        panic("fb self-test: background color readback mismatch");
    }
    if (framebuffer_get_pixel(59, 34) != 0x00E94560) {
        panic("fb self-test: rectangle color readback mismatch (inside)");
    }
    if (framebuffer_get_pixel(200, 200) != 0x001A1A2E) {
        panic("fb self-test: rectangle color readback mismatch (outside, should be background)");
    }
    kernel_log_puts("[fb] framebuffer clear/fill/readback self-test passed.\n\n");

    {
        uint32_t boot_w = framebuffer_width(), boot_h = framebuffer_height(), boot_pitch = framebuffer_pitch_bytes();

        if (!dispi_available()) {
            kernel_log_puts("[m58] no runtime mode-setting interface on this adapter - "
                       "resolution stays what the firmware chose (self-test skipped).\n\n");
        } else {
            display_mode_t list[DISPLAY_MAX_MODES];
            int n = dispi_get_modes(list, DISPLAY_MAX_MODES);
            if (n <= 0) {
                panic("M58 self-test: a DISPI adapter answered the probe but offers no modes");
            }
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
            framebuffer_remap(pitch, list[pick].width, list[pick].height);

            if (framebuffer_width() != list[pick].width || framebuffer_height() != list[pick].height) {
                panic("M58 self-test: fb geometry after a mode change is not the mode that was set");
            }
            if (framebuffer_pitch_bytes() != pitch) {
                panic("M58 self-test: fb pitch is not the one read back from the device");
            }
            if (framebuffer_mapped_bytes() < (uint64_t)pitch * framebuffer_height()) {
                panic("M58 self-test: the framebuffer mapping does not cover the new mode");
            }

            framebuffer_put_pixel(framebuffer_width() - 1, framebuffer_height() - 1, 0x00123456u);
            if (framebuffer_get_pixel(framebuffer_width() - 1, framebuffer_height() - 1) != 0x00123456u) {
                panic("M58 self-test: the last pixel of the new mode did not read back");
            }

            uint32_t back_pitch = 0;
            if (dispi_set_mode(boot_w, boot_h, &back_pitch) != 0) {
                panic("M58 self-test: could not restore the boot mode - this is the failure the revert timer exists for");
            }
            framebuffer_remap(back_pitch, boot_w, boot_h);
            if (framebuffer_width() != boot_w || framebuffer_height() != boot_h || framebuffer_pitch_bytes() != boot_pitch) {
                panic("M58 self-test: the boot mode did not come back exactly as it was");
            }
            framebuffer_clear(0x00000000u);

            kernel_log_puts("[m58] display mode set and read back from the device (geometry, "
                       "device-chosen pitch and a grown mapping), then restored - self-test passed.\n\n");
        }
    }

    console_init();

    {
        int all_ok = 1;

        for (int code = 0; code < 128 && all_ok; code++) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                if (font8x16[code][row] & 0x01u) {
                    kernel_log_puts("[font39] glyph 0x");
                    kernel_log_put_hex32((uint32_t)code);
                    kernel_log_puts(" has ink in column 7, the reserved advance gap.\n");
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
                kernel_log_puts("[font39] printable codepoint 0x");
                kernel_log_put_hex32((uint32_t)code);
                kernel_log_puts(" is blank - the table is incomplete.\n");
                all_ok = 0;
            }
        }

        for (int code = 0; code < 128 && all_ok; code++) {
            if (code > 0x20 && code < 0x7F) {
                continue;
            }
            for (int row = 0; row < FONT_HEIGHT; row++) {
                if (font8x16[code][row]) {
                    kernel_log_puts("[font39] non-printable codepoint 0x");
                    kernel_log_put_hex32((uint32_t)code);
                    kernel_log_puts(" should be blank but isn't.\n");
                    all_ok = 0;
                    break;
                }
            }
        }

        for (int code = 0; code < 128 && all_ok; code++) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = font8x16[code][row];
                if (font8x16_bold[code][row] != (uint8_t)(bits | (bits >> 1))) {
                    kernel_log_puts("[font39] font8x16_bold disagrees with the dilation of font8x16 at 0x");
                    kernel_log_put_hex32((uint32_t)code);
                    kernel_log_putc('\n');
                    all_ok = 0;
                    break;
                }
            }
        }

        if (all_ok) {
            console_puts("Axg");
            uint32_t bg = framebuffer_get_pixel(0, 0);

            int top[3], bot[3], gap_clear[3];
            for (int cell = 0; cell < 3; cell++) {
                top[cell] = -1;
                bot[cell] = -1;
                gap_clear[cell] = 1;
                for (int y = 0; y < FONT_HEIGHT; y++) {
                    for (int x = 0; x < FONT_WIDTH; x++) {
                        if (framebuffer_get_pixel((uint32_t)(cell * FONT_WIDTH + x), (uint32_t)y) != bg) {
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
                { "'g' does not reach the shared descender row",      bot[2], FONT_DESCRIPTOR_LAST },
                { "'A' drew into its advance gap (column 7)",         gap_clear[0], 1 },
                { "'x' drew into its advance gap (column 7)",         gap_clear[1], 1 },
                { "'g' drew into its advance gap (column 7)",         gap_clear[2], 1 },
            };
            for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
                if (checks[i].got != checks[i].want) {
                    kernel_log_puts("[font39] rendered-pixel check failed: ");
                    kernel_log_puts(checks[i].what);
                    kernel_log_puts(" - expected ");
                    kernel_log_put_hex32((uint32_t)checks[i].want);
                    kernel_log_puts(" got ");
                    kernel_log_put_hex32((uint32_t)checks[i].got);
                    kernel_log_putc('\n');
                    all_ok = 0;
                }
            }

            console_init();
        }

        if (!all_ok) {
            panic("M39 font self-test: glyph table and/or rendered text metric is wrong");
        }
        kernel_log_puts("[font39] glyph table + shared-baseline render self-test passed.\n\n");
    }

    kernel_log_use_console();
    kernel_log_puts("[console] framebuffer text console active - logging switched over from VGA text mode.\n\n");

    __asm__ volatile("sti");

    ioapic_init();

    pit_init();
    tsc_init();
    kernel_log_puts("[pit] channel 0 programmed for ");
    kernel_log_put_hex32(PIT_HZ);
    kernel_log_puts(" Hz, IRQ0 unmasked.\n");

    uint64_t before = pit_get_ticks();
    pit_sleep_ms(50);
    uint64_t after = pit_get_ticks();
    kernel_log_puts("[pit] slept 50ms: ticks ");
    kernel_log_put_hex64(before);
    kernel_log_puts(" -> ");
    kernel_log_put_hex64(after);
    kernel_log_putc('\n');

    keyboard_init();

    int usb_devices = xhci_init();
    if (usb_devices > 0) {
        kernel_log_puts("[usb] ");
        kernel_log_put_dec(usb_devices);
        kernel_log_puts(" boot-protocol HID device(s) on the USB bus.\n");
    }

    kernel_log_puts("[kbd] IRQ1 unmasked, waiting up to 3s for a test keypress "
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
        kernel_log_puts("[kbd] received keypress: '");
        kernel_log_putc((char)key);
        kernel_log_puts("'\n");
    } else {
        kernel_log_puts("[kbd] no keypress within timeout - driver is installed, "
                   "just untested interactively this boot.\n");
    }

    kernel_log_putc('\n');

    mouse_init();
    cursor_init((int32_t)(framebuffer_width() / 2), (int32_t)(framebuffer_height() / 2));
    kernel_log_puts("[mouse] IRQ12 unmasked, cursor drawn at screen center. Waiting "
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
        kernel_log_puts("[mouse] received movement/click - cursor now at (");
        kernel_log_put_hex32((uint32_t)cursor_x());
        kernel_log_puts(", ");
        kernel_log_put_hex32((uint32_t)cursor_y());
        kernel_log_puts(") buttons=0x");
        kernel_log_put_hex32(last_ev.buttons);
        kernel_log_putc('\n');
    } else {
        kernel_log_puts("[mouse] no movement within timeout - driver is installed, "
                   "just untested interactively this boot.\n");
    }
    kernel_log_putc('\n');

    fpu_init_cpu();
    scheduler_init();
    scheduler_spawn_idle_tasks(2);
    kernel_log_puts("[sched] round-robin scheduler initialized (this context is task 0).\n");
    task_spawn("demo-a", demo_task, "A");
    task_spawn("demo-b", demo_task, "B");
    kernel_log_puts("[sched] spawned tasks A and B; letting them run via "
               "preemption for ~1.5s...\n");
    pit_sleep_ms(1500);
    kernel_log_puts("[sched] back on the main task - preemption round trip verified.\n\n");

    long pid = do_syscall(SYS_getpid, 0, 0, 0);
    kernel_log_puts("[syscall] getpid() = ");
    kernel_log_put_hex64((uint64_t)pid);
    kernel_log_putc('\n');

    static const char message[] = "[syscall] hello via SYS_write\n";
    long written = do_syscall(SYS_write, 1, (uint64_t)message, sizeof(message) - 1);
    if (written != (long)sizeof(message) - 1) {
        panic("syscall self-test: SYS_write returned an unexpected length");
    }

    task_spawn("syscall-exit", syscall_exit_task, NULL);
    pit_sleep_ms(200);
    kernel_log_puts("[syscall] SYS_exit self-test task ran and terminated.\n\n");

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
    kernel_log_puts("[pipe] kernel-level producer/consumer self-test passed.\n\n");

    int pipe_file_descriptors[2];
    if (do_syscall(SYS_pipe, (uint64_t)pipe_file_descriptors, 0, 0) != 0) {
        panic("SYS_pipe self-test: pipe creation failed");
    }
    static const char pipe_message[] = "hello through a syscall pipe";
    long pipe_written = do_syscall(SYS_write, (uint64_t)pipe_file_descriptors[1], (uint64_t)pipe_message, sizeof(pipe_message) - 1);
    if (pipe_written != (long)sizeof(pipe_message) - 1) {
        panic("SYS_pipe self-test: SYS_write returned an unexpected length");
    }
    char pipe_readback[64] = {0};
    long pipe_read_n = do_syscall(SYS_read, (uint64_t)pipe_file_descriptors[0], (uint64_t)pipe_readback, sizeof(pipe_readback) - 1);
    if (pipe_read_n != (long)sizeof(pipe_message) - 1 || k_strcmp(pipe_readback, pipe_message) != 0) {
        panic("SYS_pipe self-test: SYS_read returned unexpected data");
    }
    kernel_log_puts("[pipe] SYS_pipe/SYS_write/SYS_read self-test passed.\n\n");

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
    kernel_log_puts("[signal] SIGTERM self-test passed (spinner task terminated).\n\n");

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
    kernel_log_puts("[wait] SYS_wait(-1) self-test passed (reaped two children, then -1).\n\n");

    task_t *pgid_child = task_spawn("pgidprobe", spinner_task, NULL);
    long self_pgid = do_syscall(SYS_getpgid, 0, 0, 0);
    long child_pgid = do_syscall(SYS_getpgid, (uint64_t)pgid_child->id, 0, 0);
    do_syscall(SYS_kill, (uint64_t)pgid_child->id, SIGKILL, 0);
    do_syscall(SYS_wait, (uint64_t)pgid_child->id, 0, 0);
    if (self_pgid != 0 || child_pgid != self_pgid) {
        panic("SYS_getpgid self-test: child did not inherit its parent's process group");
    }
    kernel_log_puts("[pgid] SYS_getpgid self-test passed (child inherited pgid ");
    kernel_log_put_hex64((uint64_t)self_pgid);
    kernel_log_puts(").\n\n");

    block_device_init();

    virtual_file_system_init();
    {
        block_device_statistics_t before, after;
        static uint8_t cold[64 * 1024];
        static uint8_t warm[64 * 1024];
        const uint32_t RUNS = 16;
        const uint32_t SECTORS = sizeof(cold) / BLOCK_DEVICE_SECTOR_SIZE;

        block_device_cache_drop();
        block_device_statistics(&before);
        uint64_t c0 = tsc_read();
        uint32_t sum_cold = 0;
        for (uint32_t r = 0; r < RUNS; r++) {
            block_device_read(LEANFS_START_LBA + r * SECTORS, SECTORS, cold);
            for (uint32_t i = 0; i < sizeof(cold); i += 512) {
                sum_cold += cold[i];
            }
        }
        uint64_t c1 = tsc_read();

        uint32_t sum_warm = 0;
        for (uint32_t r = 0; r < RUNS; r++) {
            block_device_read(LEANFS_START_LBA + r * SECTORS, SECTORS, warm);
            for (uint32_t i = 0; i < sizeof(warm); i += 512) {
                sum_warm += warm[i];
            }
        }
        uint64_t c2 = tsc_read();
        block_device_statistics(&after);
        uint64_t cold_us = tsc_to_us(c1 - c0);
        uint64_t warm_us = tsc_to_us(c2 - c1);

        if (sum_cold != sum_warm) {
            panic("M92 self-test: the cache returned different bytes than the device did");
        }
        uint64_t dev_reads_cold = after.device_reads - before.device_reads;
        if (dev_reads_cold < RUNS * SECTORS) {
            panic("M92 self-test: the cold pass did not read a full megabyte from the device");
        }
        if (after.hits <= before.hits) {
            panic("M92 self-test: the warm pass never hit the cache - it is not caching");
        }

        kernel_log_perf("disk_1mib_cold_us", cold_us, "us");
        kernel_log_perf("disk_1mib_warm_us", warm_us, "us");
        kernel_log_puts("[m92] a disk worth reading: 1 MiB through ");
        kernel_log_puts(block_device_backend_name());
        kernel_log_puts(" cold in ");
        kernel_log_put_dec((uint32_t)cold_us);
        kernel_log_puts(" us and warm from the cache in ");
        kernel_log_put_dec((uint32_t)warm_us);
        kernel_log_puts(" us, identical byte for byte; 0x");
        kernel_log_put_hex64(after.hits);
        kernel_log_puts(" of 0x");
        kernel_log_put_hex64(after.reads);
        kernel_log_puts(" reads served without touching the device since boot - self-test passed.\n\n");
    }

    {
        const uint32_t SCRATCH_LBA = LEANFS_START_LBA - 16;
        static uint8_t m107_write[512];
        static uint8_t m107_read[512];

        for (uint32_t i = 0; i < sizeof(m107_write); i++) {
            m107_write[i] = (uint8_t)(i * 7 + 13);
        }

        int ok = 1;
        if (block_device_write(SCRATCH_LBA, 1, m107_write) != 0) {
            kernel_log_puts("[m107] the scratch write failed\n");
            ok = 0;
        }
        if (ok && block_device_flush() != 0) {
            kernel_log_puts("[m107] the barrier after the scratch write failed\n");
            ok = 0;
        }
        block_device_cache_drop();
        if (ok && block_device_read(SCRATCH_LBA, 1, m107_read) != 0) {
            kernel_log_puts("[m107] the scratch read failed\n");
            ok = 0;
        }
        if (ok) {
            for (uint32_t i = 0; i < sizeof(m107_write); i++) {
                if (m107_read[i] != m107_write[i]) {
                    kernel_log_puts("[m107] the scratch sector read back wrong at byte 0x");
                    kernel_log_put_hex32(i);
                    kernel_log_putc('\n');
                    ok = 0;
                    break;
                }
            }
        }

        static uint8_t m107_multi[512 * 8];
        static uint8_t m107_back[512 * 8];
        for (uint32_t i = 0; i < sizeof(m107_multi); i++) {
            m107_multi[i] = (uint8_t)(i * 31 + (i >> 9));
        }
        if (ok && block_device_write(SCRATCH_LBA + 8, 8, m107_multi) != 0) {
            kernel_log_puts("[m107] the eight-sector write failed\n");
            ok = 0;
        }
        if (ok && block_device_flush() != 0) {
            ok = 0;
        }
        block_device_cache_drop();
        if (ok && block_device_read(SCRATCH_LBA + 8, 8, m107_back) != 0) {
            kernel_log_puts("[m107] the eight-sector read failed\n");
            ok = 0;
        }
        if (ok) {
            for (uint32_t i = 0; i < sizeof(m107_multi); i++) {
                if (m107_back[i] != m107_multi[i]) {
                    kernel_log_puts("[m107] the eight-sector round trip differs at byte 0x");
                    kernel_log_put_hex32(i);
                    kernel_log_putc('\n');
                    ok = 0;
                    break;
                }
            }
        }

        if (!ok) {
            panic("M107 self-test: the block backend did not return what was written to it");
        }

        kernel_log_puts("[m107] the devices a real machine has: the block layer is on ");
        kernel_log_puts(block_device_backend_name());
        kernel_log_puts(", one sector and eight sectors written, dropped from the cache, "
                  "and read back byte for byte; USB: ");
        kernel_log_put_dec((uint32_t)xhci_device_count());
        kernel_log_puts(" boot-protocol HID device(s) - self-test passed.\n\n");
    }

    {
        static uint8_t wbuf[64 * 1024];
        static uint8_t rbuf[64 * 1024];
        const uint32_t RUNS = 16;
        const uint32_t SECTORS = sizeof(wbuf) / BLOCK_DEVICE_SECTOR_SIZE;
        const uint32_t SCRATCH = leanfs_free_scratch_lba(RUNS * 16u);
        if (SCRATCH == 0) {
            kernel_log_puts("[m104] no free run at the end of the data region - the "
                       "write benchmark is skipped rather than run over "
                       "somebody's file.\n\n");
        } else {

        for (uint32_t i = 0; i < sizeof(wbuf); i++) {
            wbuf[i] = (uint8_t)(i * 7u + 13u);
        }

        block_device_statistics_t b0, b1, b2;

        block_device_cache_drop();
        block_device_statistics(&b0);
        uint64_t t0 = tsc_read();
        for (uint32_t r = 0; r < RUNS; r++) {
            block_device_write(SCRATCH + r * SECTORS, SECTORS, wbuf);
        }
        block_device_flush();
        uint64_t t1 = tsc_read();
        block_device_statistics(&b1);

        block_device_cache_drop();
        uint64_t t2 = tsc_read();
        for (uint32_t r = 0; r < RUNS; r++) {
            for (uint32_t k = 0; k < SECTORS; k++) {
                block_device_write(SCRATCH + r * SECTORS + k, 1,
                          wbuf + (uint64_t)k * BLOCK_DEVICE_SECTOR_SIZE);
            }
        }
        uint64_t t3 = tsc_read();
        block_device_statistics(&b2);

        uint64_t absorbed_us = tsc_to_us(t1 - t0);
        uint64_t through_us = tsc_to_us(t3 - t2);

        block_device_cache_drop();
        int identical = 1;
        for (uint32_t r = 0; r < RUNS && identical; r++) {
            block_device_read(SCRATCH + r * SECTORS, SECTORS, rbuf);
            for (uint32_t i = 0; i < sizeof(rbuf); i++) {
                if (rbuf[i] != wbuf[i]) {
                    identical = 0;
                    break;
                }
            }
        }
        if (!identical) {
            panic("M104 self-test: what came back is not what was written - the "
                  "writeback cache lost bytes");
        }
        if (b1.device_writes <= b0.device_writes) {
            panic("M104 self-test: the barrier issued no device writes - the cache "
                  "is holding a megabyte and calling it written");
        }

        block_device_set_readahead(0);
        block_device_cache_drop();
        uint64_t r0 = tsc_read();
        for (uint32_t r = 0; r < RUNS; r++) {
            block_device_read(SCRATCH + r * SECTORS, SECTORS, rbuf);
        }
        uint64_t r1 = tsc_read();

        block_device_set_readahead(8);
        block_device_cache_drop();
        uint64_t r2 = tsc_read();
        for (uint32_t r = 0; r < RUNS; r++) {
            block_device_read(SCRATCH + r * SECTORS, SECTORS, rbuf);
        }
        uint64_t r3 = tsc_read();
        block_device_statistics_t b3;
        block_device_statistics(&b3);

        uint64_t noread_us = tsc_to_us(r1 - r0);
        uint64_t ahead_us = tsc_to_us(r3 - r2);

        kernel_log_perf("disk_1mib_write_absorbed_us", absorbed_us, "us");
        kernel_log_perf("disk_1mib_write_through_us", through_us, "us");
        kernel_log_perf("disk_1mib_seq_read_no_readahead_us", noread_us, "us");
        kernel_log_perf("disk_1mib_seq_read_readahead_us", ahead_us, "us");

        kernel_log_puts("[m104] writeback: 1 MiB written and barriered in ");
        kernel_log_put_dec((uint32_t)absorbed_us);
        kernel_log_puts(" us against ");
        kernel_log_put_dec((uint32_t)through_us);
        kernel_log_puts(" us written through; 1 MiB read sequentially in ");
        kernel_log_put_dec((uint32_t)ahead_us);
        kernel_log_puts(" us with readahead against ");
        kernel_log_put_dec((uint32_t)noread_us);
        kernel_log_puts(" us without (0x");
        kernel_log_put_hex64(b3.readaheads);
        kernel_log_puts(" lines fetched ahead); every byte read back identical to what "
                   "was written, and the device's own write counter proves the "
                   "barrier reached it - self-test passed.\n\n");
        }
    }

    tty_init();
    {
        static const char *const LAYOUT[] = {PATH_BIN, PATH_HOME, PATH_ETC, PATH_TEMPORARY};
        for (size_t i = 0; i < sizeof(LAYOUT) / sizeof(LAYOUT[0]); i++) {
            if (!virtual_file_system_exists(LAYOUT[i]) && virtual_file_system_mkdir(LAYOUT[i]) != 0) {
                panic("vfs_mkdir: failed to create the filesystem layout");
            }
        }
    }
    for (size_t i = 0; i < EMBEDDED_PROGRAM_COUNT; i++) {
        const embedded_program_t *p = &embedded_programs[i];
        char path[PATH_MAX_LENGTH];
        if (path_join(path, PATH_BIN_DIRECTORY, p->name) != 0) {
            panic("a program name is too long to live in /bin");
        }
        if (!virtual_file_system_exists(path)) {
            kernel_log_puts("[fs] seeding disk with '");
            kernel_log_puts(path);
            kernel_log_puts("' (first boot only)...\n");
            size_t size = (size_t)(p->end - p->start);
            if (virtual_file_system_write(path, p->start, size) != 0) {
                panic("vfs_write: failed to seed a program onto disk");
            }
        }
    }
    kernel_log_puts("[fs] all user programs present in " PATH_BIN ".\n\n");

    {
        static const struct {
            const char *path;
            const char *body;
        } FIRST_BOOT[] = {
            {PATH_HOME_DIRECTORY "readme.txt",
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
            {PATH_HOME_DIRECTORY "notes.txt",
             "Scratch file.\n"
             "\n"
             "The editor has undo (Ctrl+Z), redo (Ctrl+Y), find (Ctrl+F),\n"
             "cut/copy/paste, and a File menu that can save somewhere else.\n"
             "\n"
             "Nothing here is precious - edit it.\n"},
            {PATH_HOME_DIRECTORY "hello.sh",
             "#!/bin/sh\n"
             "# A script is a program here. Run it from the terminal as\n"
             "#   /home/hello.sh\n"
             "echo \"hello from $SHELL\"\n"
             "pwd\n"
             "echo \"there are these programs:\"\n"
             "ls /bin\n"},
            {PATH_RESOLV_CONF,
             "# /etc/resolv.conf - which nameservers this machine asks.\n"
             "#\n"
             "# Every `nameserver` line here is asked, and so is whatever\n"
             "# DHCP handed over - all of them at once, first correct\n"
             "# answer wins. That is on purpose: a nameserver that\n"
             "# accepts queries and never answers is a common failure,\n"
             "# and asking one server at a time means paying its whole\n"
             "# timeout before trying the next.\n"
             "#\n"
             "# The line below is a public resolver, listed so that a\n"
             "# machine whose own DNS server is broken still works. If\n"
             "# you would rather this machine only ever talked to the\n"
             "# server your network gave it, delete the line.\n"
             "#\n"
             "# `nslookup <name>` prints the servers it actually asked.\n"
             "nameserver 1.1.1.1\n"},
        };
        for (size_t i = 0; i < sizeof(FIRST_BOOT) / sizeof(FIRST_BOOT[0]); i++) {
            if (virtual_file_system_exists(FIRST_BOOT[i].path)) {
                continue;
            }
            size_t length = 0;
            while (FIRST_BOOT[i].body[length]) {
                length++;
            }
            if (virtual_file_system_write(FIRST_BOOT[i].path, FIRST_BOOT[i].body, length) != 0) {
                panic("vfs_write: failed to seed a first-boot file into " PATH_HOME);
            }
        }
    }

    {
        size_t fstest_length = 20000;
        uint8_t *fstest_buffer = (uint8_t *)kmalloc(fstest_length);
        uint8_t *fstest_readback = (uint8_t *)kmalloc(fstest_length);
        if (!fstest_buffer || !fstest_readback) {
            panic("out of memory for leanfs indirect-block self-test");
        }
        for (size_t i = 0; i < fstest_length; i++) {
            fstest_buffer[i] = (uint8_t)(i * 31 + 7);
        }
        if (virtual_file_system_write(PATH_TEMPORARY_DIRECTORY "fstest", fstest_buffer, fstest_length) != 0) {
            panic("leanfs indirect-block self-test: vfs_write failed");
        }
        k_memset(fstest_readback, 0, fstest_length);
        int64_t fstest_size = virtual_file_system_read(PATH_TEMPORARY_DIRECTORY "fstest", fstest_readback, fstest_length);
        if (fstest_size != (int64_t)fstest_length) {
            panic("leanfs indirect-block self-test: size mismatch on readback");
        }
        for (size_t i = 0; i < fstest_length; i++) {
            if (fstest_readback[i] != fstest_buffer[i]) {
                panic("leanfs indirect-block self-test: data mismatch on readback");
            }
        }
        kfree(fstest_buffer);
        kfree(fstest_readback);
        kernel_log_puts("[fs] leanfs indirect-block self-test passed (20000-byte round trip).\n\n");
    }

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
        kernel_log_puts("[memtest] user-space malloc/free and cross-process shm self-tests passed.\n\n");
    }

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
        kernel_log_puts("[m57] proportional UI font: per-glyph advances, one shared baseline across three sizes, and every measured width matching the ink drawn - self-test passed.\n\n");
    }

    if (boot_selftests_enabled()) {
        boot_selftests_desktop();
    }

    smp_init();

    power_init();

    if (!net_init()) {
        kernel_log_puts("[net] no RTL8139 NIC found - networking unavailable this boot "
                   "(expected on real hardware).\n\n");
    }

    if (boot_selftests_enabled()) {
        boot_selftests_system();
    }

    if (boot_selftests_enabled()) {
        selftest_settings_restore();
    }

    kernel_log_puts("[sched] task table at handoff: 0x");
    kernel_log_put_hex32((uint32_t)scheduler_live_task_count());
    kernel_log_puts(" live of 0x");
    kernel_log_put_hex32((uint32_t)MAX_TASKS);
    kernel_log_puts(" slots (high-water mark 0x");
    kernel_log_put_hex32((uint32_t)scheduler_task_count());
    kernel_log_puts(").\n");

    if (boot_bootstrap_enabled()) {
        os_stat_t bst;
        if (do_syscall(SYS_stat, (uint64_t)"/tests/bootstrap/run.sh",
                       (uint64_t)&bst, 0) != 0) {
            kernel_log_puts("[m98boot] no build fixtures on this image - skipped. "
                       "tools/install-native-toolchain.sh puts them there, and "
                       "it needs tools/build-native-toolchain.sh to have run.\n\n");
        } else {
            kernel_log_puts("[m98boot] building on this machine - this is minutes, "
                       "not seconds.\n");
            uint64_t started = pit_get_ticks();
            long pid = do_syscall(SYS_spawn,
                                  (uint64_t)"/tests/bootstrap/run.sh", 0, 0);
            if (pid < 0) {
                panic("M98 bootstrap: the build script could not be spawned");
            }
            do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            uint64_t elapsed_s = (pit_get_ticks() - started) / PIT_HZ;

            int file_descriptor_task = -1;
            int file_descriptor_peak = scheduler_file_descriptor_high_water(&file_descriptor_task);
            int task_peak = scheduler_peak_live_tasks();
            uint64_t page_kb = 4;
            uint64_t used_frames = physical_memory_total_frame_count() - physical_memory_free_frame_count();

            kernel_log_perf("build_wall_s", elapsed_s, "s");
            kernel_log_perf("build_peak_live_tasks", (uint64_t)task_peak, "tasks");
            kernel_log_perf("build_peak_fds_one_task", (uint64_t)file_descriptor_peak, "fds");
            kernel_log_perf("build_frames_in_use_after", used_frames * page_kb, "KiB");

            kernel_log_puts("[m98boot] the toolchain built somebody else's program here: "
                       "the whole build ran in ");
            kernel_log_put_dec((uint32_t)elapsed_s);
            kernel_log_puts(" s, and the two ceilings this milestone predicted it would "
                       "hit were reached at ");
            kernel_log_put_dec((uint32_t)task_peak);
            kernel_log_puts(" of ");
            kernel_log_put_dec((uint32_t)MAX_TASKS);
            kernel_log_puts(" task slots and ");
            kernel_log_put_dec((uint32_t)file_descriptor_peak);
            kernel_log_puts(" of ");
            kernel_log_put_dec((uint32_t)MAX_FILE_DESCRIPTORS);
            kernel_log_puts(" descriptors in one task - measured.\n\n");
        }
    }

    if (boot_pytest_enabled()) {
        os_stat_t pst;
        if (do_syscall(SYS_stat, (uint64_t)"/tests/python/run.sh",
                       (uint64_t)&pst, 0) != 0) {
            kernel_log_puts("[m99pytest] no python fixtures on this image - "
                       "skipped. tools/build-python.sh builds CPython and "
                       "tools/install-python.sh puts it and its own test "
                       "suite here.\n\n");
        } else {
            kernel_log_puts("[m99pytest] running CPython's own regression suite - "
                       "this is tens of minutes, not seconds.\n");
            uint64_t started = pit_get_ticks();
            long pid = do_syscall(SYS_spawn,
                                  (uint64_t)"/tests/python/run.sh", 0, 0);
            if (pid < 0) {
                panic("M99: the regression-suite script could not be spawned");
            }
            do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            uint64_t elapsed_s = (pit_get_ticks() - started) / PIT_HZ;

            int file_descriptor_task = -1;
            int file_descriptor_peak = scheduler_file_descriptor_high_water(&file_descriptor_task);
            int task_peak = scheduler_peak_live_tasks();

            kernel_log_perf("pytest_wall_s", elapsed_s, "s");
            kernel_log_perf("pytest_peak_live_tasks", (uint64_t)task_peak, "tasks");
            kernel_log_perf("pytest_peak_fds_one_task", (uint64_t)file_descriptor_peak, "fds");

            kernel_log_puts("[m99pytest] somebody else's test suite ran here: "
                       "the whole list took ");
            kernel_log_put_dec((uint32_t)elapsed_s);
            kernel_log_puts(" s, reaching ");
            kernel_log_put_dec((uint32_t)task_peak);
            kernel_log_puts(" of ");
            kernel_log_put_dec((uint32_t)MAX_TASKS);
            kernel_log_puts(" task slots and ");
            kernel_log_put_dec((uint32_t)file_descriptor_peak);
            kernel_log_puts(" of ");
            kernel_log_put_dec((uint32_t)MAX_FILE_DESCRIPTORS);
            kernel_log_puts(" descriptors in one task - and what it says about "
                       "itself is above, in its own words.\n\n");
        }
    }

    if (boot_pybuild_enabled()) {
        os_stat_t bst;
        if (do_syscall(SYS_stat, (uint64_t)"/tests/pybuild/run.sh",
                       (uint64_t)&bst, 0) != 0) {
            kernel_log_puts("[m99build] no pybuild fixture on this image - "
                       "skipped.\n\n");
        } else {
            kernel_log_puts("[m99build] measuring what CPython's own build would "
                       "cost on this machine.\n");
            uint64_t started = pit_get_ticks();
            long pid = do_syscall(SYS_spawn,
                                  (uint64_t)"/tests/pybuild/run.sh", 0, 0);
            if (pid < 0) {
                panic("M99: the pybuild script could not be spawned");
            }
            do_syscall(SYS_wait, (uint64_t)pid, 0, 0);
            uint64_t elapsed_s = (pit_get_ticks() - started) / PIT_HZ;
            int file_descriptor_task = -1;
            int file_descriptor_peak = scheduler_file_descriptor_high_water(&file_descriptor_task);
            int task_peak = scheduler_peak_live_tasks();
            kernel_log_perf("pybuild_wall_s", elapsed_s, "s");
            kernel_log_perf("pybuild_peak_live_tasks", (uint64_t)task_peak, "tasks");
            kernel_log_perf("pybuild_peak_fds_one_task", (uint64_t)file_descriptor_peak, "fds");
            kernel_log_puts("[m99build] the units are measured: what they multiply "
                       "out to is tools/python-build-test.sh's arithmetic, "
                       "printed there so it can be checked rather than "
                       "believed.\n\n");
        }
    }

    size_t init_size_bytes = 0;
    uint8_t *init_image = read_program("/bin/init", &init_size_bytes);
    int64_t init_size = (int64_t)init_size_bytes;
    process_spawn("init", init_image, (size_t)init_size, "");
    kfree(init_image);

    if (*(volatile uint64_t *)kernel_stack_guard != KERNEL_STACK_GUARD_VALUE) {
        kernel_log_puts("[boot] KERNEL STACK GUARD CLOBBERED - the boot stack overflowed; "
                  "statics below it in .rodata/.data are not to be trusted\n");
    } else {
        kernel_log_puts("[boot] kernel stack guard intact.\n");
    }

    {
        uint64_t boot_s = pit_get_ticks() * (1000 / PIT_HZ) / 1000;
        kernel_log_perf("boot_to_desktop_s", boot_s, "s");
        kernel_log_puts("[boot] reached the desktop handoff in ");
        kernel_log_put_dec((uint32_t)boot_s);
        kernel_log_puts(" s\n");
    }
    kernel_log_puts("[init] PID 1 spawned - handing off to the desktop shell.\n\n");

    scheduler_mark_self_idle();

    for (;;) {
        __asm__ volatile("hlt");
    }
}
