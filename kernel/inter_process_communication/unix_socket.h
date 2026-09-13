#pragma once

#include <stdint.h>

#include "os_net.h"
#include "scheduler/scheduler.h"

#define UNIX_SOCK_STREAM    1
#define UNIX_SOCK_SEQPACKET 5

#define UNIX_PATH_MAX 108

#define UNIX_BUF_SIZE 4096

#define UNIX_MAX_SEGS 16

#define UNIX_MAX_FDS OS_MSG_MAX_FDS

#define UNIX_BACKLOG     8
#define UNIX_MAX_SOCKETS 64

#define UNIX_MAX_NAMES 16

struct unix_socket;

#define UNIX_RECV_TRUNC  1
#define UNIX_RECV_CTRUNC 2

void unix_socket_init(void);

struct unix_socket *unix_socket_alloc(int type);

int unix_socket_pair(int type, struct unix_socket **a_out, struct unix_socket **b_out);

void unix_socket_reference(struct unix_socket *s);
void unix_socket_unref(struct unix_socket *s);

int unix_socket_type(const struct unix_socket *s);

int unix_socket_bind(struct unix_socket *s, const char *name, int len);

int unix_socket_listen(struct unix_socket *s);

int unix_socket_connect(struct unix_socket *s, const char *name, int len);

struct unix_socket *unix_socket_accept(struct unix_socket *listener);

long unix_socket_send(struct unix_socket *s, const uint8_t *data, uint32_t len,
                   const file_descriptor_slot_t *fds, int nfds);

long unix_socket_receive(struct unix_socket *s, uint8_t *out, uint32_t max,
                   file_descriptor_slot_t *file_descriptors_out, int max_fds, int *nfds_out,
                   int *flags_out);

int unix_socket_pending(const struct unix_socket *s);

int unix_socket_writable(const struct unix_socket *s);
int unix_socket_hup(const struct unix_socket *s);
int unix_socket_rdhup(const struct unix_socket *s);

int unix_socket_shutdown(struct unix_socket *s, int how);

int unix_socket_in_use(void);

int unix_socket_queued_file_descriptors(void);
