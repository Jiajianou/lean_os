/* user_space/libc/include/netinet/tcp.h - M89
 *
 * TCP-level socket options.
 *
 * TCP_NODELAY is the one programs set, and it is worth knowing what it
 * means here: this stack does not implement Nagle's algorithm at all -
 * it is on the deferred list in milestones.md, waiting for a
 * measurement - so a segment is already sent when it is written.
 * Disabling an algorithm that is not running is a request this system
 * can honestly grant, and setsockopt accepts it. Turning it *on* is
 * refused, because that would be agreeing to buffer.
 */
#pragma once

#define TCP_NODELAY     1
#define TCP_MAXSEG      2
#define TCP_KEEPIDLE    4
#define TCP_KEEPINTVL   5
#define TCP_KEEPCNT     6
