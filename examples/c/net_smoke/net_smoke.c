/* net_smoke - minimal virtio-net consumer that validates RX_FRAME_NOTIFY.
 *
 * Looks up the virtio.net driver, fetches the NIC MAC, subscribes to RX
 * notifications, transmits an ARP request for the SLIRP gateway (10.0.2.2), and
 * waits for the reply to be pushed back via NETDRV_IPC_RX_FRAME_NOTIFY (rather
 * than polling). Proves the driver -> consumer receive path end to end. */

#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmos/libsys.h"
#include "wasmos_driver_abi.h"

/* Block until a message arrives on `ep` (bounded so a stuck driver can't hang
 * the test). Returns 0 and fills `m`, or -1 on timeout. */
static int
recv_on(int32_t ep, wasmos_ipc_message_t *m)
{
    for (int spin = 0; spin < 200000; ++spin) {
        if (wasmos_ipc_select_one(ep) == 1) {
            wasmos_ipc_message_read_last(m);
            return 0;
        }
        (void)wasmos_sched_yield();
    }
    return -1;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int32_t proc_ep = wasmos_startup_arg(0);
    int32_t reply_ep = wasmos_ipc_create_endpoint();
    if (proc_ep <= 0 || reply_ep < 0) {
        puts("[net-smoke] setup failed");
        return 1;
    }

    int32_t req = 1;
    int32_t net_ep = -1;
    for (int spin = 0; spin < 2048 && net_ep < 0; ++spin) {
        net_ep = wasmos_svc_lookup(proc_ep, reply_ep, "virtio.net", req++);
        if (net_ep < 0) {
            (void)wasmos_sched_yield();
        }
    }
    if (net_ep < 0) {
        puts("[net-smoke] no virtio.net");
        return 1;
    }
    puts("[net-smoke] found virtio.net");

    wasmos_ipc_message_t m;

    /* Fetch the NIC MAC (driver writes 6 bytes into our xfer buffer). SLIRP
     * unicasts its ARP reply to the sender MAC, so it must be the real NIC MAC
     * or the device would filter the reply out. */
    uint8_t mac[6];
    if (wasmos_ipc_send(net_ep, reply_ep, NETDRV_IPC_LINK_GET, req++, 0, 0, 0, 0) != 0
        || recv_on(reply_ep, &m) != 0 || m.type != NETDRV_IPC_RESP
        || wasmos_xfer_buffer_read((int32_t)(uintptr_t)mac, 6, 0) != 0) {
        puts("[net-smoke] link_get failed");
        return 1;
    }

    /* Subscribe (and drain any already-queued frames). */
    if (wasmos_ipc_send(net_ep, reply_ep, NETDRV_IPC_RX_POLL, req++, 0, 0, 0, 0) != 0
        || recv_on(reply_ep, &m) != 0) {
        puts("[net-smoke] subscribe failed");
        return 1;
    }

    /* Build an ARP request for 10.0.2.2 with the NIC MAC as sender. */
    static const uint8_t sender_ip[4] = {10, 0, 2, 15};
    static const uint8_t target_ip[4] = {10, 0, 2, 2};
    uint8_t arp[42];
    uint32_t p = 0;
    int i;
    for (i = 0; i < 6; ++i) arp[p++] = 0xFFu;            /* dst broadcast */
    for (i = 0; i < 6; ++i) arp[p++] = mac[i];           /* src NIC MAC */
    arp[p++] = 0x08u; arp[p++] = 0x06u;                  /* ethertype ARP */
    arp[p++] = 0x00u; arp[p++] = 0x01u;                  /* htype */
    arp[p++] = 0x08u; arp[p++] = 0x00u;                  /* ptype */
    arp[p++] = 0x06u; arp[p++] = 0x04u;                  /* hlen, plen */
    arp[p++] = 0x00u; arp[p++] = 0x01u;                  /* oper=request */
    for (i = 0; i < 6; ++i) arp[p++] = mac[i];           /* sender MAC */
    for (i = 0; i < 4; ++i) arp[p++] = sender_ip[i];     /* sender IP */
    for (i = 0; i < 6; ++i) arp[p++] = 0x00u;            /* target MAC */
    for (i = 0; i < 4; ++i) arp[p++] = target_ip[i];     /* target IP */

    if (wasmos_xfer_buffer_write((int32_t)(uintptr_t)arp, (int32_t)p, 0) != 0
        || wasmos_ipc_send(net_ep, reply_ep, NETDRV_IPC_TX_FRAME, req++,
                           (int32_t)p, 0, 0, 0) != 0
        || recv_on(reply_ep, &m) != 0 || m.type != NETDRV_IPC_RESP) {
        puts("[net-smoke] tx failed");
        return 1;
    }
    puts("[net-smoke] arp sent");

    /* Block for the driver's RX_FRAME_NOTIFY push, then pull the frame with
     * RX_POLL. This is a pure push path — no polling — and depends on the device
     * interrupt re-firing (PCI INTx level/active-low + directed IOAPIC EOI).
     * The driver's own boot ARP is interrupt #1; this reply is interrupt #2, so
     * receiving the notify proves re-delivery works. */
    for (int rounds = 0; rounds < 8; ++rounds) {
        if (recv_on(reply_ep, &m) != 0) {
            break;  /* no notify arrived — re-delivery broken */
        }
        if (m.type != NETDRV_IPC_RX_FRAME_NOTIFY) {
            continue;
        }
        if (wasmos_ipc_send(net_ep, reply_ep, NETDRV_IPC_RX_POLL, req++, 0, 0, 0, 0) != 0
            || recv_on(reply_ep, &m) != 0 || m.type != NETDRV_IPC_RESP) {
            break;
        }
        int32_t len = m.arg0;
        if (len <= 0) {
            continue;
        }
        uint8_t frame[128];
        int32_t rd = len < (int32_t)sizeof frame ? len : (int32_t)sizeof frame;
        if (wasmos_xfer_buffer_read((int32_t)(uintptr_t)frame, rd, 0) != 0) {
            break;
        }
        unsigned et = ((unsigned)frame[12] << 8) | (unsigned)frame[13];
        (void)printf("[net-smoke] notify rx=%d ethertype=0x%04X "
                     "from=%02X:%02X:%02X:%02X:%02X:%02X\n",
                     (int)len, et, frame[6], frame[7], frame[8],
                     frame[9], frame[10], frame[11]);
        return 0;
    }

    puts("[net-smoke] no notify");
    return 1;
}
