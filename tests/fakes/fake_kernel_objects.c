#include <stddef.h>
#include <stdint.h>

#include "fakes/fakes.h"

struct pipe;
struct open_file;
struct socket;

static int refs_pipe_read, refs_pipe_write, refs_file, refs_socket;
/* M225: a reference taken on a pipe write end whose count was at or below
   this - an object whose last holder had already let it go. Off (INT_MIN)
   unless a test sets it. */
#define NO_FLOOR (-2147483647 - 1)
static int pipe_write_floor = NO_FLOOR;
static int pipe_write_revivals;
static int pipe_write_dead_uses;
/* M225 (fd-use-holds): which one fake pipe address is a named pipe's -
   made once and kept for good, so a call that uses it holds nothing. */
static const struct pipe *persistent_pipe;
static int destroyed_spaces, shared_memory_frees;

static void (*shared_memory_free_hook)(int owner_task_id);
static void (*region_walk_hook)(void *task, uint32_t index);

/* M225: fake_objects_reset clears the hooks as well as the counters, and so
   does the end of every test (fake_spinlock_at_test_end): a REQUIRE that
   aborted a test before it uninstalled one left it armed, and the next test
   to free shared memory raised SIGKILL on a task pointer from the test
   before - one failure becoming several misleading ones. */
static void clear_hooks(void) {
    shared_memory_free_hook = 0;
    region_walk_hook = 0;
}

void fake_objects_reset(void) {
    refs_pipe_read = refs_pipe_write = refs_file = refs_socket = 0;
    pipe_write_floor = NO_FLOOR;
    pipe_write_revivals = 0;
    pipe_write_dead_uses = 0;
    persistent_pipe = (const struct pipe *)0;
    destroyed_spaces = shared_memory_frees = 0;
    clear_hooks();
}
int fake_objects_pipe_read_refs(void) { return __atomic_load_n(&refs_pipe_read, __ATOMIC_ACQUIRE); }
int fake_objects_pipe_write_refs(void) { return __atomic_load_n(&refs_pipe_write, __ATOMIC_ACQUIRE); }
int fake_objects_file_refs(void) { return refs_file; }
int fake_objects_socket_refs(void) { return refs_socket; }
int fake_objects_address_spaces_destroyed(void) { return destroyed_spaces; }
int fake_objects_shared_memory_frees(void) { return shared_memory_frees; }

void fake_objects_pipe_write_floor(int floor) {
    __atomic_store_n(&pipe_write_floor, floor, __ATOMIC_RELEASE);
}
int fake_objects_pipe_write_revivals(void) {
    return __atomic_load_n(&pipe_write_revivals, __ATOMIC_ACQUIRE);
}

void fake_objects_pipe_write_use(void) {
    if (__atomic_load_n(&refs_pipe_write, __ATOMIC_ACQUIRE) <=
        __atomic_load_n(&pipe_write_floor, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&pipe_write_dead_uses, 1, __ATOMIC_ACQ_REL);
    }
}
int fake_objects_pipe_write_dead_uses(void) {
    return __atomic_load_n(&pipe_write_dead_uses, __ATOMIC_ACQUIRE);
}

/* M225: atomic, because tests/test_descriptor_slots_threads.c takes and
   drops these from several real threads at once - which the machine's own
   objects do under their own locks. A plain ++ here would lose updates of
   its own and blame the code under test for them. */
void pipe_reference_read(struct pipe *p) { (void)p; __atomic_fetch_add(&refs_pipe_read, 1, __ATOMIC_ACQ_REL); }
void pipe_reference_write(struct pipe *p) {
    (void)p;
    int before = __atomic_fetch_add(&refs_pipe_write, 1, __ATOMIC_ACQ_REL);
    if (before <= __atomic_load_n(&pipe_write_floor, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&pipe_write_revivals, 1, __ATOMIC_ACQ_REL);
    }
}
void fake_objects_pipe_persistent(const struct pipe *p) { persistent_pipe = p; }
int pipe_is_persistent(const struct pipe *p) { return p && p == persistent_pipe; }
void pipe_unref_read(struct pipe *p) { (void)p; __atomic_fetch_sub(&refs_pipe_read, 1, __ATOMIC_ACQ_REL); }
void pipe_unref_write(struct pipe *p) { (void)p; __atomic_fetch_sub(&refs_pipe_write, 1, __ATOMIC_ACQ_REL); }

void open_file_reference(struct open_file *f) { (void)f; __atomic_fetch_add(&refs_file, 1, __ATOMIC_ACQ_REL); }
void open_file_unref(struct open_file *f) { (void)f; __atomic_fetch_sub(&refs_file, 1, __ATOMIC_ACQ_REL); }

void socket_reference(struct socket *s) { (void)s; __atomic_fetch_add(&refs_socket, 1, __ATOMIC_ACQ_REL); }
void socket_unref(struct socket *s) { (void)s; __atomic_fetch_sub(&refs_socket, 1, __ATOMIC_ACQ_REL); }

/* M225: the first thing task_exit does, so a test can stand in for what the
   rest of the machine does while a task is part way out (one shot). */
void fake_objects_on_shared_memory_free(void (*hook)(int owner_task_id)) {
    fake_spinlock_at_test_end(clear_hooks);
    shared_memory_free_hook = hook;
}

void fake_objects_on_region_walk_step(void (*hook)(void *task, uint32_t index)) {
    fake_spinlock_at_test_end(clear_hooks);
    region_walk_hook = hook;
}

/* Called by scheduler.c's region walk in a host build (REGION_WALK_STEP). */
void fake_region_walk_step(void *task, uint32_t index);
void fake_region_walk_step(void *task, uint32_t index) {
    if (region_walk_hook) {
        region_walk_hook(task, index);
    }
}

void shared_memory_free_by_owner(int owner_task_id) {
    shared_memory_frees++;
    void (*hook)(int) = shared_memory_free_hook;
    shared_memory_free_hook = 0;
    if (hook) {
        hook(owner_task_id);
    }
}

void process_destroy_address_space(uint64_t pml4_phys) {
    (void)pml4_phys;
    destroyed_spaces++;
}

int64_t virtual_file_system_handle_read(int handle, void *buffer, size_t length, uint32_t off) {
    (void)handle; (void)buffer; (void)length; (void)off;
    return -1;
}
uint64_t file_mapping_get(int handle, uint32_t index, int writable) {
    (void)handle; (void)index; (void)writable;
    return 0;
}
void file_mapping_put(int handle, uint32_t index) { (void)handle; (void)index; }
