#pragma once

#include <stdint.h>

int dhcp_configure(void);

/* Each question - the discover, then the request - is sent up to `attempts`
   times, two seconds apart. A wired boot asks once; a radio that has just
   finished its handshake asks again, because the access point may not have
   opened its port by the time the first frame arrives. `give_up`, when there
   is one, is asked while waiting, so an attempt for a network that has since
   been left stops instead of answering a question nobody is asking. */
typedef int (*dhcp_abandoned_t)(void);
int dhcp_configure_retrying(uint32_t attempts, dhcp_abandoned_t give_up);
