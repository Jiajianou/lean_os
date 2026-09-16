#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <malloc.h>
#include <stdint.h>
#include <process.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>
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

int main(void) {
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
    printf("[posixtest] the POSIX surface a C++ runtime asks for, verified.\n");
    return 0;
}
