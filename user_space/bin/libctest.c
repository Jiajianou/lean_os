/* user_space/bin/libctest.c
 *
 * M63's self-test for the libc subset, and for the SSE support
 * underneath it. Spawned and waited on from kernel_main the way M19's
 * memtest and M57's fonttest are, and for the same reason: the thing
 * being proved only exists in ring 3.
 *
 * It is deliberately written *as a program*, using the standard headers
 * and nothing from this project - the same rules third_party/ is built
 * under. If it needed a lean_os header to work it would not be testing
 * what it claims to.
 *
 * The assertions are on values a maths library either gets right or does
 * not: sin(pi/6) is exactly a half, log(e) is exactly one, and the
 * formatter's job is that %e prints what the number is. Tolerances are
 * stated (1e-10) rather than "close enough", because math.h claims an
 * accuracy and this is where that claim can fail.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <langinfo.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

static int failures;

static void fail(const char *what) {
    printf("[libctest] FAIL: %s\n", what);
    failures++;
}

static void near(const char *what, double got, double want) {
    double d = got - want;
    if (d < 0) {
        d = -d;
    }
    /* Relative for large values, absolute for small - the usual rule,
     * and the one that keeps sin(pi) (which should be ~0) from being
     * judged by a relative error against zero. */
    double scale = want < 0 ? -want : want;
    double tol = scale > 1.0 ? scale * 1e-10 : 1e-10;
    if (d > tol) {
        printf("[libctest] FAIL: %s: got %.15g want %.15g\n", what, got, want);
        failures++;
    }
}

static void same(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        printf("[libctest] FAIL: %s: got \"%s\" want \"%s\"\n", what, got, want);
        failures++;
    }
}

/* ---- M80 groundwork: the helpers the new checks below need -----------
 *
 * At file scope because a comparison function, a thread body and a
 * signal handler cannot be locals - which is also why they are here
 * rather than inline in main with the checks they belong to. */
static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

static pthread_mutex_t cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_cond = PTHREAD_COND_INITIALIZER;
static volatile int cv_ready;
static volatile int handed_over;

static void *cv_signaller(void *arg) {
    (void)arg;
    /* A short spin first, so the main thread is genuinely inside
     * pthread_cond_wait when the signal arrives rather than having
     * raced past it - which would make this check pass without the
     * condition variable doing anything. */
    for (volatile int i = 0; i < 400000; i++) {
    }
    pthread_mutex_lock(&cv_lock);
    handed_over = 0x5A5A;
    cv_ready = 1;
    pthread_cond_signal(&cv_cond);
    pthread_mutex_unlock(&cv_lock);
    return 0;
}

static pthread_key_t tss_key;
static void *tss_other;

static void *tss_worker(void *arg) {
    (void)arg;
    pthread_setspecific(tss_key, (void *)0x2222);
    tss_other = pthread_getspecific(tss_key);
    return 0;
}

static volatile int usr_seen;

static void usr_handler(int sig) {
    (void)sig;
    usr_seen++;
}

int main(void) {
    /* ---- floating point exists at all ---- */
    volatile double a = 3.0, b = 7.0;
    if (a / b * b != 3.0 && fabs(a / b * b - 3.0) > 1e-15) {
        fail("double division does not round-trip");
    }

    /* ---- math.h ---- */
    near("sqrt(2)", sqrt(2.0), 1.4142135623730951);
    near("sin(pi/6)", sin(M_PI / 6.0), 0.5);
    near("cos(pi/3)", cos(M_PI / 3.0), 0.5);
    near("sin(0)", sin(0.0), 0.0);
    near("sin(1000)", sin(1000.0), 0.8268795405320025);   /* range reduction, not the series */
    near("cos(-40)", cos(-40.0), -0.6669380616522619);
    near("atan(1)", atan(1.0), M_PI / 4.0);
    near("atan(1e6)", atan(1000000.0), 1.5707953267948966);
    near("atan2(1,-1)", atan2(1.0, -1.0), 2.356194490192345);
    near("exp(1)", exp(1.0), M_E);
    near("exp(-20)", exp(-20.0), 2.061153622438558e-09);
    near("log(e)", log(M_E), 1.0);
    near("log(1e9)", log(1000000000.0), 20.72326583694641);
    near("pow(2,10)", pow(2.0, 10.0), 1024.0);
    near("pow(2,0.5)", pow(2.0, 0.5), 1.4142135623730951);
    near("fmod(7,3)", fmod(7.0, 3.0), 1.0);
    near("floor(-1.5)", floor(-1.5), -2.0);
    near("ceil(-1.5)", ceil(-1.5), -1.0);

    /* ---- stdio's formatter ---- */
    char buf[128];
    snprintf(buf, sizeof(buf), "%d %5d %-5d| %05d", 42, 42, 42, 42);
    same("integer widths", buf, "42    42 42   | 00042");
    snprintf(buf, sizeof(buf), "%x %X %o %u", 255u, 255u, 8u, 4000000000u);
    same("bases", buf, "ff FF 10 4000000000");
    snprintf(buf, sizeof(buf), "%s|%8s|%-8s|%.3s", "ab", "ab", "ab", "abcdef");
    same("string widths", buf, "ab|      ab|ab      |abc");
    snprintf(buf, sizeof(buf), "%.2f %.0f %.4f", 3.14159, 2.5, -0.00005);
    same("fixed point", buf, "3.14 3 -0.0001");
    snprintf(buf, sizeof(buf), "%e %.2e %12.4e", 1234.5, 0.000271828, 1234.5);
    same("scientific", buf, "1.234500e+03 2.72e-04   1.2345e+03");
    snprintf(buf, sizeof(buf), "%ld %c %%", 1234567890L, 'z');
    same("long and char", buf, "1234567890 z %");
    /* Truncation reports what it *would* have written, which is what
     * makes snprintf usable for sizing. */
    if (snprintf(buf, 4, "abcdef") != 6 || strcmp(buf, "abc") != 0) {
        fail("snprintf truncation");
    }

    /* ---- stdlib ---- */
    if (atoi("  -42xyz") != -42) {
        fail("atoi");
    }
    if (strtol("0x1f", (char **)0, 0) != 31) {
        fail("strtol base detection");
    }
    near("strtod", strtod("-3.5e2", (char **)0), -350.0);
    {
        char *p = (char *)malloc(100);
        memset(p, 0xAB, 100);
        char *q = (char *)realloc(p, 200);
        if (!q) {
            fail("realloc returned nothing");
        } else {
            for (int i = 0; i < 100; i++) {
                if ((unsigned char)q[i] != 0xAB) {
                    fail("realloc did not preserve the old contents");
                    break;
                }
            }
            free(q);
        }
        int *z = (int *)calloc(64, sizeof(int));
        for (int i = 0; i < 64; i++) {
            if (z[i] != 0) {
                fail("calloc did not zero");
                break;
            }
        }
        free(z);
    }

    /* ---- string ---- */
    {
        char s[32] = "hello";
        strcat(s, ", world");
        same("strcat", s, "hello, world");
        if (!strstr(s, "o, w") || strstr(s, "zzz")) {
            fail("strstr");
        }
        if (strchr(s, 'w') != s + 7 || strrchr(s, 'o') != s + 8) {
            fail("strchr/strrchr");
        }
        char ov[16] = "abcdefgh";
        memmove(ov + 2, ov, 6);
        same("memmove overlapping", ov, "ababcdef");
    }

    /* ---- time ---- */
    if (time((time_t *)0) <= 0) {
        fail("time() has no clock");
    }

    /* ---- and that a task switch preserves all of this ----
     *
     * The one thing a single-threaded test cannot check by arithmetic
     * alone: FXSAVE/FXRSTOR around a context switch. A long float loop
     * spans many scheduler quanta, so if the switch did not preserve xmm
     * state this sum would come back wrong - and it would come back
     * wrong *intermittently*, which is exactly the kind of bug worth a
     * deterministic assertion. */
    {
        double sum = 0.0;
        for (int i = 1; i <= 200000; i++) {
            sum += 1.0 / ((double)i * (double)i);
        }
        /* Converging on pi^2/6; 200000 terms gets within 5e-6 of it. */
        double want = M_PI * M_PI / 6.0;
        if (fabs(sum - want) > 1e-5) {
            printf("[libctest] FAIL: a float loop across scheduler quanta lost state: %.12f vs %.12f\n",
                   sum, want);
            failures++;
        }
    }

    /* ---- M80 groundwork: everything the CPython probe asked for -------
     *
     * Each block below checks a function this libc gained because
     * CPython's own source would not compile without it. They are
     * checked here rather than left to a future port for the reason this
     * project keeps rediscovering: a header that compiles and a function
     * that works are different claims, and the second one is the one
     * anybody will rely on.
     */

    /* setjmp/longjmp - the one pair that cannot be written in C, and the
     * one whose failure mode is a jump into nothing rather than a wrong
     * number. Both directions are checked: that setjmp returns 0 the
     * first time, that longjmp's value comes back, and that
     * longjmp(buf, 0) is turned into 1 as the standard requires. */
    {
        static jmp_buf env;
        volatile int stage = 0;
        int r = setjmp(env);
        if (r == 0) {
            if (stage != 0) {
                fail("setjmp: a local was clobbered before the first return");
            }
            stage = 1;
            longjmp(env, 42);
            fail("longjmp returned");
        } else if (r == 42) {
            if (stage != 1) {
                fail("setjmp: a volatile local did not survive longjmp");
            }
            stage = 2;
            longjmp(env, 0); /* must arrive as 1, not 0 */
        } else if (r == 1) {
            if (stage != 2) {
                fail("longjmp(buf, 0) arrived out of order");
            }
        } else {
            fail("longjmp delivered the wrong value");
        }
        if (stage != 2) {
            fail("longjmp(buf, 0) did not arrive as 1");
        }
    }

    /* strtoul / strtoll, including the base-0 prefixes and the endptr
     * contract that a caller uses to tell "no digits" from "zero". */
    {
        char *end;
        if (strtoul("0x1f", &end, 0) != 31 || *end != '\0') {
            fail("strtoul: base-0 hex");
        }
        if (strtoul("0755", &end, 0) != 493 || *end != '\0') {
            fail("strtoul: base-0 octal");
        }
        if (strtoul("  42abc", &end, 10) != 42 || *end != 'a') {
            fail("strtoul: endptr after the last digit");
        }
        if (strtoul("zzz", &end, 10) != 0 || end != (char *)0 + 0) {
            /* No digits: the standard says endptr comes back as the
             * ORIGINAL pointer, which is the only way a caller can tell
             * this from a genuine zero. */
        }
        const char *none = "zzz";
        if (strtoul(none, &end, 10) != 0 || end != none) {
            fail("strtoul: no-digits must hand back the original pointer");
        }
        if (strtoll("-9000000000", &end, 10) != -9000000000LL) {
            fail("strtoll: a value past 32 bits");
        }
    }

    /* qsort / bsearch over a type big enough that a byte-wise swap is
     * doing real work. */
    {
        int a[9] = {5, 3, 9, 1, 7, 3, 8, 0, 2};
        qsort(a, 9, sizeof(int), cmp_int);
        for (int i = 1; i < 9; i++) {
            if (a[i - 1] > a[i]) {
                fail("qsort did not sort");
                break;
            }
        }
        int key = 7;
        int *hit = (int *)bsearch(&key, a, 9, sizeof(int), cmp_int);
        if (!hit || *hit != 7) {
            fail("bsearch did not find a present key");
        }
        key = 6;
        if (bsearch(&key, a, 9, sizeof(int), cmp_int)) {
            fail("bsearch found a key that is not there");
        }
    }

    /* strpbrk / strdup / strerror. */
    {
        const char *hay = "abc:def";
        if (strpbrk(hay, ":=") != hay + 3) {
            fail("strpbrk");
        }
        char *dup = strdup("copied");
        if (!dup || strcmp(dup, "copied") != 0) {
            fail("strdup");
        }
        free(dup);
        if (strcmp(strerror(ENOENT), "ENOENT") != 0) {
            fail("strerror does not name the code");
        }
    }

    /* The maths additions. frexp is the one that has to be exact - it is
     * how a program takes a double apart - so it is checked as an
     * identity rather than against a tolerance. */
    {
        int e = 0;
        double m = frexp(3072.0, &e);
        if (m != 0.75 || e != 12) {
            printf("[libctest] FAIL: frexp(3072) = %g x 2^%d\n", m, e);
            failures++;
        }
        if (ldexp(m, e) != 3072.0) {
            fail("ldexp did not invert frexp exactly");
        }
        double ip = 0.0;
        if (modf(-3.25, &ip) != -0.25 || ip != -3.0) {
            fail("modf");
        }
        if (trunc(-2.7) != -2.0 || round(-2.5) != -3.0 || round(2.5) != 3.0) {
            fail("trunc/round");
        }
        if (copysign(3.0, -0.5) != -3.0) {
            fail("copysign");
        }
        near("hypot(3,4)", hypot(3.0, 4.0), 5.0);
        near("log2(1024)", log2(1024.0), 10.0);
        near("tanh(0)", tanh(0.0), 0.0);
        if (!isnan(0.0 / 0.0) || !isinf(1.0 / 0.0) || !isfinite(1.0)) {
            fail("isnan/isinf/isfinite");
        }
        if (fmax(1.0, 2.0) != 2.0 || fmin(1.0, 2.0) != 1.0) {
            fail("fmax/fmin");
        }
    }

    /* The calendar. A round trip through a known instant, so that a
     * wrong epoch, a wrong month base or a wrong weekday all show up. */
    {
        struct tm tm;
        time_t t = 1000000000; /* 2001-09-09T01:46:40Z, a Sunday */
        gmtime_r(&t, &tm);
        if (tm.tm_year + 1900 != 2001 || tm.tm_mon != 8 || tm.tm_mday != 9 ||
            tm.tm_hour != 1 || tm.tm_min != 46 || tm.tm_sec != 40 || tm.tm_wday != 0) {
            printf("[libctest] FAIL: gmtime gave %04d-%02d-%02d %02d:%02d:%02d wday=%d\n",
                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                   tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday);
            failures++;
        }
        if (timegm(&tm) != t) {
            fail("timegm did not invert gmtime");
        }
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %a", &tm);
        same("strftime", buf, "2001-09-09 01:46:40 Sun");
        strftime(buf, sizeof(buf), "%F %T", &tm);
        same("strftime %F %T", buf, "2001-09-09 01:46:40");
    }

    /* The wide-character subset, including the Latin-1 conversion this
     * system is honest about being. */
    {
        wchar_t w[16];
        if (mbstowcs(w, "wide", 16) != 4 || wcslen(w) != 4 || w[0] != L'w') {
            fail("mbstowcs");
        }
        char back[16];
        if (wcstombs(back, w, 16) != 4 || strcmp(back, "wide") != 0) {
            fail("wcstombs did not round-trip");
        }
        if (wcscmp(w, L"wide") != 0 || wcschr(w, L'd') != w + 2 ||
            wcsrchr(w, L'e') != w + 3) {
            fail("wcscmp/wcschr/wcsrchr");
        }
        wchar_t nums[] = L"0x2a rest";
        wchar_t *wend;
        if (wcstol(nums, &wend, 0) != 42 || *wend != L' ') {
            fail("wcstol");
        }
        /* Above U+00FF is deliberately not representable - checked so
         * that the limit <wchar.h> documents is a limit rather than a
         * silent truncation. */
        wchar_t big[2] = {0x1F600, 0};
        if (wcstombs(back, big, 16) != (size_t)-1) {
            fail("wcstombs accepted a code point it cannot represent");
        }
    }

    /* The locale, which has exactly one honest answer. */
    {
        if (strcmp(setlocale(LC_ALL, "C"), "C") != 0) {
            fail("setlocale(\"C\")");
        }
        if (setlocale(LC_ALL, "en_US.UTF-8") != (char *)0) {
            fail("setlocale accepted a locale this system does not have");
        }
        if (strcmp(localeconv()->decimal_point, ".") != 0) {
            fail("localeconv");
        }
        if (strcmp(nl_langinfo(CODESET), "ANSI_X3.4-1968") != 0) {
            fail("nl_langinfo(CODESET) is not ASCII");
        }
    }

    /* The clocks. Monotonic has to actually be monotonic, which is the
     * only property a caller names it for. */
    {
        struct timespec a, b;
        if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) {
            fail("clock_gettime(CLOCK_MONOTONIC)");
        }
        for (volatile int i = 0; i < 200000; i++) {
            /* Real work between two clock reads, so that "monotonic"
             * is being asked about an interval rather than about two
             * samples the compiler could have folded together. */
        }
        if (clock_gettime(CLOCK_MONOTONIC, &b) != 0 ||
            b.tv_sec < a.tv_sec ||
            (b.tv_sec == a.tv_sec && b.tv_nsec < a.tv_nsec)) {
            fail("CLOCK_MONOTONIC went backwards");
        }
        struct timespec res;
        if (clock_getres(CLOCK_MONOTONIC, &res) != 0 || res.tv_nsec != 1000000L) {
            fail("clock_getres does not report a millisecond");
        }
        struct timeval tv;
        if (gettimeofday(&tv, 0) != 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) {
            fail("gettimeofday");
        }
    }

    /* mmap through <sys/mman.h>, which is M78 reached by its POSIX name
     * rather than by sys_mmap. */
    {
        void *p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) {
            fail("mmap through <sys/mman.h>");
        } else {
            ((char *)p)[0] = 'x';
            ((char *)p)[8191] = 'y';
            if (((char *)p)[0] != 'x' || ((char *)p)[8191] != 'y') {
                fail("an mmap'd page did not hold what was written to it");
            }
            if (munmap(p, 8192) != 0) {
                fail("munmap");
            }
        }
        /* addr and fd are refused rather than ignored - see <sys/mman.h> */
        if (mmap((void *)0x8000000000UL, 4096, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0) != MAP_FAILED) {
            fail("mmap accepted an address hint it cannot honour");
        }
    }

    /* A condition variable, which is M79's deferred item and the thing
     * CPython's GIL is built from. The wait has to actually wait: the
     * signal comes from a second thread, so a cond_wait that returned
     * immediately would leave `handed_over` still zero. */
    {
        if (pthread_mutex_init(&cv_lock, 0) != 0 || pthread_cond_init(&cv_cond, 0) != 0) {
            fail("pthread_cond_init");
        }
        pthread_t th;
        if (pthread_create(&th, 0, cv_signaller, 0) != 0) {
            fail("pthread_create for the condition-variable check");
        } else {
            pthread_mutex_lock(&cv_lock);
            while (!cv_ready) {
                pthread_cond_wait(&cv_cond, &cv_lock);
            }
            int seen = handed_over;
            pthread_mutex_unlock(&cv_lock);
            pthread_join(th, 0);
            if (seen != 0x5A5A) {
                fail("a condition variable did not carry the other thread's write");
            }
        }
        pthread_cond_destroy(&cv_cond);
        pthread_mutex_destroy(&cv_lock);
    }

    /* Thread-specific storage, which is what CPython's headers refused to
     * compile without. Two threads must see two different values through
     * the same key - which is the entire claim. */
    {
        if (pthread_key_create(&tss_key, 0) != 0) {
            fail("pthread_key_create");
        } else {
            pthread_setspecific(tss_key, (void *)0x1111);
            pthread_t th;
            if (pthread_create(&th, 0, tss_worker, 0) != 0) {
                fail("pthread_create for the TSS check");
            } else {
                pthread_join(th, 0);
                if (tss_other != (void *)0x2222) {
                    fail("a thread did not see its own TSS value");
                }
            }
            if (pthread_getspecific(tss_key) != (void *)0x1111) {
                fail("TSS: this thread's value was overwritten by another's");
            }
            pthread_key_delete(tss_key);
        }
    }

    /* <fcntl.h>'s open, and the fcntl that refuses what it cannot do. */
    {
        int fd = open("/tmp/libctest.tmp", O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            fail("open through <fcntl.h>");
        } else {
            if (write(fd, "ok", 2) != 2) {
                fail("write to an fd from open()");
            }
            if (fcntl(fd, F_GETFD) != 0 || fcntl(fd, F_GETFL) != 0) {
                fail("fcntl: a flag that is not set should read as 0");
            }
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0) {
                fail("fcntl accepted a flag it cannot honour");
            }
            close(fd);
            unlink("/tmp/libctest.tmp");
        }
        if (!isatty(1)) {
            fail("isatty(1) should be true - fd 1 is the implicit stdout");
        }
    }

    /* sigaction, which is M76 reached by its POSIX name. Installing and
     * reading back is the whole surface a ported program uses. */
    {
        struct sigaction sa, old;
        sa.sa_handler = usr_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sa.sa_sigaction = 0;
        if (sigaction(SIGUSR1, &sa, &old) != 0) {
            fail("sigaction could not install a handler");
        }
        raise(SIGUSR1);
        (void)getpid(); /* a syscall, so the handler is delivered on the way out */
        if (usr_seen != 1) {
            fail("a handler installed with sigaction did not run");
        }
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);
        if (!sigismember(&set, SIGUSR2) || sigismember(&set, SIGUSR1)) {
            fail("sigaddset/sigismember");
        }
    }

    if (failures) {
        return 1;
    }
    printf("[libctest] libc subset and SSE verified.\n");
    return 0;
}
