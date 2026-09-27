#include <elf.h>
#include <errno.h>
#include <fcntl.h>

#if !defined(S_IRUSR) || !defined(S_IWUSR) || !defined(S_IRGRP) || !defined(S_IROTH)
#error "fcntl.h has to define the mode bits open(2)'s third argument is made of"
#endif

#ifndef ENONET
#error "errno.h has to name ENONET - somebody else's error table does"
#endif
#include <limits.h>
#include <link.h>
#include <malloc.h>
#include <stdint.h>
#include <process.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "paths.h"

#if !defined(_POSIX_MONOTONIC_CLOCK) || _POSIX_MONOTONIC_CLOCK < 0
#error "unistd.h has to advertise the monotonic clock this libc implements"
#endif

#ifndef __THROW
#error "sys/cdefs.h has to define __THROW"
#endif

#ifndef __BEGIN_DECLS
#error "sys/cdefs.h has to define __BEGIN_DECLS"
#endif

static char order[64];
static int order_at;

static void note(char c) {
    if (order_at < (int)sizeof(order) - 1) {
        order[order_at++] = c;
    }
}

static void prepare_one(void) { note('1'); }
static void prepare_two(void) { note('2'); }
static void prepare_three(void) { note('3'); }
static void parent_one(void) { note('a'); }
static void parent_two(void) { note('b'); }
static void parent_three(void) { note('c'); }
static void child_one(void) { note('A'); }
static void child_two(void) { note('B'); }
static void child_three(void) { note('C'); }

static int monotonic_clock_moves_forward(void) {
    struct timespec first;
    struct timespec second;
    if (clock_gettime(CLOCK_MONOTONIC, &first) != 0) {
        return 2;
    }
    struct timespec nap = {0, 20 * 1000 * 1000};
    if (nanosleep(&nap, 0) != 0) {
        return 3;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &second) != 0) {
        return 4;
    }
    long long moved = (long long)(second.tv_sec - first.tv_sec) * 1000000000ll +
                      ((long long)second.tv_nsec - (long long)first.tv_nsec);
    if (moved <= 0) {
        return 5;
    }
    return 0;
}

static int usable_size_covers_the_request(void) {
    void *p = malloc(100);
    if (!p) {
        return 6;
    }
    size_t usable = malloc_usable_size(p);
    memset(p, 0xa5, usable);
    free(p);
    if (usable < 100) {
        return 7;
    }
    return 0;
}

static int this_program_parses_as_elf(void) {
    if (sizeof(ElfW(Ehdr)) != 64) {
        return 8;
    }
    if (sizeof(ElfW(Phdr)) != 56) {
        return 9;
    }
    int fd = open(PATH_BIN_DIRECTORY "posixtest", O_RDONLY);
    if (fd < 0) {
        return 10;
    }
    ElfW(Ehdr) header;
    if (read(fd, &header, sizeof(header)) != (long)sizeof(header)) {
        close(fd);
        return 11;
    }
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0) {
        close(fd);
        return 12;
    }
    if (header.e_ident[EI_CLASS] != ELFCLASS64) {
        close(fd);
        return 13;
    }
    if (header.e_ident[EI_DATA] != ELFDATA2LSB) {
        close(fd);
        return 14;
    }
    if (header.e_machine != EM_X86_64) {
        close(fd);
        return 15;
    }
    if (header.e_type != ET_EXEC && header.e_type != ET_DYN) {
        close(fd);
        return 16;
    }
    if (header.e_ehsize != sizeof(ElfW(Ehdr))) {
        close(fd);
        return 17;
    }
    if (header.e_phentsize != sizeof(ElfW(Phdr))) {
        close(fd);
        return 18;
    }
    if (header.e_phnum == 0) {
        close(fd);
        return 19;
    }
    if (lseek(fd, (long)header.e_phoff, SEEK_SET) < 0) {
        close(fd);
        return 20;
    }
    int loads = 0;
    int entry_is_in_an_executable_segment = 0;
    for (unsigned i = 0; i < header.e_phnum; i++) {
        ElfW(Phdr) segment;
        if (read(fd, &segment, sizeof(segment)) != (long)sizeof(segment)) {
            close(fd);
            return 21;
        }
        if (segment.p_type != PT_LOAD) {
            continue;
        }
        loads++;
        if (segment.p_filesz > segment.p_memsz) {
            close(fd);
            return 22;
        }
        if ((segment.p_flags & PF_X) && header.e_entry >= segment.p_vaddr &&
            header.e_entry < segment.p_vaddr + segment.p_memsz) {
            entry_is_in_an_executable_segment = 1;
        }
    }
    close(fd);
    if (loads == 0) {
        return 23;
    }
    if (!entry_is_in_an_executable_segment) {
        return 24;
    }
    return 0;
}

static int atfork_handlers_run_in_the_documented_order(void) {
    int channel[2];
    if (pipe(channel) != 0) {
        return 25;
    }
    if (pthread_atfork(prepare_one, parent_one, child_one) != 0) {
        return 26;
    }
    if (pthread_atfork(prepare_two, parent_two, child_two) != 0) {
        return 27;
    }
    if (pthread_atfork(prepare_three, parent_three, child_three) != 0) {
        return 28;
    }
    order_at = 0;
    pid_t child = fork();
    if (child < 0) {
        return 29;
    }
    if (child == 0) {
        close(channel[0]);
        order[order_at] = '\0';
        write(channel[1], order, strlen(order) + 1);
        close(channel[1]);
        _exit(0);
    }
    close(channel[1]);
    char seen[64];
    long got = read(channel[0], seen, sizeof(seen) - 1);
    close(channel[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (got <= 0) {
        return 30;
    }
    seen[got] = '\0';
    order[order_at] = '\0';
    if (strcmp(order, "321abc") != 0) {
        printf("[posixtest] the parent saw %s, not 321abc\n", order);
        return 31;
    }
    if (strcmp(seen, "321ABC") != 0) {
        printf("[posixtest] the child saw %s, not 321ABC\n", seen);
        return 32;
    }
    return 0;
}

static void *stack_probe(void *argument) {
    (void)argument;
    int local = 0;
    pthread_attr_t attribute;
    if (pthread_getattr_np(pthread_self(), &attribute) != 0) {
        return (void *)1;
    }
    void *base = 0;
    size_t size = 0;
    if (pthread_attr_getstack(&attribute, &base, &size) != 0) {
        return (void *)2;
    }
    pthread_attr_destroy(&attribute);
    unsigned char *low = (unsigned char *)base;
    unsigned char *high = low + size;
    if ((unsigned char *)&local < low || (unsigned char *)&local >= high) {
        return (void *)3;
    }
    if (size < 32u * 1024u) {
        return (void *)4;
    }
    return 0;
}

static int a_thread_can_find_its_own_stack(void) {
    pthread_attr_t request;
    if (pthread_attr_init(&request) != 0) {
        return 33;
    }
    void *base = 0;
    size_t size = 0;
    if (pthread_attr_getstack(&request, &base, &size) == 0) {
        return 34;
    }
    if (pthread_attr_setstacksize(&request, 128u * 1024u) != 0) {
        return 35;
    }
    pthread_t thread;
    if (pthread_create(&thread, &request, stack_probe, 0) != 0) {
        return 36;
    }
    pthread_attr_destroy(&request);
    void *result = (void *)9;
    if (pthread_join(thread, &result) != 0) {
        return 37;
    }
    if (result != 0) {
        printf("[posixtest] the thread's stack probe returned %ld\n",
               (long)(size_t)result);
        return 38;
    }

    /* The MAIN thread, which this thread library did not create and therefore
       has no record of. Until M155 that made it unanswerable and this check
       tolerated a failure - which is why nothing noticed that the answer was
       missing until V8 asked for it and called not knowing fatal.
       Tolerating it was the bug: a program that scans its own stack has no
       second way to find out where it is.

       The bounds come from system_api/include/process.h, which the kernel
       derives its own from, so this also says the two agree. */
    int local = 0;
    pthread_attr_t mine;
    if (pthread_getattr_np(pthread_self(), &mine) != 0) {
        return 39;
    }
    void *main_base = 0;
    size_t main_size = 0;
    if (pthread_attr_getstack(&mine, &main_base, &main_size) != 0) {
        return 40;
    }
    unsigned char *low = (unsigned char *)main_base;
    if ((unsigned char *)&local < low ||
        (unsigned char *)&local >= low + main_size) {
        printf("[posixtest] a local at %p is outside the main stack "
               "%p +%lu\n", (void *)&local, main_base,
               (unsigned long)main_size);
        return 41;
    }
    if ((unsigned long long)(uintptr_t)low + main_size != OS_MAIN_STACK_TOP ||
        main_size != OS_MAIN_STACK_MAX_BYTES) {
        printf("[posixtest] the main stack is %p +%lu, the ABI says "
               "%llx +%llu\n", main_base, (unsigned long)main_size,
               (unsigned long long)OS_MAIN_STACK_TOP,
               (unsigned long long)OS_MAIN_STACK_MAX_BYTES);
        return 42;
    }
    return 0;
}

/* A thread's name, which is the one piece of the POSIX-ish surface here
   whose answer lives in the KERNEL rather than in this library. ANGLE's
   system_utils_linux.cpp is what asked for it - M159 - and the temptation
   was to keep the name in the pthread registry and hand it back, which would
   have passed every test a round trip can write and told nothing else on the
   machine which thread it was looking at.

   So what is graded here is that it went somewhere: the name set on one
   thread does not follow another, the name survives the call that reads it
   back, and a buffer too small to hold it is refused rather than filled with
   a truncation. */
static char other_thread_name[32];
static int other_thread_result;

static void *name_a_second_thread(void *argument) {
    (void)argument;
    if (pthread_setname_np(pthread_self(), "second") != 0) {
        other_thread_result = 1;
        return 0;
    }
    if (pthread_getname_np(pthread_self(), other_thread_name,
                           sizeof(other_thread_name)) != 0) {
        other_thread_result = 2;
    }
    return 0;
}

static int a_thread_can_name_itself(void) {
    char name[32];
    if (pthread_setname_np(pthread_self(), "posixtest-main") != 0) {
        printf("[posixtest] pthread_setname_np refused a 14-character name\n");
        return 43;
    }
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        printf("[posixtest] pthread_getname_np could not read the name back\n");
        return 44;
    }
    if (strcmp(name, "posixtest-main") != 0) {
        printf("[posixtest] the name came back as \"%s\"\n", name);
        return 45;
    }

    /* Too small to hold it. Linux answers ERANGE here rather than truncating
       and this does too, because a caller handed "posixtest-mai" would
       believe that is what the thread is called. */
    char narrow[4];
    if (pthread_getname_np(pthread_self(), narrow, sizeof(narrow)) != ERANGE) {
        printf("[posixtest] a 4-byte buffer was not refused\n");
        return 46;
    }
    /* And a name longer than the kernel keeps. TASK_NAME_MAX is 24. */
    if (pthread_setname_np(pthread_self(),
                           "a-name-far-longer-than-the-kernel-keeps") !=
        ERANGE) {
        printf("[posixtest] an over-long name was accepted\n");
        return 47;
    }
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0 ||
        strcmp(name, "posixtest-main") != 0) {
        printf("[posixtest] a refused name changed the thread's name anyway\n");
        return 48;
    }

    pthread_t other;
    if (pthread_create(&other, 0, name_a_second_thread, 0) != 0) {
        printf("[posixtest] could not start a second thread\n");
        return 49;
    }
    pthread_join(other, 0);
    if (other_thread_result != 0) {
        printf("[posixtest] the second thread could not name itself (%d)\n",
               other_thread_result);
        return 50;
    }
    if (strcmp(other_thread_name, "second") != 0) {
        printf("[posixtest] the second thread read back \"%s\"\n",
               other_thread_name);
        return 51;
    }
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0 ||
        strcmp(name, "posixtest-main") != 0) {
        printf("[posixtest] naming a second thread renamed this one to "
               "\"%s\"\n", name);
        return 52;
    }
    return 0;
}

#define EXEC_PROBE_REACHED 42

static int exec_probe(int argc, char **argv) {
    if (argc != 4 || strcmp(argv[0], "posixtest") != 0 ||
        strcmp(argv[3], "last") != 0) {
        return 1;
    }
    if (strcmp(argv[2], "execle") == 0) {
        const char *value = getenv("POSIXTEST_PROBE");
        if (!value || strcmp(value, "given") != 0) {
            return 2;
        }
    }
    return EXEC_PROBE_REACHED;
}

static int one_list_exec(const char *form) {
    pid_t child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        char *const environment[] = {"POSIXTEST_PROBE=given", 0};
        if (strcmp(form, "execl") == 0) {
            execl(PATH_BIN_DIRECTORY "posixtest", "posixtest", "--exec-probe",
                  form, "last", (char *)0);
        } else if (strcmp(form, "execlp") == 0) {
            execlp("posixtest", "posixtest", "--exec-probe", form, "last",
                   (char *)0);
        } else {
            execle(PATH_BIN_DIRECTORY "posixtest", "posixtest", "--exec-probe",
                   form, "last", (char *)0, environment);
        }
        _exit(97);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

static int the_list_forms_of_exec_reach_the_program(void) {
    const char *forms[] = {"execl", "execlp", "execle"};
    for (int i = 0; i < 3; i++) {
        int got = one_list_exec(forms[i]);
        if (got != EXEC_PROBE_REACHED) {
            printf("[posixtest] %s came back %d, not the probe's %d\n",
                   forms[i], got, EXEC_PROBE_REACHED);
            return 53 + i;
        }
    }
    return 0;
}

/* The question Chromium's ProcessSingleton asks before it trusts a directory
   with its socket - "is this private to its owner", which is exactly 0700 -
   and the rest of what a single-principal machine's mode bits should say:
   the owner may do anything, a link's bits are 0777, and chmod grants the
   one request that is already true and refuses the rest (M187). */
static int mode_bits_say_the_owner_may_do_anything(void) {
    char directory[] = "/tmp/posixtest-XXXXXX";
    if (!mkdtemp(directory)) {
        return 60;
    }
    struct stat st;
    if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode) ||
        (st.st_mode & 07777) != 0700) {
        printf("[posixtest] a directory mkdtemp made reports mode %o, not 0700\n",
               (unsigned)(st.st_mode & 07777));
        return 61;
    }
    char file[64];
    snprintf(file, sizeof(file), "%s/file", directory);
    int fd = open(file, O_CREAT | O_WRONLY, 0600);
    if (fd < 0) {
        return 62;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & 07777) != 0700) {
        printf("[posixtest] a file reports mode %o, not 0700\n",
               (unsigned)(st.st_mode & 07777));
        return 63;
    }
    if (fchmod(fd, 0700) != 0 || chmod(file, 0700) != 0) {
        return 64;
    }
    errno = 0;
    if (chmod(file, 0644) != -1 || errno != EPERM) {
        printf("[posixtest] chmod to a mode nothing here would enforce was "
               "not refused\n");
        return 65;
    }
    errno = 0;
    if (chmod("/tmp/posixtest-no-such-file", 0700) != -1 || errno != ENOENT) {
        return 66;
    }
    close(fd);
    unlink(file);
    rmdir(directory);
    return 0;
}

/* dup and F_DUPFD past the first 128 descriptors, which is where the library
   used to stop looking, and F_DUPFD's "lowest at or above" taken literally
   (M187: Chromium's browser process holds more than 128, and every dup of a
   shared memory region there failed, so each child it launched was handed a
   region with no token). */
static int descriptors_duplicate_past_128(void) {
    int fd = open("/tmp/posixtest-dup", O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        return 70;
    }
    static int copies[300];
    int made = 0;
    int code = 0;
    for (; made < 300; made++) {
        copies[made] = dup(fd);
        if (copies[made] < 0) {
            printf("[posixtest] dup ran out after %d copies\n", made);
            code = 71;
            break;
        }
    }
    if (!code) {
        int high = fcntl(fd, F_DUPFD, 500);
        if (high != 500) {
            printf("[posixtest] F_DUPFD 500 returned %d\n", high);
            code = 72;
        } else {
            close(high);
        }
    }
    if (!code) {
        int cloexec = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (cloexec < 0 || fcntl(cloexec, F_GETFD) != FD_CLOEXEC ||
            fcntl(copies[0], F_GETFD) != 0) {
            code = 73;
        }
        if (cloexec >= 0) {
            close(cloexec);
        }
    }
    for (int i = 0; i < made; i++) {
        close(copies[i]);
    }
    close(fd);
    unlink("/tmp/posixtest-dup");
    if (!code && sysconf(_SC_OPEN_MAX) != OPEN_MAX) {
        code = 74;
    }
    return code;
}

/* An anonymous mapping does not look at its descriptor, and a page can be
   mapped over the middle of an existing region - GWP-ASan does both at once,
   with fd 0, every time it frees a sampled allocation (M187). */
static int anonymous_mappings_ignore_the_descriptor(void) {
    char *region = (char *)mmap(0, 4 * 4096, PROT_READ | PROT_WRITE,
                                MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
    if (region == MAP_FAILED) {
        return 80;
    }
    region[0] = 'a';
    region[2 * 4096] = 'c';
    void *over = mmap(region + 4096, 4096, PROT_NONE,
                      MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
    if (over != region + 4096) {
        return 81;
    }
    if (region[0] != 'a' || region[2 * 4096] != 'c') {
        return 82;
    }
    munmap(region, 4 * 4096);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--exec-probe") == 0) {
        return exec_probe(argc, argv);
    }
    int code = monotonic_clock_moves_forward();
    if (code) {
        return code;
    }
    code = usable_size_covers_the_request();
    if (code) {
        return code;
    }
    code = this_program_parses_as_elf();
    if (code) {
        return code;
    }
    code = atfork_handlers_run_in_the_documented_order();
    if (code) {
        return code;
    }
    code = a_thread_can_find_its_own_stack();
    if (code) {
        return code;
    }
    code = a_thread_can_name_itself();
    if (code) {
        return code;
    }
    code = the_list_forms_of_exec_reach_the_program();
    if (code) {
        return code;
    }
    code = mode_bits_say_the_owner_may_do_anything();
    if (code) {
        return code;
    }
    code = descriptors_duplicate_past_128();
    if (code) {
        return code;
    }
    code = anonymous_mappings_ignore_the_descriptor();
    if (code) {
        return code;
    }
    printf("[posixtest] the POSIX surface a C++ runtime asks for, verified.\n");
    return 0;
}
