/* tests/fuzz/fuzz_net.c - Q4
 *
 * Coverage-guided fuzzing of every parser that reads bytes off the wire.
 *
 * The unit tests next door check specific malformations that a person
 * thought of. This checks the ones nobody thought of, which is a
 * different job and not a replacement: a fuzzer can only assert "it did
 * not crash", and tests/test_net.c is where "it replied with exactly
 * this" lives. Run both.
 *
 * libFuzzer is the one third-party thing anywhere near this project's own
 * code, and it is a compiler flag rather than a library - clang is already
 * a build dependency for the EFI app. Nothing here ships.
 *
 * The first byte of each input selects a parser, so one corpus and one
 * binary cover the whole inbound path and libFuzzer's coverage feedback
 * can steer between them. Everything after it is the frame.
 *
 * Build and run:
 *   make fuzz            # build every target
 *   make fuzz-run        # 60 seconds each, the pre-commit dose
 *   build/fuzz/fuzz_net -max_total_time=3600 tests/corpus/net
 *
 * A crash writes its input straight into tests/corpus/<target>/ as
 * crash-<hash> (-artifact_prefix, see the Makefile). The corpus around it
 * is gitignored and those files are not, so a finding shows up as exactly
 * one new file in `git status` - which is what turns it into a permanent
 * regression test rather than an afternoon.
 */
#include "net/arp.h"
#include "net/ethernet.h"
#include "net/icmp.h"
#include "net/ip.h"
#include "net/tcp.h"
#include "net/udp.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void fake_net_reset(void);
void fake_socket_reset(void);

static const uint8_t src_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) {
        return 0;
    }
    uint8_t which = data[0];
    const uint8_t *frame = data + 1;
    size_t len = size - 1;

    if (len > 65535) {
        len = 65535;
    }

    /* Copied into an exactly-sized heap allocation rather than passed in
     * place. libFuzzer's own buffer usually has slack after it, and slack
     * is what hides a one-byte over-read: ASan can only report a read past
     * the end if the end is where the allocation ends. This copy is the
     * difference between a fuzzer that finds over-reads and one that does
     * not. */
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) {
        return 0;
    }
    memcpy(buf, frame, len);

    /* The transmit and delivery captures are bounded and would otherwise
     * panic partway through a long campaign for a reason that is about
     * the harness rather than the code. */
    fake_net_reset();
    fake_socket_reset();

    switch (which % 6) {
        case 0: eth_receive(buf, (uint16_t)len); break;
        case 1: arp_handle_packet(buf, (uint16_t)len); break;
        case 2: ip_handle_packet(src_mac, buf, (uint16_t)len); break;
        case 3: icmp_handle_packet(0x0A000202u, buf, (uint16_t)len); break;
        case 4: udp_handle_packet(0x0A000202u, 0x0A00020Fu, buf, (uint16_t)len); break;
        case 5: tcp_handle_packet(0x0A000202u, 0x0A00020Fu, buf, (uint16_t)len); break;
        default: break;
    }

    free(buf);
    return 0;
}
