/* tests/fakes/fake_kernel_objects.c - Q13
 *
 * The refcounted things a task's descriptor table points at, and the two
 * calls a dying task makes into subsystems that are not the scheduler.
 *
 * kernel/sched/sched.c holds pointers to pipes, open files and sockets
 * and does exactly one thing with them - takes a reference when a task
 * inherits a descriptor and drops it when the task dies. That is
 * ownership bookkeeping, and it is testable *as* bookkeeping: this file
 * counts the references outstanding, so "a task that forked and exited
 * left every descriptor's refcount where it found it" becomes an
 * assertion rather than an argument.
 *
 * Linking the real pipe.c, openfile.c and socket.c instead would test
 * three more subsystems at once and make a scheduler failure read as a
 * pipe failure. Counting is the whole of what the scheduler is
 * responsible for here.
 */
#include <stddef.h>
#include <stdint.h>

#include "fakes/fakes.h"

struct pipe;
struct openfile;
struct socket;

static int refs_pipe_read, refs_pipe_write, refs_file, refs_socket;
static int destroyed_spaces, shm_frees;

void fake_objects_reset(void) {
    refs_pipe_read = refs_pipe_write = refs_file = refs_socket = 0;
    destroyed_spaces = shm_frees = 0;
}
int fake_objects_pipe_read_refs(void) { return refs_pipe_read; }
int fake_objects_pipe_write_refs(void) { return refs_pipe_write; }
int fake_objects_file_refs(void) { return refs_file; }
int fake_objects_socket_refs(void) { return refs_socket; }
int fake_objects_address_spaces_destroyed(void) { return destroyed_spaces; }
int fake_objects_shm_frees(void) { return shm_frees; }

void pipe_ref_read(struct pipe *p) { (void)p; refs_pipe_read++; }
void pipe_ref_write(struct pipe *p) { (void)p; refs_pipe_write++; }
void pipe_unref_read(struct pipe *p) { (void)p; refs_pipe_read--; }
void pipe_unref_write(struct pipe *p) { (void)p; refs_pipe_write--; }

void openfile_ref(struct openfile *f) { (void)f; refs_file++; }
void openfile_unref(struct openfile *f) { (void)f; refs_file--; }

void socket_ref(struct socket *s) { (void)s; refs_socket++; }
void socket_unref(struct socket *s) { (void)s; refs_socket--; }

void shm_free_by_owner(int owner_task_id) { (void)owner_task_id; shm_frees++; }

void process_destroy_address_space(uint64_t pml4_phys) {
    (void)pml4_phys;
    destroyed_spaces++;
}

/* The demand-paging seam. sched_fault_fill reaches these for a
 * file-backed mapping; nothing in the scheduler tests maps a file, and a
 * fake that answered would be inventing a filesystem. Both refuse, which
 * is the answer for "there is no file behind this address" and is a path
 * the scheduler has to handle either way. */
int64_t vfs_handle_read(int handle, void *buf, size_t len, uint32_t off) {
    (void)handle; (void)buf; (void)len; (void)off;
    return -1;
}
uint64_t filemap_get(int handle, uint32_t index, int writable) {
    (void)handle; (void)index; (void)writable;
    return 0;
}
void filemap_put(int handle, uint32_t index) { (void)handle; (void)index; }
