#pragma once

#include <stdint.h>

#include "os_network.h"
#include "scheduler/scheduler.h"

#define UNIX_SOCKET_STREAM    1
#define UNIX_SOCKET_SEQPACKET 5

#define UNIX_PATH_MAX 108

#define UNIX_BUFFER_SIZE 65536

#define UNIX_MAX_SEGS 16

#define UNIX_MAX_FILE_DESCRIPTORS OS_MESSAGE_MAX_FILE_DESCRIPTORS

#define UNIX_BACKLOG     8
/* Every mojo channel between two processes is a pair (M187: 64 until then).
   Each carries its UNIX_BUFFER_SIZE ring inline, so this is 32 MB of kernel
   heap if every one is in use at once. */
#define UNIX_MAX_SOCKETS 512

#define UNIX_MAX_NAMES 16

struct unix_socket;

#define UNIX_RECEIVE_TRUNC  1
#define UNIX_RECEIVE_CTRUNC 2

void unix_socket_init(void);

struct unix_socket *unix_socket_alloc(int type);

int unix_socket_pair(int type, struct unix_socket **a_out, struct unix_socket **b_out);

void unix_socket_reference(struct unix_socket *s);
void unix_socket_unref(struct unix_socket *s);

int unix_socket_type(const struct unix_socket *s);

int unix_socket_peer_pid(const struct unix_socket *s);

int unix_socket_bind(struct unix_socket *s, const char *name, int length);

int unix_socket_listen(struct unix_socket *s);

int unix_socket_connect(struct unix_socket *s, const char *name, int length);

struct unix_socket *unix_socket_accept(struct unix_socket *listener);

long unix_socket_send(struct unix_socket *s, const uint8_t *data, uint32_t length,
                   const file_descriptor_slot_t *file_descriptors, int nfds);

long unix_socket_receive(struct unix_socket *s, uint8_t *out, uint32_t max,
                   file_descriptor_slot_t *file_descriptors_out, int max_file_descriptors, int *nfds_out,
                   int *flags_out);

/* The bytes that are queued, without taking them out of the queue - the
   AF_UNIX half of MSG_PEEK. Descriptors are NOT reported: a peek takes
   nothing, and a descriptor handed over twice would be two references to
   one object where the sender sent one. They stay queued for the receive
   that follows. */
long unix_socket_peek(struct unix_socket *s, uint8_t *out, uint32_t max);

int unix_socket_pending(const struct unix_socket *s);
int unix_socket_readable_bytes(const struct unix_socket *s);

int unix_socket_writable(const struct unix_socket *s);
int unix_socket_hup(const struct unix_socket *s);
int unix_socket_rdhup(const struct unix_socket *s);

int unix_socket_shutdown(struct unix_socket *s, int how);

int unix_socket_in_use(void);

int unix_socket_queued_file_descriptors(void);
