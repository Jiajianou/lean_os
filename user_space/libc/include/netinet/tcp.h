#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_NODELAY     1
#define TCP_MAXSEG      2
#define TCP_KEEPIDLE    4
#define TCP_KEEPINTVL   5
#define TCP_KEEPCNT     6

/* M161. Linux's own: how long a connection may go unacknowledged before the
   kernel gives up on it, in milliseconds. M69's rule is why this kernel does
   not act on it - the retransmission timeout here is measured rather than
   configured, and nothing has shown that a caller-supplied ceiling would
   improve anything - so setsockopt refuses it with ENOPROTOOPT and the
   caller learns that. WebRTC asked. */
#define TCP_USER_TIMEOUT 18

#ifdef __cplusplus
}
#endif
