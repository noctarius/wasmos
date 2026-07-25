/* net_tcp_echo - TCP socket-ring smoke client for the native net-stack.
 *
 * Opens a stream socket backed by client-owned TX/RX byte rings (overlaid into
 * linear memory, zero-copy), connects to the QEMU SLIRP host echo server,
 * streams a payload through the TX ring, and verifies the echo arrives in the
 * RX ring. Exercises the shared wasmos_net_tcp_* helper end to end. */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/net.h"
#include "wasmos/startup.h"

/* SLIRP host gateway 10.0.2.2 as the network-order IPv4 word, TCP echo port. */
#define ECHO_ADDR_V4 0x0202000Au
#define ECHO_PORT 5556u
#define RING_CAP 4096u

int main(void) {
    int32_t proc_ep = wasmos_startup_arg(0);
    int32_t reply_ep = wasmos_ipc_create_endpoint();
    int32_t stack_ep = -1;
    int32_t rid = 1;
    wasmos_net_tcp_t sock;
    static const uint8_t payload[] = "wasmos-tcp-echo";
    uint32_t len = sizeof(payload) - 1u;
    uint8_t response[64];
    uint32_t got = 0;

    if (proc_ep <= 0 || reply_ep < 0) {
        puts("[net-tcp-echo] setup failed");
        return 1;
    }
    for (int spin = 0; spin < 4096 && stack_ep < 0; ++spin) {
        stack_ep = wasmos_svc_lookup(proc_ep, reply_ep, "net.stack", rid++);
        if (stack_ep < 0) {
            (void)wasmos_sched_yield();
        }
    }
    if (stack_ep < 0) {
        puts("[net-tcp-echo] no net.stack");
        return 1;
    }
    puts("[net-tcp-echo] found net.stack");

    if (wasmos_net_tcp_connect(&sock, stack_ep, reply_ep, ECHO_ADDR_V4, ECHO_PORT, RING_CAP, rid) !=
        0) {
        puts("[net-tcp-echo] connect failed");
        return 1;
    }
    puts("[net-tcp-echo] connected");

    if (wasmos_net_tcp_send(&sock, payload, (int32_t)len) != (int32_t)len) {
        puts("[net-tcp-echo] send failed");
        wasmos_net_tcp_close(&sock);
        return 1;
    }

    /* The echo may stream back in more than one segment. */
    while (got < len) {
        int32_t n = wasmos_net_tcp_recv(&sock, response + got, (int32_t)(sizeof(response) - got));
        if (n <= 0) {
            break;
        }
        got += (uint32_t)n;
    }
    wasmos_net_tcp_close(&sock);
    if (got < len) {
        puts("[net-tcp-echo] no echo");
        return 1;
    }
    for (uint32_t i = 0; i < len; ++i) {
        if (response[i] != payload[i]) {
            puts("[net-tcp-echo] payload mismatch");
            return 1;
        }
    }
    puts("[net-tcp-echo] echo ok");
    return 0;
}
