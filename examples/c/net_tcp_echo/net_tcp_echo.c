/* net_tcp_echo - TCP socket-ring smoke client for the native net-stack.
 *
 * Opens a stream socket backed by client-owned TX/RX byte rings (overlaid into
 * linear memory, zero-copy), connects to the QEMU SLIRP host echo server,
 * streams a payload through the TX ring, and verifies the echo arrives in the
 * RX ring.
 *
 * The connect handshake is driven as a coroutine over the IPC-future bridge:
 * each NET_IPC step is sent through wasmos_sys_wasm_ipc_future_send and awaited
 * with wasmos_future_await, so the process yields to its event loop instead of
 * blocking in-hostcall. (Slice 1: connect is non-blocking; send/recv still use
 * the ring helpers on the ready socket and are converted next.) */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/net.h"
#include "wasmos/startup.h"

/* SLIRP host gateway 10.0.2.2 as the network-order IPv4 word, TCP echo port. */
#define ECHO_ADDR_V4 0x0202000Au
#define ECHO_PORT 5556u
#define RING_CAP 4096u

/* Coroutine state for a non-blocking TCP connect: sends NET_IPC_SOCKET_OPEN then
 * NET_IPC_CONNECT through the IPC-future bridge and awaits each reply. */
typedef struct {
    int pc;
    wasmos_net_tcp_t* sock;
    wasmos_sys_event_loop_t* loop;
    int32_t stack_ep;
    int32_t reply_ep;
    uint32_t addr;
    uint16_t port;
    uint32_t cap;
    wasmos_sys_wasm_ipc_future_t op;
    wasmos_future_t* future;
    int32_t desc_bid;
    int32_t desc_grant;
    int32_t result; /* 0 connected, -1 failed */
} connect_co_t;

static int32_t connect_fail(connect_co_t* c, uintptr_t* out_value) {
    c->result = -1;
    *out_value = (uintptr_t)(-1);
    return WASMOS_WASM_TASK_COMPLETE;
}

static int32_t connect_resume(void* user, uintptr_t* out_value) {
    connect_co_t* c = user;
    wasmos_net_tcp_t* s = c->sock;

    if (c->pc == 0) {
        net_socket_open_descriptor_v1_t desc;
        uint32_t region = wasmos_ringbuf_bytes_for(c->cap);
        wasmos_net__reset(s, c->stack_ep, c->reply_ep, 1);
        c->desc_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(desc));
        if (wasmos_net__setup_rings(s, c->stack_ep, c->cap) != 0 || c->desc_bid < 0) {
            return connect_fail(c, out_value);
        }
        c->desc_grant = wasmos_xfer_buffer_borrow(c->stack_ep, c->desc_bid, WASMOS_BUFFER_GRANT_READ);
        if (c->desc_grant < 0) {
            return connect_fail(c, out_value);
        }
        wasmos_net__fill_desc(&desc, s->tx_bid, s->tx_grant, s->rx_bid, s->rx_grant, region, 0u, 0);
        if (wasmos_xfer_buffer_write(c->desc_bid, addr_cast(int32_t, &desc), (int32_t)sizeof(desc),
                                     0) != 0) {
            return connect_fail(c, out_value);
        }
        wasmos_sys_wasm_ipc_future_init(&c->op, NULL, NULL);
        c->future = wasmos_sys_wasm_ipc_future_send(c->loop, &c->op, c->stack_ep, c->reply_ep,
                                                    NET_IPC_SOCKET_OPEN, c->desc_bid, c->desc_grant,
                                                    (int32_t)sizeof(desc), 0, NULL);
        if (c->future == 0) {
            return connect_fail(c, out_value);
        }
        c->pc = 1;
    }
    if (c->pc == 1) {
        uintptr_t val;
        int32_t st = wasmos_future_await(c->future, &val);
        if (st == WASMOS_WASM_AWAIT_PENDING) {
            return WASMOS_WASM_TASK_YIELDED;
        }
        if (st != 0 || c->op.reply.arg0 < 0) {
            return connect_fail(c, out_value);
        }
        s->socket_id = c->op.reply.arg0;
        (void)wasmos_xfer_buffer_release(c->desc_bid);
        c->desc_bid = -1;
        wasmos_sys_wasm_ipc_future_init(&c->op, NULL, NULL);
        c->future = wasmos_sys_wasm_ipc_future_send(c->loop, &c->op, c->stack_ep, c->reply_ep,
                                                    NET_IPC_CONNECT, (uint32_t)s->socket_id,
                                                    (int32_t)c->port, (int32_t)c->addr, 0, NULL);
        if (c->future == 0) {
            return connect_fail(c, out_value);
        }
        c->pc = 2;
    }
    if (c->pc == 2) {
        uintptr_t val;
        int32_t st = wasmos_future_await(c->future, &val);
        if (st == WASMOS_WASM_AWAIT_PENDING) {
            return WASMOS_WASM_TASK_YIELDED;
        }
        if (st != 0) {
            return connect_fail(c, out_value);
        }
        if (c->op.reply.arg0 != NET_STATUS_OK) {
            return connect_fail(c, out_value);
        }
        c->result = 0;
        *out_value = 0u;
        return WASMOS_WASM_TASK_COMPLETE;
    }
    return connect_fail(c, out_value);
}

int main(void) {
    int32_t proc_ep = wasmos_startup_arg(0);
    int32_t reply_ep = wasmos_ipc_create_endpoint();
    int32_t stack_ep = -1;
    int32_t rid = 1;
    wasmos_net_tcp_t sock;
    wasmos_wasm_runtime_t runtime;
    wasmos_sys_event_loop_t loop;
    wasmos_wasm_coroutine_t coro = {0};
    connect_co_t cstate = {0};
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

    /* Non-blocking connect: drive the handshake coroutine from the app event
     * loop, which blocks on its select-set (never busy-polls) and resolves each
     * step's future when the reply arrives. */
    wasmos_wasm_runtime_init(&runtime);
    wasmos_sys_event_loop_init(&loop, reply_ep, 1000);
    cstate.sock = &sock;
    cstate.loop = &loop;
    cstate.stack_ep = stack_ep;
    cstate.reply_ep = reply_ep;
    cstate.addr = ECHO_ADDR_V4;
    cstate.port = ECHO_PORT;
    cstate.cap = RING_CAP;
    if (wasmos_async_start(&runtime, &coro, connect_resume, &cstate) == 0) {
        puts("[net-tcp-echo] connect start failed");
        return 1;
    }
    while (coro.state != WASMOS_WASM_COROUTINE_DEAD) {
        (void)wasmos_wasm_coroutine_run(&runtime);
        if (coro.state == WASMOS_WASM_COROUTINE_DEAD) {
            break;
        }
        (void)wasmos_sys_event_loop_poll(&loop, 8);
    }
    if (cstate.result != 0) {
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
