/* net_smoke - packet-socket smoke client that validates the RX push path.
 *
 * Opens an AF_PACKET / NET_SOCKET_RAW socket on net-stack, transmits an ARP
 * request for the SLIRP gateway (10.0.2.2), and waits for the reply to be pushed
 * back over the socket's RX ring rather than polled for.
 *
 * ARP is the reason a packet socket exists: it lives below IP, so no stream or
 * datagram socket can carry it. Everything under this app -- net-stack's frame
 * fan-out, the driver's per-consumer RX queues, and the device interrupt that
 * starts it -- has to work for the reply to arrive, and the app reaches none of
 * it directly: it never talks to the NIC driver, so it cannot compete with
 * net-stack for the device's frames. */

#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/net.h"
#include "wasmos/startup.h"
#include "wasmos/libsys.h"
#include "wasmos_driver_abi.h"

/* Per-direction ring capacity. A power of two, and large enough for a burst of
 * full-size frames so a slow reader does not drop the reply it is waiting for. */
#define NET_SMOKE_RING_CAP 4096u

/* How many RX doorbells to wait through before giving up. Broadcast traffic
 * shares the socket, so the ARP reply is not necessarily the first frame in. */
#define NET_SMOKE_RX_ROUNDS 16

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    int32_t proc_ep = wasmos_startup_proc_endpoint();
    int32_t reply_ep = wasmos_ipc_create_endpoint();
    if (proc_ep <= 0 || reply_ep < 0) {
        puts("[net-smoke] setup failed");
        return 1;
    }

    int32_t rid = 1;
    int32_t stack_ep = -1;
    for (int spin = 0; spin < 2048 && stack_ep < 0; ++spin) {
        stack_ep = wasmos_svc_lookup(proc_ep, reply_ep, "net.stack", rid++);
        if (stack_ep < 0) {
            (void)wasmos_sched_yield();
        }
    }
    if (stack_ep < 0) {
        puts("[net-smoke] no net.stack");
        return 1;
    }
    puts("[net-smoke] found net.stack");

    wasmos_net_tcp_t sock;
    if (wasmos_net_packet_open(&sock, stack_ep, reply_ep, NET_SMOKE_RING_CAP, rid) != 0) {
        puts("[net-smoke] packet socket open failed");
        return 1;
    }
    puts("[net-smoke] packet socket open");

    /* The interface's own MAC, so the gateway's reply is unicast back to this
     * machine rather than dropped as being for someone else. */
    uint8_t mac[6];
    if (wasmos_net_if_hwaddr(stack_ep, reply_ep, mac, rid++) != 0) {
        puts("[net-smoke] hwaddr failed");
        return 1;
    }

    /* ARP request for 10.0.2.2, sent from 10.0.2.15 — QEMU's SLIRP gateway and
     * the address it hands out. */
    static const uint8_t sender_ip[4] = {10, 0, 2, 15};
    static const uint8_t target_ip[4] = {10, 0, 2, 2};
    uint8_t arp[42];
    uint32_t p = 0;
    int i;
    for (i = 0; i < 6; ++i)
        arp[p++] = 0xFFu; /* dst broadcast */
    for (i = 0; i < 6; ++i)
        arp[p++] = mac[i]; /* src: this interface */
    arp[p++] = 0x08u;
    arp[p++] = 0x06u; /* ethertype ARP */
    arp[p++] = 0x00u;
    arp[p++] = 0x01u; /* hardware type ethernet */
    arp[p++] = 0x08u;
    arp[p++] = 0x00u; /* protocol type IPv4 */
    arp[p++] = 6u;    /* hardware address length */
    arp[p++] = 4u;    /* protocol address length */
    arp[p++] = 0x00u;
    arp[p++] = 0x01u; /* opcode: request */
    for (i = 0; i < 6; ++i)
        arp[p++] = mac[i];
    for (i = 0; i < 4; ++i)
        arp[p++] = sender_ip[i];
    for (i = 0; i < 6; ++i)
        arp[p++] = 0x00u; /* target hardware address: unknown */
    for (i = 0; i < 4; ++i)
        arp[p++] = target_ip[i];

    if (wasmos_net_packet_send(&sock, arp, (int32_t)p) != (int32_t)p) {
        puts("[net-smoke] arp send failed");
        wasmos_net_tcp_close(&sock);
        return 1;
    }
    puts("[net-smoke] arp sent");

    /* Wait for the reply to be PUSHED to the socket. Frames other than the reply
     * may arrive first, so keep reading until an ARP frame shows up or the
     * doorbell budget runs out. */
    for (int round = 0; round < NET_SMOKE_RX_ROUNDS; ++round) {
        uint8_t frame[NET_PACKET_FRAME_MAX];
        int32_t len = wasmos_net_packet_recv(&sock, frame, (int32_t)sizeof frame, 2);
        if (len < 14) {
            continue;
        }
        unsigned et = ((unsigned)frame[12] << 8) | (unsigned)frame[13];
        if (et != 0x0806u) {
            continue;
        }
        (void)printf("[net-smoke] notify rx=%d ethertype=0x%04X "
                     "from=%02X:%02X:%02X:%02X:%02X:%02X\n",
                     (int)len,
                     et,
                     frame[6],
                     frame[7],
                     frame[8],
                     frame[9],
                     frame[10],
                     frame[11]);
        wasmos_net_tcp_close(&sock);
        return 0;
    }

    puts("[net-smoke] no notify");
    wasmos_net_tcp_close(&sock);
    return 1;
}
