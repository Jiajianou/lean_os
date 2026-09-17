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
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/random.h>
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
    volatile double a = 3.0, b = 7.0;
    if (a / b * b != 3.0 && fabs(a / b * b - 3.0) > 1e-15) {
        fail("double division does not round-trip");
    }

    near("sqrt(2)", sqrt(2.0), 1.4142135623730951);
    near("sin(pi/6)", sin(M_PI / 6.0), 0.5);
    near("cos(pi/3)", cos(M_PI / 3.0), 0.5);
    near("sin(0)", sin(0.0), 0.0);
    near("sin(1000)", sin(1000.0), 0.8268795405320025);
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

    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%d %5d %-5d| %05d", 42, 42, 42, 42);
    same("integer widths", buffer, "42    42 42   | 00042");
    snprintf(buffer, sizeof(buffer), "%x %X %o %u", 255u, 255u, 8u, 4000000000u);
    same("bases", buffer, "ff FF 10 4000000000");
    snprintf(buffer, sizeof(buffer), "%s|%8s|%-8s|%.3s", "ab", "ab", "ab", "abcdef");
    same("string widths", buffer, "ab|      ab|ab      |abc");
    snprintf(buffer, sizeof(buffer), "%.2f %.0f %.4f", 3.14159, 2.5, -0.00005);
    same("fixed point", buffer, "3.14 2 -0.0001");
    snprintf(buffer, sizeof(buffer), "%e %.2e %12.4e", 1234.5, 0.000271828, 1234.5);
    same("scientific", buffer, "1.234500e+03 2.72e-04   1.2345e+03");
    snprintf(buffer, sizeof(buffer), "%ld %c %%", 1234567890L, 'z');
    same("long and char", buffer, "1234567890 z %");
    if (snprintf(buffer, 4, "abcdef") != 6 || strcmp(buffer, "abc") != 0) {
        fail("snprintf truncation");
    }

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

    if (time((time_t *)0) <= 0) {
        fail("time() has no clock");
    }

    {
        double sum = 0.0;
        for (int i = 1; i <= 200000; i++) {
            sum += 1.0 / ((double)i * (double)i);
        }
        double want = M_PI * M_PI / 6.0;
        if (fabs(sum - want) > 1e-5) {
            printf("[libctest] FAIL: a float loop across scheduler quanta lost state: %.12f vs %.12f\n",
                   sum, want);
            failures++;
        }
    }

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
            longjmp(env, 0);
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
        }
        const char *none = "zzz";
        if (strtoul(none, &end, 10) != 0 || end != none) {
            fail("strtoul: no-digits must hand back the original pointer");
        }
        if (strtoll("-9000000000", &end, 10) != -9000000000LL) {
            fail("strtoll: a value past 32 bits");
        }
    }

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

    {
        struct tm tm;
        time_t t = 1000000000;
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
        char buffer[64];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S %a", &tm);
        same("strftime", buffer, "2001-09-09 01:46:40 Sun");
        strftime(buffer, sizeof(buffer), "%F %T", &tm);
        same("strftime %F %T", buffer, "2001-09-09 01:46:40");
    }

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
        wchar_t big[2] = {0x1F600, 0};
        if (wcstombs(back, big, 16) != 4) {
            fail("wcstombs did not encode a four-byte character");
        }
        wchar_t round[4];
        if (mbstowcs(round, back, 4) != 1 || round[0] != (wchar_t)0x1F600) {
            fail("a four-byte character did not round-trip");
        }
        if (mbstowcs(round, "\xC0\xAF", 4) != (size_t)-1) {
            fail("mbstowcs accepted an overlong encoding");
        }
        if (MB_CUR_MAX != 4) {
            fail("MB_CUR_MAX is not 4");
        }
    }

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
        if (strcmp(nl_langinfo(CODESET), "UTF-8") != 0) {
            fail("nl_langinfo(CODESET) is not UTF-8");
        }
        if (strcmp(setlocale(LC_ALL, "C.UTF-8"), "C") != 0) {
            fail("setlocale(\"C.UTF-8\")");
        }
    }

    {
        struct timespec a, b;
        if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) {
            fail("clock_gettime(CLOCK_MONOTONIC)");
        }
        for (volatile int i = 0; i < 200000; i++) {
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

        struct timespec m0, m1;
        struct timeval w0, w1;
        clock_gettime(CLOCK_MONOTONIC, &m0);
        gettimeofday(&w0, 0);
        long long previous = (long long)w0.tv_sec * 1000000 + w0.tv_usec;
        long long step_back = 0;
        for (;;) {
            gettimeofday(&w1, 0);
            long long now = (long long)w1.tv_sec * 1000000 + w1.tv_usec;
            if (now < previous && previous - now > step_back) {
                step_back = previous - now;
            }
            previous = now;
            clock_gettime(CLOCK_MONOTONIC, &m1);
            long long mono_ms = ((long long)m1.tv_sec - m0.tv_sec) * 1000 +
                                (m1.tv_nsec - m0.tv_nsec) / 1000000;
            if (mono_ms >= 1200) {
                break;
            }
        }
        if (step_back) {
            printf("libctest: gettimeofday went back %lld us\n", step_back);
            fail("gettimeofday went backwards");
        }
        long long mono_ms = ((long long)m1.tv_sec - m0.tv_sec) * 1000 +
                            (m1.tv_nsec - m0.tv_nsec) / 1000000;
        long long wall_ms = ((long long)w1.tv_sec - w0.tv_sec) * 1000 +
                            (w1.tv_usec - w0.tv_usec) / 1000;
        if (wall_ms < mono_ms - 20 || wall_ms > mono_ms + 1000) {
            printf("libctest: %lld ms of the monotonic clock was %lld ms of the time of day\n",
                   mono_ms, wall_ms);
            fail("gettimeofday does not advance with the monotonic clock");
        }
    }

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
        if (mmap(0, 4096, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, 3, 0) != MAP_FAILED) {
            fail("mmap accepted a file descriptor it cannot honour");
        }
    }

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

    {
        int pf[2];
        if (pipe(pf) != 0) {
            fail("pipe() for the poll test");
        } else {
            struct pollfd p;
            p.fd = pf[0];
            p.events = POLLIN;
            p.revents = 0;

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

            char got = 0;
            if (read(pf[0], &got, 1) != 1 || got != 'x') {
                fail("poll: the byte did not read back");
            }
            p.revents = 0;
            if (poll(&p, 1, 30) != 0 || p.revents != 0) {
                fail("poll: a drained pipe still reported ready");
            }

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

            tv.tv_sec = 0;
            tv.tv_usec = 20000;
            if (select(0, 0, 0, 0, &tv) != 0) {
                fail("select: an empty set with a timeout was not a sleep");
            }

            close(sf[0]);
            close(sf[1]);
        }
    }

    {
        if (strcasecmp("AbC", "aBc") != 0) {
            fail("strcasecmp is not visible from <string.h>, or is wrong");
        }
        if (strncasecmp("AbCxx", "aBcyy", 3) != 0) {
            fail("strncasecmp is not visible from <string.h>, or is wrong");
        }
        if (ffs(0) != 0 || ffs(1) != 1 || ffs(8) != 4) {
            fail("ffs is not visible from <string.h>, or is wrong");
        }

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

        if (EXIT_SUCCESS != 0 || EXIT_FAILURE == 0) {
            fail("EXIT_SUCCESS/EXIT_FAILURE are not 0 and non-zero");
        }

        {
            char tmpl[] = "/tmp/libctest-mkXXXXXX";
            char *got = mktemp(tmpl);
            if (got != tmpl || tmpl[0] == '\0') {
                fail("mktemp did not produce a name");
            }
            if (strncmp(tmpl, "/tmp/libctest-mk", 16) != 0) {
                fail("mktemp overwrote the part of the template it must keep");
            }
            int fd = open(tmpl, O_RDONLY);
            if (fd >= 0) {
                close(fd);
                fail("mktemp returned a name that already existed");
            }
        }

        /* mkstemps, which is mkstemp with a suffix the template keeps.
           M159: ANGLE's system_utils_posix.cpp is the caller that asked for
           it. What separates it from mkstemp is where the six X's are, so
           the check is on the SUFFIX surviving and on the X's not: a
           mkstemps that ignored the suffix length would replace the ".png"
           and still hand back a working descriptor. */
        {
            static const char pattern[] = "/tmp/libctest-msXXXXXX.png";
            char first[sizeof(pattern)];
            char second[sizeof(pattern)];
            strcpy(first, pattern);
            strcpy(second, pattern);

            int fd = mkstemps(first, 4);
            if (fd < 0) {
                fail("mkstemps did not create a file");
            } else {
                close(fd);
                if (strcmp(first + strlen(first) - 4, ".png") != 0) {
                    printf("libctest: mkstemps produced \"%s\"\n", first);
                    fail("mkstemps overwrote the suffix it was told to keep");
                }
                if (strncmp(first, "/tmp/libctest-ms", 16) != 0) {
                    fail("mkstemps overwrote the prefix");
                }
                int again = open(first, O_RDONLY);
                if (again < 0) {
                    fail("the file mkstemps says it created is not there");
                } else {
                    close(again);
                }

                /* A second call, with the first file still on the disk.
                   This is the check that found a bug older than any Chromium
                   work here: the retry loop could not retry. Every one of
                   these four calls fills the template and, on the next
                   attempt, VALIDATED it again - against six X's the previous
                   attempt had already replaced with letters - so a collision
                   produced EINVAL rather than a second name. Reaching it
                   needs the same process, the same millisecond and the file
                   already there, which is why a hundred and fifty milestones
                   did not.

                   The two names differing is also the only honest way to ask
                   whether the X's were replaced at all: the alphabet they are
                   drawn from CONTAINS 'X', so a name with an X in it is a
                   perfectly good answer and "strchr(name, 'X') == 0" tests
                   nothing. This test asserted that first and was right to
                   fail on /tmp/libctest-ms7XUt80.png. */
                int other = mkstemps(second, 4);
                if (other < 0) {
                    fail("mkstemps refused a second file");
                } else {
                    close(other);
                    if (strcmp(first, second) == 0) {
                        printf("libctest: mkstemps produced \"%s\" twice\n",
                               first);
                        fail("mkstemps handed out a name that already existed");
                    }
                    unlink(second);
                }
                unlink(first);
            }

            /* A template with no room for six X's before the suffix is not a
               template, and saying so is the difference between a refusal
               and a file with a name nobody asked for. */
            char narrow[] = "/tmp/XXX.png";
            if (mkstemps(narrow, 4) >= 0) {
                fail("mkstemps accepted a template with only three X's");
            }
            char negative[] = "/tmp/libctest-msXXXXXX";
            if (mkstemps(negative, -1) >= 0) {
                fail("mkstemps accepted a negative suffix length");
            }
        }
    }

    /* M160. The four calls //cc's graph asked this libc for, and the two of
       them whose honest answer is "no" are graded on saying so rather than on
       succeeding. */
    {
        unsigned int a = 12345;
        unsigned int b = 12345;
        int first = rand_r(&a);
        int second = rand_r(&b);
        if (first != second || a != b) {
            fail("rand_r is not a function of the state it was handed");
        }
        int third = rand_r(&a);
        if (third == first) {
            fail("rand_r did not advance the state it was handed");
        }
        srand(12345);
        if (rand() != first) {
            fail("rand_r and rand are not the same generator");
        }
        if (rand_r((unsigned int *)0) != 0 || errno != EINVAL) {
            fail("rand_r accepted a null state");
        }

        /* flock: there is no file locking in this kernel and an flock that
           returned 0 would claim exclusive access to a file anybody can
           open. M65's rule - a truthful failure, not a no-op. */
        int fd = open("/tmp/libctest-flock", O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            fail("could not make a file to not-lock");
        } else {
            if (flock(fd, LOCK_EX) != -1 || errno != ENOSYS) {
                fail("flock claimed a lock this kernel cannot hold");
            }
            close(fd);
            unlink("/tmp/libctest-flock");
        }
        if (flock(-1, LOCK_EX) != -1 || errno != EBADF) {
            fail("flock did not refuse a bad descriptor as a bad descriptor");
        }

        /* mlock: the promise is residency, and this kernel keeps it. The
           check is that it is kept for memory that has never been touched -
           which is where an mlock that returned 0 and did nothing would be
           lying - and that an unmapped range is refused. */
        size_t span = 64 * 4096;
        void *pages = mmap(0, span, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pages == MAP_FAILED) {
            fail("could not map pages to lock");
        } else {
            if (mlock(pages, span) != 0) {
                fail("mlock refused a range this process had just mapped");
            }
            unsigned char resident[64];
            if (mincore(pages, span, resident) != 0) {
                fail("mincore refused a range mlock had just accepted");
            } else {
                int missing = 0;
                for (int i = 0; i < 64; i++) {
                    if (!(resident[i] & 1)) {
                        missing++;
                    }
                }
                if (missing != 0) {
                    printf("libctest: %d of 64 pages are not resident after "
                           "mlock\n", missing);
                    fail("mlock returned 0 for pages that are not there");
                }
            }
            if (munlock(pages, span) != 0) {
                fail("munlock refused what mlock accepted");
            }
            munmap(pages, span);
            /* And the distinction this found in the kernel (M160): mincore
               used to answer "not resident" for a range nobody had mapped,
               because it read the page table and an untouched page of a live
               mapping is absent from it for the same reason. They are
               different answers, and this is the one that matters - it is
               how a caller asks "may I touch this" without finding out by
               faulting. mlock is what needed it, and got a page fault
               instead. */
            unsigned char after[64];
            if (mincore(pages, span, after) != -1 || errno != ENOMEM) {
                fail("mincore answered for a range that had been unmapped");
            }
            if (mlock(pages, span) != -1 || errno != ENOMEM) {
                fail("mlock accepted a range that had been unmapped");
            }
        }
    }

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
        long phys = sysconf(_SC_PHYS_PAGES);
        long avail = sysconf(_SC_AVPHYS_PAGES);
        if (phys <= 0 || avail < 0 || avail > phys) {
            fail("sysconf's memory answers are not a machine's memory");
        }
        if (sysconf(_SC_THREADS) <= 0 || sysconf(_SC_JOB_CONTROL) <= 0) {
            fail("sysconf denies an option this machine implements");
        }
        if (sysconf(_SC_MESSAGE_PASSING) != -1 || sysconf(_SC_TIMERS) != -1) {
            fail("sysconf claims an option this machine does not have");
        }
        if (sysconf(-12345) != -1) {
            fail("sysconf accepted a name it does not know");
        }
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

    {
        struct sigaction sa, old;
        sa.sa_handler = usr_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGUSR1, &sa, &old) != 0) {
            fail("sigaction could not install a handler");
        }
        raise(SIGUSR1);
        (void)getpid();
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

    {
        struct tms t0, t1;
        clock_t r0 = times(&t0);
        if (r0 == (clock_t)-1) {
            fail("times() failed");
        }
        volatile unsigned long spin = 0;
        for (unsigned long i = 0; i < 40000000UL; i++) {
            spin += i;
        }
        (void)spin;
        clock_t r1 = times(&t1);
        if (t1.tms_utime <= t0.tms_utime) {
            printf("[libctest] utime %ld -> %ld\n", (long)t0.tms_utime, (long)t1.tms_utime);
            fail("times() did not charge a busy loop to user time");
        }
        if (r1 < r0) {
            fail("times() elapsed clock went backwards");
        }
        if (t1.tms_cutime != 0 || t1.tms_cstime != 0) {
            fail("times() reported child time in a process with no children");
        }

        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) != 0) {
            fail("getrusage(RUSAGE_SELF)");
        }
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
        if (clock() == (clock_t)-1) {
            fail("clock() failed");
        }

        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur != (rlim_t)sysconf(_SC_OPEN_MAX)) {
            fail("getrlimit(RLIMIT_NOFILE) does not agree with sysconf");
        }
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            fail("setrlimit refused a request to change nothing");
        }
        rl.rlim_cur = 8;
        if (setrlimit(RLIMIT_NOFILE, &rl) != -1 || errno != EPERM) {
            fail("setrlimit agreed to enforce a limit nothing here enforces");
        }
    }

    {
        struct statvfs virtual_file_system;
        if (statvfs("/", &virtual_file_system) != 0) {
            fail("statvfs(\"/\")");
        }
        if (virtual_file_system.f_bsize == 0 || virtual_file_system.f_blocks == 0 || virtual_file_system.f_bfree > virtual_file_system.f_blocks) {
            fail("statvfs reported an impossible filesystem");
        }
        if (virtual_file_system.f_files == 0 || virtual_file_system.f_ffree > virtual_file_system.f_files) {
            fail("statvfs reported an impossible inode count");
        }
        if (virtual_file_system.f_namemax == 0) {
            fail("statvfs did not report a name limit");
        }
        if (statvfs("/no/such/path", &virtual_file_system) != -1) {
            fail("statvfs answered about a path that does not exist");
        }
        if (statvfs("/proc", &virtual_file_system) != -1) {
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
            ut.modtime = 1000000000;
            if (utime(path, &ut) != 0) {
                fail("utime failed");
            }
            struct stat st;
            if (stat(path, &st) != 0 || st.st_mtime != 1000000000) {
                printf("[libctest] mtime is %ld\n", (long)st.st_mtime);
                fail("utime did not set the modification time");
            }
            if (stat(path, &st) != 0 || st.st_mtime != 1000000000) {
                fail("a stat re-stamped the file it looked at");
            }
            if (unlink(path) != 0) {
                fail("could not remove the stamped file");
            }
        }
    }

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

    {
        int p[2];
        char buffer[4096];
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
            if (read(p[0], buffer, 1) != -1 || errno != EAGAIN) {
                fail("a non-blocking read of an empty pipe was not EAGAIN");
            }
            if (write(p[1], "x", 1) != 1 || read(p[0], buffer, 1) != 1 || buffer[0] != 'x') {
                fail("a byte did not cross a non-blocking pipe");
            }
            if (fcntl(p[1], F_SETFL, O_NONBLOCK) != 0) {
                fail("O_NONBLOCK could not be set on the write end");
            }
            memset(buffer, 'y', sizeof(buffer));
            long total = 0, n;
            int rounds = 0;
            errno = 0;
            while ((n = write(p[1], buffer, sizeof(buffer))) > 0 && rounds++ < 1000) {
                total += n;
            }
            if (n != -1 || errno != EAGAIN || total <= 0) {
                printf("[libctest] non-blocking write stopped with %ld after %ld bytes, errno %d\n",
                       n, total, errno);
                fail("a non-blocking write did not fill the pipe and then say EAGAIN");
            }
            long drained = 0;
            while ((n = read(p[0], buffer, sizeof(buffer))) > 0) {
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

    {
        volatile double nan_v = NAN, one = 1.0, two = 2.0;
        if (!isgreater(two, one) || isgreater(one, two) || isgreater(nan_v, one) ||
            !isgreaterequal(one, one) || !isless(one, two) || isless(nan_v, two) ||
            !islessequal(two, two) || !islessgreater(one, two) || islessgreater(nan_v, one) ||
            !isunordered(nan_v, one) || isunordered(one, two)) {
            fail("a C99 comparison macro answered wrongly, or a NaN compared true");
        }
    }

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

        if (pthread_mutexattr_settype(&at, 7) != EINVAL) {
            fail("a mutex type that does not exist was accepted");
        }
        pthread_mutex_t nm = PTHREAD_MUTEX_INITIALIZER;
        if (pthread_mutex_lock(&nm) != 0 || pthread_mutex_trylock(&nm) != EBUSY ||
            pthread_mutex_unlock(&nm) != 0 || pthread_mutex_trylock(&nm) != 0 ||
            pthread_mutex_unlock(&nm) != 0) {
            fail("a normal mutex changed behaviour");
        }
    }

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
