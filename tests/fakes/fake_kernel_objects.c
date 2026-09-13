#include <stddef.h>
#include <stdint.h>

#include "fakes/fakes.h"

struct pipe;
struct open_file;
struct socket;

static int refs_pipe_read, refs_pipe_write, refs_file, refs_socket;
static int destroyed_spaces, shared_memory_frees;

void fake_objects_reset(void) {
    refs_pipe_read = refs_pipe_write = refs_file = refs_socket = 0;
    destroyed_spaces = shared_memory_frees = 0;
}
int fake_objects_pipe_read_refs(void) { return refs_pipe_read; }
int fake_objects_pipe_write_refs(void) { return refs_pipe_write; }
int fake_objects_file_refs(void) { return refs_file; }
int fake_objects_socket_refs(void) { return refs_socket; }
int fake_objects_address_spaces_destroyed(void) { return destroyed_spaces; }
int fake_objects_shared_memory_frees(void) { return shared_memory_frees; }

void pipe_reference_read(struct pipe *p) { (void)p; refs_pipe_read++; }
void pipe_reference_write(struct pipe *p) { (void)p; refs_pipe_write++; }
void pipe_unref_read(struct pipe *p) { (void)p; refs_pipe_read--; }
void pipe_unref_write(struct pipe *p) { (void)p; refs_pipe_write--; }

void open_file_reference(struct open_file *f) { (void)f; refs_file++; }
void open_file_unref(struct open_file *f) { (void)f; refs_file--; }

void socket_reference(struct socket *s) { (void)s; refs_socket++; }
void socket_unref(struct socket *s) { (void)s; refs_socket--; }

void shared_memory_free_by_owner(int owner_task_id) { (void)owner_task_id; shared_memory_frees++; }

void process_destroy_address_space(uint64_t pml4_phys) {
    (void)pml4_phys;
    destroyed_spaces++;
}

int64_t virtual_file_system_handle_read(int handle, void *buf, size_t len, uint32_t off) {
    (void)handle; (void)buf; (void)len; (void)off;
    return -1;
}
uint64_t file_mapping_get(int handle, uint32_t index, int writable) {
    (void)handle; (void)index; (void)writable;
    return 0;
}
void file_mapping_put(int handle, uint32_t index) { (void)handle; (void)index; }
