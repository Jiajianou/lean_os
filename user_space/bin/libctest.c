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
#include <poll.h>
#include <sys/select.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h> /* M100 */
#include <pwd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/times.h>
#include <utime.h>
#include <sys/time.h>
#include <sys/wait.h>
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
    /* M98: "3 " became "2 ", and the host's own printf is the authority:
     * 2.5 is exactly representable, ties round to even, and every printf
     * on x86 says 2. The -0.0001 is the other side of the same coin -
     * -0.00005's double sits just ABOVE the tie, which is exactly the
     * distinction tools/printf-test.sh now grades. This line asserted
     * what the old engine happened to do; the differential test is what
     * says what it should do. */
    snprintf(buf, sizeof(buf), "%.2f %.0f %.4f", 3.14159, 2.5, -0.00005);
    same("fixed point", buf, "3.14 2 -0.0001");
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

    /* The wide-character subset, and the UTF-8 conversion under it.
     * M88 changed what this section asserts: the encoding used to be
     * Latin-1 and the test below used to require a code point above
     * U+00FF to be REFUSED. It is now encoded, which is the point of
     * that milestone. The exhaustive grading of the encoding lives in
     * tests/test_utf8.c, where the malformed sequences a booted machine
     * cannot produce can be written down as bytes; what is checked here
     * is that the same code behaves the same way in ring 3, on this
     * machine, against this libc's own <wchar.h>. */
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
        /* M88: four bytes out, and the same character back. The old
         * assertion here was that this returned -1. */
        wchar_t big[2] = {0x1F600, 0};
        if (wcstombs(back, big, 16) != 4) {
            fail("wcstombs did not encode a four-byte character");
        }
        wchar_t round[4];
        if (mbstowcs(round, back, 4) != 1 || round[0] != (wchar_t)0x1F600) {
            fail("a four-byte character did not round-trip");
        }
        /* And a sequence that is not valid UTF-8 is still refused, which
         * is the half of this that did not change. */
        if (mbstowcs(round, "\xC0\xAF", 4) != (size_t)-1) {
            fail("mbstowcs accepted an overlong encoding");
        }
        if (MB_CUR_MAX != 4) {
            fail("MB_CUR_MAX is not 4");
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
        /* M88: UTF-8, because the conversions are now real - see
         * <langinfo.h> for the argument this reverses. */
        if (strcmp(nl_langinfo(CODESET), "UTF-8") != 0) {
            fail("nl_langinfo(CODESET) is not UTF-8");
        }
        if (strcmp(setlocale(LC_ALL, "C.UTF-8"), "C") != 0) {
            fail("setlocale(\"C.UTF-8\")");
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
        /* M91: an address is a hint now, not a refusal. This used to
         * assert the opposite - "addr and fd are refused rather than
         * ignored" - which was the honest contract while this kernel
         * placed every mapping itself. It places them where it is asked
         * to now, so the check is inverted rather than deleted, the same
         * way M88's racetest was when SYS_waitfds started accepting a
         * count of zero.
         *
         * 0x8000000000 is this program's own load address, so it is a
         * hint that CANNOT be honoured - which is exactly the interesting
         * case: an unusable hint must be quietly ignored and a mapping
         * made somewhere legal, not refused and not granted on top of the
         * caller's own text. The address that comes back is the proof,
         * and it must not be the one asked for. */
        void *hinted = mmap((void *)0x8000000000UL, 4096, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (hinted == MAP_FAILED) {
            fail("mmap refused an unusable address hint instead of ignoring it");
        } else {
            if (hinted == (void *)0x8000000000UL) {
                fail("mmap honoured an address hint over the program's own image");
            }
            ((char *)hinted)[0] = 'z';
            if (((char *)hinted)[0] != 'z') {
                fail("the mapping made in place of an unusable hint is not usable");
            }
            munmap(hinted, 4096);
        }
        /* M91 (second attempt): a descriptor WITH MAP_ANONYMOUS is still
         * refused, and now for a sharper reason than "there are no
         * file-backed mappings here". There are; POSIX says the
         * descriptor is ignored for an anonymous mapping, and this
         * kernel refuses it instead - a caller who passed one is a
         * caller who meant to map a file, and silently handing them
         * zeroes is the thing every refusal in this file exists to
         * avoid. */
        if (mmap(0, 4096, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, 3, 0) != MAP_FAILED) {
            fail("mmap accepted a file descriptor it cannot honour");
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

    /* <fcntl.h>'s open, the one descriptor flag this machine has, and the
     * fcntl that still refuses what it cannot do.
     *
     * M84 changed half of this test, and the half it changed is the point:
     * F_SETFD used to be asserted to FAIL, because there was no exec on
     * this machine and FD_CLOEXEC had nothing to mean. There is now, so it
     * has to succeed and read back. F_SETFL is unchanged and still has to
     * fail, because O_NONBLOCK is still a flag nothing here can honour -
     * which is what keeps this from being a test that just believes
     * whatever fcntl says. */
    {
        int fd = open("/tmp/libctest.tmp", O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            fail("open through <fcntl.h>");
        } else {
            if (write(fd, "ok", 2) != 2) {
                fail("write to an fd from open()");
            }
            if (fcntl(fd, F_GETFD) != 0) {
                fail("fcntl: a flag that is not set should read as 0");
            }
            /* M98: F_GETFL carries the ACCESS MODE now (the kernel's
             * answer, and the reason is in <fcntl.h>: 0 is not a valid
             * access mode in this encoding, and BFD aborts on it). This
             * fd was opened O_WRONLY, and on this kernel a disk file is
             * always readable too - openfile_t records only `writable`
             * - so the honest expectation is O_RDWR, exactly. Not
             * "& O_ACCMODE": a fresh descriptor has no status flag set,
             * so any stray bit in the answer would be a bit that means
             * nothing, and equality is what catches one. */
            if (fcntl(fd, F_GETFL) != O_RDWR) {
                fail("fcntl: F_GETFL should report the access mode (M98)");
            }
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
                fcntl(fd, F_GETFD) != FD_CLOEXEC) {
                fail("fcntl: FD_CLOEXEC did not set and read back (M84)");
            }
            if (fcntl(fd, F_SETFD, 0) != 0 || fcntl(fd, F_GETFD) != 0) {
                fail("fcntl: FD_CLOEXEC did not clear again");
            }
            /* M100: F_SETFL is real. O_NONBLOCK sets, reads back beside
             * the access mode, and clears; a bit that names no flag
             * here (this is O_RDONLY's bit, which POSIX says F_SETFL
             * ignores) is accepted and changes nothing - which is what
             * the check that stood here, "accepted a status flag it
             * cannot honour", encoded backwards: it required refusal,
             * and what a program needs is that the answer stays true. */
            if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0 || fcntl(fd, F_GETFL) != (O_RDWR | O_NONBLOCK)) {
                fail("fcntl: O_NONBLOCK did not set and read back beside the access mode");
            }
            if (fcntl(fd, F_SETFL, 0) != 0 || fcntl(fd, F_GETFL) != O_RDWR) {
                fail("fcntl: O_NONBLOCK did not clear");
            }
            if (fcntl(fd, F_SETFL, 1) != 0 || fcntl(fd, F_GETFL) != O_RDWR) {
                fail("fcntl: a flag this kernel has no notion of changed the answer");
            }
            close(fd);
            unlink("/tmp/libctest.tmp");
        }
        if (!isatty(1)) {
            fail("isatty(1) should be true - fd 1 is the implicit stdout");
        }
    }

    /* M88: poll, over the pipe every program on this desktop already
     * uses. Three states in order, because the interesting thing about
     * poll is not that it answers but that it answers *differently*:
     * nothing ready and a deadline that expires, then something ready,
     * then the same descriptor no longer ready once it is drained. A
     * test that only checked the middle one would pass against a poll
     * that always said yes. */
    {
        int pf[2];
        if (pipe(pf) != 0) {
            fail("pipe() for the poll test");
        } else {
            struct pollfd p;
            p.fd = pf[0];
            p.events = POLLIN;
            p.revents = 0;

            /* Nothing written yet: this must time out rather than
             * report readable, and must actually wait rather than
             * return immediately - which is the difference between a
             * blocking poll and a busy loop. */
            if (poll(&p, 1, 30) != 0 || p.revents != 0) {
                fail("poll: an empty pipe reported ready");
            }

            if (write(pf[1], "x", 1) != 1) {
                fail("poll: could not write to the pipe");
            } else {
                p.revents = 0;
                if (poll(&p, 1, 200) != 1 || !(p.revents & POLLIN)) {
                    fail("poll: a pipe with a byte in it did not report POLLIN");
                }
            }

            /* Drained, and not ready again. */
            char got = 0;
            if (read(pf[0], &got, 1) != 1 || got != 'x') {
                fail("poll: the byte did not read back");
            }
            p.revents = 0;
            if (poll(&p, 1, 30) != 0 || p.revents != 0) {
                fail("poll: a drained pipe still reported ready");
            }

            /* A negative fd is skipped with revents 0 - POSIX's way of
             * letting a program keep a fixed array and disable entries
             * in it, and the one case where poll must ignore rather
             * than refuse. */
            struct pollfd skip[2];
            skip[0].fd = -1;
            skip[0].events = POLLIN;
            skip[0].revents = 0xFF;
            skip[1].fd = pf[0];
            skip[1].events = POLLIN;
            skip[1].revents = 0;
            if (poll(skip, 2, 10) != 0 || skip[0].revents != 0) {
                fail("poll: a negative fd was not skipped");
            }

            close(pf[0]);
            close(pf[1]);
        }
    }

    /* M88 (second attempt): select, which is poll with the descriptor
     * set written as a bitmap.
     *
     * The same three states in the same order and for the same reason -
     * a set that always said yes would pass a test that only checked the
     * middle one. What is checked *beyond* poll is the part select gets
     * wrong everywhere: the sets are modified in place, so a descriptor
     * the caller set and that turned out not to be ready has to come
     * back CLEAR. A program that rebuilds its set every time round the
     * loop (which is what every correct select caller does) would never
     * notice; a program that does not, hangs.
     */
    {
        int sf[2];
        if (pipe(sf) != 0) {
            fail("pipe() for the select test");
        } else {
            fd_set r;
            struct timeval tv;

            FD_ZERO(&r);
            FD_SET(sf[0], &r);
            tv.tv_sec = 0;
            tv.tv_usec = 30000;
            if (select(sf[0] + 1, &r, 0, 0, &tv) != 0) {
                fail("select: an empty pipe reported ready");
            }
            if (FD_ISSET(sf[0], &r)) {
                fail("select: a descriptor that was not ready came back set");
            }

            if (write(sf[1], "y", 1) != 1) {
                fail("select: could not write to the pipe");
            } else {
                FD_ZERO(&r);
                FD_SET(sf[0], &r);
                tv.tv_sec = 0;
                tv.tv_usec = 200000;
                if (select(sf[0] + 1, &r, 0, 0, &tv) != 1 || !FD_ISSET(sf[0], &r)) {
                    fail("select: a pipe with a byte in it did not report readable");
                }
            }

            char got = 0;
            if (read(sf[0], &got, 1) != 1 || got != 'y') {
                fail("select: the byte did not read back");
            }

            /* Two descriptors, one ready and one not, in one call - which
             * is the whole reason select takes a set rather than an fd.
             * The count is what is asserted: a select that reported
             * "something happened" without saying how many would pass
             * every check above. */
            {
                int other[2];
                if (pipe(other) != 0) {
                    fail("second pipe() for the select test");
                } else {
                    if (write(other[1], "z", 1) != 1) {
                        fail("select: could not write to the second pipe");
                    }
                    FD_ZERO(&r);
                    FD_SET(sf[0], &r);
                    FD_SET(other[0], &r);
                    int max = sf[0] > other[0] ? sf[0] : other[0];
                    tv.tv_sec = 0;
                    tv.tv_usec = 200000;
                    int n = select(max + 1, &r, 0, 0, &tv);
                    if (n != 1) {
                        printf("libctest: select reported %d ready, expected 1\n", n);
                        fail("select: the wrong number of descriptors was ready");
                    }
                    if (!FD_ISSET(other[0], &r) || FD_ISSET(sf[0], &r)) {
                        fail("select: the ready descriptor was not the one with the byte");
                    }
                    close(other[0]);
                    close(other[1]);
                }
            }

            /* Writability, which this machine always reports - see
             * <sys/select.h> for why that is the honest answer rather
             * than a shortcut, and asserted here so the day it stops
             * being true the claim in that header fails with it. */
            {
                fd_set w;
                FD_ZERO(&w);
                FD_SET(sf[1], &w);
                tv.tv_sec = 0;
                tv.tv_usec = 0;
                if (select(sf[1] + 1, 0, &w, 0, &tv) != 1 || !FD_ISSET(sf[1], &w)) {
                    fail("select: a writable descriptor was not reported writable");
                }
            }

            /* exceptfds comes back empty, always. There is no
             * out-of-band data in this stack to put in it. */
            {
                fd_set e;
                FD_ZERO(&e);
                FD_SET(sf[0], &e);
                tv.tv_sec = 0;
                tv.tv_usec = 0;
                if (select(sf[0] + 1, 0, 0, &e, &tv) != 0 || FD_ISSET(sf[0], &e)) {
                    fail("select: exceptfds reported an exceptional condition");
                }
            }

            /* An empty wait with a deadline is a sleep, exactly as
             * poll's is - and nfds of 0 must not be an error. */
            tv.tv_sec = 0;
            tv.tv_usec = 20000;
            if (select(0, 0, 0, 0, &tv) != 0) {
                fail("select: an empty set with a timeout was not a sleep");
            }

            close(sf[0]);
            close(sf[1]);
        }
    }

    /* M98: four headers, tested for WHERE they declare things ----------
     *
     * Every one of these came from building binutils for this target,
     * and every one is the same failure: something this libc has, or
     * ought to have, that is not visible from the header a program
     * actually includes. M94 wrote the rule after finding `wcwidth` in
     * <wctype.h> and not <wchar.h>: **a header that has a function and
     * does not declare it where the standard says is, to a build,
     * indistinguishable from not having it.**
     *
     * So these checks are deliberately about the include, not the
     * behaviour. Each block includes exactly what a real program would
     * and uses what that entitles it to.
     */
    {
        /* bfd/sysdep.h includes <string.h> and nothing else, then calls
         * all three of these. On glibc <string.h> pulls in <strings.h>;
         * here it did not, so the whole of bfd failed to compile against
         * three functions that had existed since M89. This file includes
         * <string.h> above and does not include <strings.h>. */
        if (strcasecmp("AbC", "aBc") != 0) {
            fail("strcasecmp is not visible from <string.h>, or is wrong");
        }
        if (strncasecmp("AbCxx", "aBcyy", 3) != 0) {
            fail("strncasecmp is not visible from <string.h>, or is wrong");
        }
        if (ffs(0) != 0 || ffs(1) != 1 || ffs(8) != 4) {
            fail("ffs is not visible from <string.h>, or is wrong");
        }

        /* bfd/archive.c reads a member size with sscanf("%" SCNu64).
         * <inttypes.h> here had the whole PRI* family and NOT ONE SCN*,
         * so that was a format string ending in a bare '%'. */
        {
            uint64_t parsed = 0;
            if (sscanf("1234567890123", "%" SCNu64, &parsed) != 1 ||
                parsed != 1234567890123ULL) {
                fail("SCNu64 does not scan a 64-bit value");
            }
            long long signed_parsed = 0;
            if (sscanf("-42", "%" SCNd64, &signed_parsed) != 1 ||
                signed_parsed != -42) {
                fail("SCNd64 does not scan a signed 64-bit value");
            }
        }

        /* bfd/elf-properties.c calls _exit(EXIT_FAILURE). <stdlib.h>
         * here had neither constant - undeclared identifiers in a file
         * with nothing to do with exit codes. Checked for their VALUES
         * as well as their presence, because a program that returns
         * EXIT_FAILURE and exits 0 is worse than one that will not
         * compile. */
        if (EXIT_SUCCESS != 0 || EXIT_FAILURE == 0) {
            fail("EXIT_SUCCESS/EXIT_FAILURE are not 0 and non-zero");
        }

        /* mktemp: genuinely missing, and the first thing that stopped
         * binutils - libiberty's choose-temp.c calls it by name. It
         * picks a name that does not exist; see <stdlib.h> for why this
         * libc provides a call every modern system deprecates. */
        {
            char tmpl[] = "/tmp/libctest-mkXXXXXX";
            char *got = mktemp(tmpl);
            if (got != tmpl || tmpl[0] == '\0') {
                fail("mktemp did not produce a name");
            }
            if (strncmp(tmpl, "/tmp/libctest-mk", 16) != 0) {
                fail("mktemp overwrote the part of the template it must keep");
            }
            /* The one property it can honestly promise: the name it
             * returned did not exist when it returned it. */
            int fd = open(tmpl, O_RDONLY);
            if (fd >= 0) {
                close(fd);
                fail("mktemp returned a name that already existed");
            }
        }
    }

    /* M88: the clock advances, and says so when it does not.
     *
     * This check exists because of a failure that took three attempts to
     * understand. The M63 self-test kept reporting Whetstone's
     * "Insufficient duration - Increase the LOOP count", which is what
     * that benchmark prints when its start and end timestamps are equal.
     * Two fixes were made to the RTC on two different theories and it
     * came back both times - and neither the benchmark nor the self-test
     * could say WHICH way it was broken, because "the clock read zero
     * twice" and "the clock read the same number twice" print
     * identically from inside Whetstone.
     *
     * So the clock is now asserted directly, and the failure names
     * itself. A non-positive reading means the clock is not answering at
     * all; two equal readings a second apart mean it is answering and
     * not advancing. Those are different bugs and this is the difference
     * between them.
     *
     * The sleep is poll's, which is what M88 made possible - an empty
     * set with a deadline is a sleep, and it is the only one this libc
     * has. */
    {
        time_t before = time(0);
        if (before <= 0) {
            printf("libctest: time() returned %ld\n", (long)before);
            fail("the clock is not answering at all");
        } else {
            poll((struct pollfd *)0, 0, 1500);
            time_t after = time(0);
            if (after <= before) {
                printf("libctest: time() read %ld then %ld across a 1.5s sleep\n",
                       (long)before, (long)after);
                fail("the clock is answering but not advancing");
            }
        }
    }

    /* M88: identity and sysconf. What is checked is not the values but
     * the *shape* of the answers - that the real and effective ids agree
     * (there is no setuid here for them to differ about), that the
     * constants this machine can state are stated, and that the ones it
     * cannot are -1 rather than a plausible number a configure script
     * would then build against. */
    {
        if (getuid() != geteuid() || getgid() != getegid()) {
            fail("real and effective ids differ on a machine with no setuid");
        }
        if (sysconf(_SC_PAGESIZE) != 4096 || getpagesize() != 4096) {
            fail("sysconf(_SC_PAGESIZE) does not match the page size this kernel maps");
        }
        if (sysconf(_SC_OPEN_MAX) <= 0 || sysconf(_SC_CLK_TCK) <= 0) {
            fail("sysconf could not state a limit this machine does have");
        }
        /* M89: this asserted -1 for one milestone, on the grounds that
         * the kernel knew and no syscall reported it. SYS_meminfo does
         * now, so the assertion inverts - and what is checked is the
         * *relationship* rather than a number, because the number is a
         * fact about the guest QEMU was given rather than about this
         * code: there must be memory, and the free part of it cannot be
         * more than all of it. A bug that swapped the two fields, or one
         * that reported bytes where frames were meant, fails this. */
        long phys = sysconf(_SC_PHYS_PAGES);
        long avail = sysconf(_SC_AVPHYS_PAGES);
        if (phys <= 0 || avail < 0 || avail > phys) {
            fail("sysconf's memory answers are not a machine's memory");
        }
        /* M89: the POSIX options, which are the useful half of sysconf.
         * Two that must be positive because this machine genuinely has
         * them, and two that must be -1 because it genuinely does not -
         * the second pair being the half a stub gets wrong, since a
         * sysconf that answered 200809L for everything would satisfy
         * every positive check ever written. */
        if (sysconf(_SC_THREADS) <= 0 || sysconf(_SC_JOB_CONTROL) <= 0) {
            fail("sysconf denies an option this machine implements");
        }
        if (sysconf(_SC_MESSAGE_PASSING) != -1 || sysconf(_SC_TIMERS) != -1) {
            fail("sysconf claims an option this machine does not have");
        }
        if (sysconf(-12345) != -1) {
            fail("sysconf accepted a name it does not know");
        }
        /* M88 (second attempt): the password database, which has one row
         * because this machine has one principal. What is checked is
         * that it says so both ways and refuses everything else - a
         * database that answered about uid 1000 would be inventing a
         * second principal, which is the thing M65 refused. */
        struct passwd *pw = getpwuid(getuid());
        if (!pw || strcmp(pw->pw_name, "root") != 0 || pw->pw_uid != 0) {
            fail("getpwuid did not describe this machine's one principal");
        }
        if (getpwnam("root") != pw || getpwnam("nobody") != (struct passwd *)0) {
            fail("getpwnam disagrees with getpwuid");
        }
        if (getpwuid(1000) != (struct passwd *)0) {
            fail("getpwuid invented a user this machine does not have");
        }
        setpwent();
        if (getpwent() != pw || getpwent() != (struct passwd *)0) {
            fail("getpwent does not walk a database of exactly one");
        }
        endpwent();
    }

    /* sigaction, which is M76 reached by its POSIX name. Installing and
     * reading back is the whole surface a ported program uses. */
    {
        struct sigaction sa, old;
        /* M99: sa_handler and sa_sigaction are a union now, as they are
         * on every real system - so the `sa.sa_sigaction = 0;` that
         * stood here would clear the handler that was just assigned. */
        sa.sa_handler = usr_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
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

    /* M88 (second attempt): the three calls a build probes for, asked on
     * the machine rather than on the host - because every one of them
     * reports something only a running kernel knows.
     *
     * The assertions are about relationships rather than values. What
     * makes a CPU-time counter wrong is not a number this test could
     * predict; it is that the number does not move when the process
     * burns a slice, or that it moves when the process sleeps, or that
     * it counts somebody else's work. Those are all checkable without
     * knowing what the right answer is. */
    {
        struct tms t0, t1;
        clock_t r0 = times(&t0);
        if (r0 == (clock_t)-1) {
            fail("times() failed");
        }
        /* Long enough to cross several 10 ms ticks - the counter has a
         * tick's granularity, so a shorter loop can honestly report
         * zero and this would be a flaky test rather than a wrong one. */
        volatile unsigned long spin = 0;
        for (unsigned long i = 0; i < 40000000UL; i++) {
            spin += i;
        }
        (void)spin; /* the work is the point; the sum is not */
        clock_t r1 = times(&t1);
        if (t1.tms_utime <= t0.tms_utime) {
            printf("[libctest] utime %ld -> %ld\n", (long)t0.tms_utime, (long)t1.tms_utime);
            fail("times() did not charge a busy loop to user time");
        }
        if (r1 < r0) {
            fail("times() elapsed clock went backwards");
        }
        /* A process with no children has no reaped child time, and this
         * one has spawned nothing. A counter that reported some would be
         * reading the wrong task. */
        if (t1.tms_cutime != 0 || t1.tms_cstime != 0) {
            fail("times() reported child time in a process with no children");
        }

        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) != 0) {
            fail("getrusage(RUSAGE_SELF)");
        }
        /* The same time, in the other unit. tms_utime is in ticks and
         * ru_utime is in seconds and microseconds; they must describe
         * the same quantity, which is the check that catches a scaling
         * error in either direction. */
        long from_ticks = (long)t1.tms_utime * (1000000L / sysconf(_SC_CLK_TCK));
        long from_rusage = (long)ru.ru_utime.tv_sec * 1000000L + ru.ru_utime.tv_usec;
        long diff = from_ticks - from_rusage;
        if (diff < 0) {
            diff = -diff;
        }
        if (diff > 2000000L / sysconf(_SC_CLK_TCK)) {
            fail("times() and getrusage() disagree about this process's user time");
        }
        if (getrusage(12345, &ru) != -1) {
            fail("getrusage accepted a `who` it does not have");
        }
        /* clock() is the third spelling of the same number, and M88 made
         * it real - it returned uptime before, with a comment saying so. */
        if (clock() == (clock_t)-1) {
            fail("clock() failed");
        }

        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur != (rlim_t)sysconf(_SC_OPEN_MAX)) {
            fail("getrlimit(RLIMIT_NOFILE) does not agree with sysconf");
        }
        /* Setting a limit to what it already is changes nothing and is
         * granted; anything else is refused rather than accepted and
         * ignored - see <sys/resource.h>. */
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            fail("setrlimit refused a request to change nothing");
        }
        rl.rlim_cur = 8;
        if (setrlimit(RLIMIT_NOFILE, &rl) != -1 || errno != EPERM) {
            fail("setrlimit agreed to enforce a limit nothing here enforces");
        }
    }

    /* statvfs and utime, which need a real filesystem underneath. */
    {
        struct statvfs vfs;
        if (statvfs("/", &vfs) != 0) {
            fail("statvfs(\"/\")");
        }
        if (vfs.f_bsize == 0 || vfs.f_blocks == 0 || vfs.f_bfree > vfs.f_blocks) {
            fail("statvfs reported an impossible filesystem");
        }
        if (vfs.f_files == 0 || vfs.f_ffree > vfs.f_files) {
            fail("statvfs reported an impossible inode count");
        }
        if (vfs.f_namemax == 0) {
            fail("statvfs did not report a name limit");
        }
        if (statvfs("/no/such/path", &vfs) != -1) {
            fail("statvfs answered about a path that does not exist");
        }
        /* /proc has no blocks, and saying zero would be a number a
         * caller divides by - see the ABI note at SYS_statvfs. */
        if (statvfs("/proc", &vfs) != -1) {
            fail("statvfs described a synthetic filesystem with numbers");
        }

        const char *path = "/tmp_utime_test";
        FILE *f = fopen(path, "w");
        if (!f) {
            fail("could not create a file to stamp");
        } else {
            fputs("x", f);
            fclose(f);
            struct utimbuf ut;
            ut.actime = 0;
            ut.modtime = 1000000000; /* 2001-09-09, the same instant strftime is checked against */
            if (utime(path, &ut) != 0) {
                fail("utime failed");
            }
            struct stat st;
            if (stat(path, &st) != 0 || st.st_mtime != 1000000000) {
                printf("[libctest] mtime is %ld\n", (long)st.st_mtime);
                fail("utime did not set the modification time");
            }
            /* The whole reason this call exists: a stamped file must not
             * be re-stamped by the next thing that looks at it, because
             * `make` decides what to rebuild by comparing these. */
            if (stat(path, &st) != 0 || st.st_mtime != 1000000000) {
                fail("a stat re-stamped the file it looked at");
            }
            if (unlink(path) != 0) {
                fail("could not remove the stamped file");
            }
        }
    }

    /* ---- M100: getrandom, and /dev/urandom --------------------------------
     *
     * What a program can check about a random source from outside: the
     * call fills what it was asked for, two calls differ, the device
     * file agrees with the syscall in kind (both fill, neither repeats),
     * and a flag that does not exist is EINVAL. Whether the bytes are
     * random is tests/test_random.c's question, on the host, against
     * the RFC. */
    {
        unsigned char a[64], b[64], c[64];
        memset(a, 0, sizeof(a));
        memset(b, 0, sizeof(b));
        if (getrandom(a, sizeof(a), 0) != (ssize_t)sizeof(a) ||
            getrandom(b, sizeof(b), GRND_NONBLOCK) != (ssize_t)sizeof(b)) {
            fail("getrandom did not fill the buffer");
        }
        if (memcmp(a, b, sizeof(a)) == 0) {
            fail("two getrandom calls returned the same bytes");
        }
        int zeros = 0;
        for (size_t i = 0; i < sizeof(a); i++) {
            zeros += a[i] == 0;
        }
        if (zeros > 16) {
            fail("getrandom's bytes are mostly zero");
        }
        errno = 0;
        if (getrandom(c, sizeof(c), 0x100) != -1 || errno != EINVAL) {
            fail("getrandom accepted a flag that does not exist");
        }
        int rfd = open("/dev/urandom", O_RDONLY);
        if (rfd < 0 || read(rfd, c, sizeof(c)) != (long)sizeof(c)) {
            fail("/dev/urandom could not be read");
        } else if (memcmp(c, a, sizeof(c)) == 0 || memcmp(c, b, sizeof(c)) == 0) {
            fail("/dev/urandom repeated getrandom's bytes");
        }
        if (rfd >= 0) {
            close(rfd);
        }
    }

    /* ---- M100: O_NONBLOCK on a pipe ------------------------------------
     *
     * The flag was 0 until M100. What is checked is the whole contract
     * from one thread, which a pipe allows and a socket does not: the
     * bit reads back, an empty non-blocking read is EAGAIN rather than
     * a wait or a 0, a non-blocking write fills the pipe and then says
     * EAGAIN with the count it did take, the reader drains exactly that
     * count, and clearing the bit clears it. */
    {
        int p[2];
        char buf[4096];
        if (pipe(p) != 0) {
            fail("pipe() failed");
        } else {
            if (fcntl(p[0], F_GETFL) & O_NONBLOCK) {
                fail("a fresh pipe reported O_NONBLOCK");
            }
            if (fcntl(p[0], F_SETFL, O_NONBLOCK) != 0 || !(fcntl(p[0], F_GETFL) & O_NONBLOCK)) {
                fail("O_NONBLOCK could not be set on a pipe's read end, or did not read back");
            }
            errno = 0;
            if (read(p[0], buf, 1) != -1 || errno != EAGAIN) {
                fail("a non-blocking read of an empty pipe was not EAGAIN");
            }
            if (write(p[1], "x", 1) != 1 || read(p[0], buf, 1) != 1 || buf[0] != 'x') {
                fail("a byte did not cross a non-blocking pipe");
            }
            if (fcntl(p[1], F_SETFL, O_NONBLOCK) != 0) {
                fail("O_NONBLOCK could not be set on the write end");
            }
            memset(buf, 'y', sizeof(buf));
            long total = 0, n;
            int rounds = 0;
            errno = 0;
            while ((n = write(p[1], buf, sizeof(buf))) > 0 && rounds++ < 1000) {
                total += n;
            }
            if (n != -1 || errno != EAGAIN || total <= 0) {
                printf("[libctest] non-blocking write stopped with %ld after %ld bytes, errno %d\n",
                       n, total, errno);
                fail("a non-blocking write did not fill the pipe and then say EAGAIN");
            }
            long drained = 0;
            while ((n = read(p[0], buf, sizeof(buf))) > 0) {
                drained += n;
            }
            if (drained != total || n != -1 || errno != EAGAIN) {
                printf("[libctest] drained %ld of %ld\n", drained, total);
                fail("the reader did not drain exactly what the non-blocking writes took");
            }
            if (fcntl(p[0], F_SETFL, 0) != 0 || (fcntl(p[0], F_GETFL) & O_NONBLOCK)) {
                fail("clearing O_NONBLOCK did not clear it");
            }
            close(p[0]);
            close(p[1]);
        }
    }

    /* ---- M100: the C99 comparison macros -------------------------------
     *
     * harfbuzz found these by way of libstdc++: its configure names all
     * twelve classification and comparison macros in one probe, and
     * without the six comparisons concluded <math.h> was not C99. What
     * is checked is the property that makes them different from the
     * operators: a NaN compares false to everything, quietly. */
    {
        volatile double nan_v = NAN, one = 1.0, two = 2.0;
        if (!isgreater(two, one) || isgreater(one, two) || isgreater(nan_v, one) ||
            !isgreaterequal(one, one) || !isless(one, two) || isless(nan_v, two) ||
            !islessequal(two, two) || !islessgreater(one, two) || islessgreater(nan_v, one) ||
            !isunordered(nan_v, one) || isunordered(one, two)) {
            fail("a C99 comparison macro answered wrongly, or a NaN compared true");
        }
    }

    /* ---- M100: recursive and errorcheck mutexes ----------------------
     *
     * sqlite found these: its database mutex is PTHREAD_MUTEX_RECURSIVE
     * and it takes it inside itself on every API call. What is checked
     * is the observable contract from one thread - a recursive mutex
     * taken three times needs three unlocks before a trylock from the
     * same thread sees it... still held, since trylock by the owner
     * recurses too; so what says "released" is that pthread_mutex_unlock
     * by the owner of a NORMAL-free mutex is then EPERM - and an
     * ERRORCHECK one refuses its holder with EDEADLK rather than
     * hanging, which is the whole difference between the two. The
     * cross-thread half (a recursive mutex under contention) is in
     * threadtest, where there are threads. */
    {
        pthread_mutexattr_t at;
        pthread_mutex_t rm, em;
        if (pthread_mutexattr_init(&at) != 0 ||
            pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE) != 0) {
            fail("a recursive mutex attribute was refused");
        }
        int t = -1;
        if (pthread_mutexattr_gettype(&at, &t) != 0 || t != PTHREAD_MUTEX_RECURSIVE) {
            fail("mutexattr_gettype did not return the type that was set");
        }
        if (pthread_mutex_init(&rm, &at) != 0) {
            fail("pthread_mutex_init with a recursive attribute failed");
        }
        if (pthread_mutex_lock(&rm) != 0 || pthread_mutex_lock(&rm) != 0 ||
            pthread_mutex_trylock(&rm) != 0) {
            fail("a recursive mutex refused its own holder");
        }
        if (pthread_mutex_unlock(&rm) != 0 || pthread_mutex_unlock(&rm) != 0) {
            fail("a recursive mutex refused an unlock by its holder");
        }
        /* Two of three holds released: still held, so a NEW lock attempt
         * from the owner is a fourth recursion, not a wait - and after
         * the matching unlocks the mutex is free, which is observable
         * because an unlock of a free typed mutex is EPERM. */
        if (pthread_mutex_lock(&rm) != 0) {
            fail("a recursive mutex with one hold left refused its holder");
        }
        if (pthread_mutex_unlock(&rm) != 0 || pthread_mutex_unlock(&rm) != 0) {
            fail("the last two unlocks of a recursive mutex failed");
        }
        if (pthread_mutex_unlock(&rm) != EPERM) {
            fail("unlocking a recursive mutex nobody holds was not EPERM");
        }
        if (pthread_mutex_lock(&rm) != 0 || pthread_mutex_unlock(&rm) != 0) {
            fail("a recursive mutex did not work again after being fully released");
        }
        pthread_mutex_destroy(&rm);

        if (pthread_mutexattr_settype(&at, PTHREAD_MUTEX_ERRORCHECK) != 0 ||
            pthread_mutex_init(&em, &at) != 0) {
            fail("an errorcheck mutex could not be made");
        }
        if (pthread_mutex_lock(&em) != 0) {
            fail("an errorcheck mutex refused its first lock");
        }
        if (pthread_mutex_lock(&em) != EDEADLK) {
            fail("an errorcheck mutex did not answer EDEADLK to its holder");
        }
        if (pthread_mutex_trylock(&em) != EBUSY) {
            fail("trylock of a held errorcheck mutex was not EBUSY");
        }
        if (pthread_mutex_unlock(&em) != 0 || pthread_mutex_unlock(&em) != EPERM) {
            fail("an errorcheck mutex's unlock contract is wrong");
        }
        pthread_mutex_destroy(&em);
        pthread_mutexattr_destroy(&at);

        /* And the type nothing asked for is still refused. */
        if (pthread_mutexattr_settype(&at, 7) != EINVAL) {
            fail("a mutex type that does not exist was accepted");
        }
        /* A NORMAL mutex is what it always was: one word's worth of
         * behaviour, and no owner check on unlock. */
        pthread_mutex_t nm = PTHREAD_MUTEX_INITIALIZER;
        if (pthread_mutex_lock(&nm) != 0 || pthread_mutex_trylock(&nm) != EBUSY ||
            pthread_mutex_unlock(&nm) != 0 || pthread_mutex_trylock(&nm) != 0 ||
            pthread_mutex_unlock(&nm) != 0) {
            fail("a normal mutex changed behaviour");
        }
    }

    /* ---- M100: popen, pclose and system ------------------------------
     *
     * Three functions over one mechanism (/bin/sh -c, a fork and an
     * exec), asked for by sqlite's shell. What is checked is the part a
     * caller depends on and a lazy version gets wrong: the command's
     * OUTPUT arrives through the "r" stream, its INPUT arrives through
     * the "w" one (which means the child saw EOF - a leaked pipe end
     * would hang here rather than fail), and the EXIT STATUS comes back
     * through pclose and system, including the 127 a shell answers for
     * a command it cannot run. */
    {
        char line[64];
        FILE *p = popen("echo popen-read", "r");
        if (!p) {
            fail("popen(\"r\") returned NULL");
        } else {
            memset(line, 0, sizeof(line));
            if (!fgets(line, sizeof(line), p) || strcmp(line, "popen-read\n") != 0) {
                printf("[libctest] popen read %s\n", line);
                fail("popen(\"r\") did not deliver the command's output");
            }
            int st = pclose(p);
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                fail("pclose did not report a clean exit for a command that made one");
            }
        }
        p = popen("exit 7", "r");
        if (!p) {
            fail("popen of a command that exits non-zero returned NULL");
        } else {
            int st = pclose(p);
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 7) {
                printf("[libctest] pclose status %d\n", st);
                fail("pclose lost the command's exit status");
            }
        }
        const char *path = "/tmp_popen_test";
        p = popen("cat > /tmp_popen_test", "w");
        if (!p) {
            fail("popen(\"w\") returned NULL");
        } else {
            fputs("through the pipe\n", p);
            if (pclose(p) != 0) {
                fail("pclose of a \"w\" stream did not report the command finishing");
            }
            FILE *f = fopen(path, "r");
            memset(line, 0, sizeof(line));
            if (!f || !fgets(line, sizeof(line), f) || strcmp(line, "through the pipe\n") != 0) {
                printf("[libctest] the child wrote %s\n", line);
                fail("what was written to a popen(\"w\") stream did not reach the command");
            }
            if (f) {
                fclose(f);
            }
            unlink(path);
        }
        if (pclose(stdin) != -1) {
            fail("pclose accepted a stream popen did not open");
        }
        int st = system("exit 3");
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 3) {
            printf("[libctest] system status %d\n", st);
            fail("system did not return the command's exit status");
        }
        if (system((const char *)0) == 0) {
            fail("system(NULL) denied there is a command processor");
        }
        st = system("/no/such/program");
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 127) {
            printf("[libctest] system status %d for a program that is not there\n", st);
            fail("system did not answer 127 for a command that cannot run");
        }
    }

    if (failures) {
        return 1;
    }
    printf("[libctest] libc subset and SSE verified.\n");
    return 0;
}
