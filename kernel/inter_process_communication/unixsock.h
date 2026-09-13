#pragma once

#include <stdint.h>

#include "os_net.h"
#include "scheduler/sched.h"

#define UNIX_SOCK_STREAM    1
#define UNIX_SOCK_SEQPACKET 5

#define UNIX_PATH_MAX 108

#define UNIX_BUF_SIZE 4096

#define UNIX_MAX_SEGS 16

#define UNIX_MAX_FDS OS_MSG_MAX_FDS

#define UNIX_BACKLOG     8
#define UNIX_MAX_SOCKETS 64

#define UNIX_MAX_NAMES 16

struct unixsock;

#define UNIX_RECV_TRUNC  1
#define UNIX_RECV_CTRUNC 2

void unixsock_init(void);

struct unixsock *unixsock_alloc(int type);

int unixsock_pair(int type, struct unixsock **a_out, struct unixsock **b_out);

void unixsock_ref(struct unixsock *s);
void unixsock_unref(struct unixsock *s);

int unixsock_type(const struct unixsock *s);

int unixsock_bind(struct unixsock *s, const char *name, int len);

int unixsock_listen(struct unixsock *s);

int unixsock_connect(struct unixsock *s, const char *name, int len);

struct unixsock *unixsock_accept(struct unixsock *listener);

long unixsock_send(struct unixsock *s, const uint8_t *data, uint32_t len,
                   const fd_slot_t *fds, int nfds);

long unixsock_recv(struct unixsock *s, uint8_t *out, uint32_t max,
                   fd_slot_t *fds_out, int max_fds, int *nfds_out,
                   int *flags_out);

int unixsock_pending(const struct unixsock *s);

int unixsock_writable(const struct unixsock *s);
int unixsock_hup(const struct unixsock *s);
int unixsock_rdhup(const struct unixsock *s);

int unixsock_shutdown(struct unixsock *s, int how);

int unixsock_in_use(void);

int unixsock_queued_fds(void);
