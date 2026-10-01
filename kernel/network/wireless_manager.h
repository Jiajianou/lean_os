#pragma once

#include <stdint.h>

#include "network/ieee80211.h"
#include "wireless.h"

/* The wireless network as this kernel runs it, whatever radio is under it:
   what the last scan heard, which of those networks this machine has joined
   before, joining one - authentication, association, the WPA2 handshake, an
   address - and carrying IP over it once joined.

   A radio is a backend. Its task calls wireless_run() and never returns from
   it; every call below into the backend is made from that task, so a driver
   needs no locks of its own against the manager. Everything that reaches the
   manager from elsewhere - a system call, the IP stack sending a frame - goes
   through a queue the task drains. */

typedef struct {
    const char *name;
    uint8_t address[6];
    /* Listens on every channel, reports each network it heard through
       wireless_heard(), and returns 0 when the scan finished. */
    int (*scan)(void);
    /* Authenticates and associates with `network`, offering `rsn` (empty
       for an open network). 0 when the network accepted; otherwise the
       802.11 status it answered with in *status, or 0xFFFF when it never
       answered. Frames arriving meanwhile go through wireless_received(). */
    int (*join)(const ieee80211_network_t *network, const uint8_t *rsn, uint32_t rsn_length, uint16_t *status);
    /* One Ethernet frame onto the network. */
    int (*transmit)(const uint8_t *ethernet, uint32_t length);
    /* The pairwise key (group 0) or a group key (group 1, with its index). */
    int (*install_key)(int group, const uint8_t *key, uint32_t length, uint8_t index);
    void (*leave)(void);
    /* Gives the device its turn: drains what it received. Negative when the
       device has stopped working. */
    int (*service)(void);
} wireless_backend_t;

/* The task body of a radio: services `backend` and the manager in turn,
   forever. Returns only when the device dies. */
void wireless_run(const wireless_backend_t *backend);

/* Called by a backend from its own task. */
void wireless_heard(const ieee80211_network_t *network, int8_t signal_dbm);
void wireless_received(const uint8_t *ethernet, uint32_t length);
void wireless_departed(uint16_t reason);

/* What the radio is doing while the backend is still starting, or has failed
   to; wireless_run() takes over from there. */
void wireless_set_starting(void);
void wireless_set_failed(void);

/* The system call: any task. Returns 0 or a negative errno-style code;
   NETWORKS returns how many it wrote. */
long wireless_status(os_wireless_status_t *out);
long wireless_networks(os_wireless_network_t *out, uint32_t capacity);
long wireless_request_scan(void);
long wireless_request_connect(const os_wireless_connect_t *request);
long wireless_request_disconnect(void);
long wireless_request_forget(const char *ssid, uint32_t ssid_length);

/* One pass of the loop wireless_run() makes, for the host tests: no task, no
   sleeping. `now_ms` is the time it is. */
void wireless_manager_reset(const wireless_backend_t *backend);
void wireless_step(uint64_t now_ms);

/* Where the networks this machine has joined are kept: a file of lines, the
   name in hexadecimal and the pairwise master key in hexadecimal. The key
   rather than the password, so the password itself is never written down;
   but the key joins the network as well as the password does, and the file
   is as readable as any other in /etc - this machine has one principal. */
#define WIRELESS_KNOWN_PATH "/etc/wireless/known"
#define WIRELESS_KNOWN_MAX 16

int wireless_storage_read(char *buffer, uint32_t capacity);
int wireless_storage_write(const char *buffer, uint32_t length);

/* The IP stack's link send: queues one Ethernet frame for the radio. */
int wireless_link_send(const uint8_t *frame, uint16_t length);

int wireless_manager_busy(void);
int wireless_manager_failed(void);

/* What the manager needs from the rest of the kernel, kept apart so the host
   tests can supply their own: random bytes for the handshake's nonce, and a
   received IP or ARP frame handed to the stack. */
void wireless_random(void *out, uint32_t length);
void *wireless_allocate(uint32_t length);
void wireless_deliver_ip(const uint8_t *frame, uint32_t length);

/* Starts the address request for a network just joined, and says whether
   it has finished: 1 with an address, -1 without, 0 still asking. The kernel
   attaches the radio as the IP link and runs DHCP on a task of its own. */
int wireless_addressing_start(const uint8_t address[6]);
int wireless_addressing_poll(uint32_t *ip);
void wireless_addressing_stop(void);
