/* kernel/net/dhcp.h
 *
 * M64: a DHCP client, so the address stops being a constant.
 *
 * Runs once at boot, from net_init, before anything else can send. It
 * does not use the socket layer above it and could not: a socket binds
 * to a port on an address, and the entire point of DHCP is that there is
 * no address yet. So it drives udp_send directly, from 0.0.0.0 to the
 * all-ones broadcast, and reads its replies out of a port it registers
 * with the socket layer as a plain kernel-side handler.
 *
 * DISCOVER -> OFFER -> REQUEST -> ACK, and nothing else: no lease
 * renewal, no RELEASE, no rebinding, no NAK handling beyond giving up.
 * A machine that runs for a week on a two-hour lease will lose its
 * address, and that is an honest limitation rather than a hidden one -
 * this OS has never run for a week, and a renewal timer is a thing to
 * add the day it does. What it *does* get right is the part that would
 * bite immediately: an xid that ties a reply to this exchange, so
 * another client's ACK on the same segment cannot configure us.
 */
#pragma once

/* Runs the exchange with a bounded timeout and, on success, hands the
 * result to net_set_config. Returns 1 if a lease was obtained, 0 if
 * nothing answered - in which case the fallback constants stand and
 * net_config_is_leased() keeps saying so. */
int dhcp_configure(void);
