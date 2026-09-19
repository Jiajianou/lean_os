/* The destructors a thread runs on its way out.

   There are two kinds and they arrive from opposite directions. A C++
   thread_local with a non-trivial destructor makes the compiler emit a call
   to __cxa_thread_atexit at the point the object is constructed, which is
   the first time that thread touches it; a pthread_key_create destructor is
   named once, by the program, and applies to every thread that ever put a
   value under that key. POSIX and the C++ standard both say the same thing
   about when they run - as the thread ends, in the reverse of the order they
   were established - so one hook at the one place a thread leaves is what
   both of them are.

   Neither existed here before M165. __cxa_thread_atexit was not declared at
   all, which a link finds and a compile does not; pthread_key_create took a
   destructor argument and discarded it with a (void) cast, which nothing
   finds, because a destructor that never runs looks exactly like a program
   that had nothing to clean up. */

#include <pthread.h>
#include <stdlib.h>

/* Registration happens during construction, and the objects being constructed
   include the ones a C++ runtime brings up before main. A malloc on that path
   is not obviously safe - an allocator with thread_local state of its own
   would be asking this function to run inside its own initialisation - so the
   first few entries come out of the thread's own storage and cost nothing.
   Eight is what a program has to exceed before this reaches the heap at all,
   and Chromium's renderer does not. */
#define INLINE_DESTRUCTORS 8

typedef struct thread_destructor {
    void (*function)(void *);
    void *object;
    struct thread_destructor *next;
} thread_destructor_t;

static __thread struct {
    void (*function)(void *);
    void *object;
} inline_destructors[INLINE_DESTRUCTORS];

static __thread int inline_count;
static __thread thread_destructor_t *heap_destructors;

/* The Itanium C++ ABI's name. glibc also publishes
   __cxa_thread_atexit_impl and clang emits a call to whichever one the
   target is known to have; this target is known to have this one. */
int __cxa_thread_atexit(void (*function)(void *), void *object, void *dso) {
    (void)dso;
    if (!function) {
        return -1;
    }
    if (inline_count < INLINE_DESTRUCTORS) {
        inline_destructors[inline_count].function = function;
        inline_destructors[inline_count].object = object;
        inline_count++;
        return 0;
    }
    thread_destructor_t *entry =
        (thread_destructor_t *)malloc(sizeof(thread_destructor_t));
    if (!entry) {
        return -1;
    }
    entry->function = function;
    entry->object = object;
    entry->next = heap_destructors;
    heap_destructors = entry;
    return 0;
}

/* Each entry is removed from the list BEFORE it is called, which is what
   makes a destructor that constructs another thread_local - and so registers
   another destructor - terminate rather than run the same one for ever. It
   also means a second call to this function finds nothing, which is what
   the main thread needs: it reaches here from exit(), and a program that
   calls exit() from inside a static destructor would otherwise get every
   thread_local destroyed twice. */
static void run_cxx_thread_destructors(void) {
    for (;;) {
        if (heap_destructors) {
            thread_destructor_t *entry = heap_destructors;
            heap_destructors = entry->next;
            void (*function)(void *) = entry->function;
            void *object = entry->object;
            free(entry);
            function(object);
            continue;
        }
        if (inline_count > 0) {
            inline_count--;
            void (*function)(void *) = inline_destructors[inline_count].function;
            void *object = inline_destructors[inline_count].object;
            inline_destructors[inline_count].function = 0;
            inline_destructors[inline_count].object = 0;
            function(object);
            continue;
        }
        return;
    }
}

/* Weak, because the two live in pthread.c and this object is pulled into
   every program that calls exit(). A program with no thread-specific keys
   has no pthread.o in it, and resolving these strongly would put the whole
   thread implementation into every binary on the image to run a sweep over
   a table that does not exist. */
extern void __lean_run_key_destructors(void) __attribute__((weak));
extern void __lean_release_thread_storage(void) __attribute__((weak));

/* The C++ thread_locals first and the pthread keys second, because a
   thread_local's destructor is ordinary C++ that may read a key, and by the
   time the keys have been swept there is nothing left to read. */
void __lean_run_thread_destructors(void) {
    run_cxx_thread_destructors();
    if (__lean_run_key_destructors) {
        __lean_run_key_destructors();
    }
    if (__lean_release_thread_storage) {
        __lean_release_thread_storage();
    }
}
