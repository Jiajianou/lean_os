/* tests/fakes/fake_net.c - Q4
 *
 * The wire, as a list of frames nobody sent.
 *
 * The point of this fake is not to let the parsers run - it is to let a
 * test assert what they *replied*. "It did not crash on a malformed
 * packet" is a weak claim; "it replied to this ARP request with exactly
 * this reply, and it replied to that malformed one with nothing at all"
 * is the claim worth making, and it needs the transmit side captured.
 *
 * The addresses are fixed and are the ones QEMU's SLIRP backend hands
 * out, so a test's expectations read the same as the boot self-test's. */
#include "net/net.h"
#include "drivers/rtl8139.h"

#include <stdint.h>
#include <string.h>

void panic(const char *msg);

#define MAX_TX 64
#define MAX_FRAME 2048

static uint8_t tx[MAX_TX][MAX_FRAME];
static uint32_t tx_len[MAX_TX];
static int tx_count;

static const uint8_t local_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static uint32_t local_ip   = 0x0A00020Fu; /* 10.0.2.15 */
static uint32_t gateway_ip = 0x0A000202u; /* 10.0.2.2  */
static uint32_t subnet     = 0xFFFFFF00u;
static uint32_t dns_ip     = 0x0A000203u;

void fake_net_reset(void);
int fake_net_tx_count(void);
const uint8_t *fake_net_tx_frame(int i, uint32_t *len_out);

void fake_net_reset(void) {
    tx_count = 0;
    memset(tx_len, 0, sizeof(tx_len));
}

int fake_net_tx_count(void) { return tx_count; }

const uint8_t *fake_net_tx_frame(int i, uint32_t *len_out) {
    if (i < 0 || i >= tx_count) {
        return NULL;
    }
    if (len_out) {
        *len_out = tx_len[i];
    }
    return tx[i];
}

int rtl8139_send(const uint8_t *frame, uint16_t len) {
    if (tx_count >= MAX_TX) {
        /* Not silently dropped. A parser that answers one bad packet with
         * sixty-four frames is a bug worth stopping on, not one to
         * discover by reading a count afterwards. */
        panic("fake_net: transmit queue overflow - the stack is replying far too much");
    }
    if (len > MAX_FRAME) {
        panic("fake_net: a frame longer than the fake can hold");
    }
    memcpy(tx[tx_count], frame, len);
    tx_len[tx_count] = len;
    tx_count++;
    /* Q16: the driver reports now - see kernel/drivers/rtl8139.h. This
     * fake never refuses a frame it could hold, because the cases it
     * refuses are harness bugs and panic above. */
    return 0;
}

int rtl8139_init(void) { return 1; }
uint32_t rtl8139_tx_error_count(void) { return 0; }
const uint8_t *rtl8139_mac(void) { return local_mac; }

const uint8_t *net_local_mac(void) { return local_mac; }
uint32_t net_local_ip(void) { return local_ip; }
uint32_t net_gateway_ip(void) { return gateway_ip; }
uint32_t net_subnet_mask(void) { return subnet; }
uint32_t net_dns_ip(void) { return dns_ip; }
int net_have_nic(void) { return 1; }
int net_config_is_leased(void) { return 0; }
int net_is_local_ip(uint32_t ip) { return ip == local_ip; }

void net_set_config(uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns) {
    local_ip = ip;
    subnet = mask;
    gateway_ip = gateway;
    dns_ip = dns;
}

/* Single-threaded here, and deliberately not routed through the fake
 * spinlock: the net lock is taken on the inbound path and a test that
 * drives two packets back to back would trip the recursion check for a
 * reason that is about this harness rather than about the kernel. */
void net_lock_acquire(void) {}
void net_lock_release(void) {}

int net_init(void) { return 1; }
