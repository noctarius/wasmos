/* net.h - minimal net-stack client helpers for WASM apps.
 *
 * System-wide, dependency-light entry points for talking to the `net.stack`
 * service. Currently just DNS resolution; socket helpers may grow here so apps
 * stop hand-rolling raw NET_IPC_* traffic.
 */
#ifndef WASMOS_NET_H
#define WASMOS_NET_H

#include <stdint.h>

#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

/* Resolve `hostname` to an IPv4 address through net.stack (NET_IPC_RESOLVE).
 *
 * Synchronous from the caller's view: the request is sent and this blocks on
 * `reply_ep` for the reply. net-stack itself never blocks - it defers the reply
 * until lwIP's DNS callback fires, so a slow lookup does not stall the stack.
 * `request_id` must be unique on `reply_ep`. On success returns 0 and writes the
 * resolved address as a network-order IPv4 word (octet a in the low byte, the
 * form `wasmos_ipc`/lwIP use) to *out_addr_no; returns a negative value on any
 * failure (bad args, transport error, NXDOMAIN, or timeout). */
static inline int32_t wasmos_net_resolve(int32_t stack_ep, int32_t reply_ep, const char* hostname,
                                         int32_t request_id, uint32_t* out_addr_no) {
    wasmos_ipc_message_t reply;
    int32_t len = 0;
    int32_t bid;
    int32_t grant;
    int32_t rc;
    if (stack_ep < 0 || reply_ep < 0 || hostname == 0) {
        return -1;
    }
    while (hostname[len] != '\0') {
        len++;
    }
    if (len <= 0) {
        return -1;
    }
    bid = wasmos_xfer_buffer_acquire(len);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, addr_cast(int32_t, hostname), len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    grant = wasmos_xfer_buffer_borrow(stack_ep, bid, WASMOS_BUFFER_GRANT_READ);
    if (grant < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    /* net-stack maps the name, copies it, and unmaps before it can reply, so the
     * buffer is safe to release once the (possibly deferred) reply arrives. */
    rc = wasmos_ipc_call(stack_ep, reply_ep, NET_IPC_RESOLVE, request_id, bid, grant, len, 0,
                         &reply);
    (void)wasmos_xfer_buffer_release(bid);
    if (rc != 0 || reply.type != NET_IPC_RESP || (int32_t)reply.arg0 != NET_STATUS_OK) {
        return -1;
    }
    if (out_addr_no != 0) {
        *out_addr_no = reply.arg1;
    }
    return 0;
}

#endif /* WASMOS_NET_H */
