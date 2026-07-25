/* net_stack.c - native (non-WASM) net-stack service entry.
 *
 * LIVE CONTROL-PLANE BASELINE:
 *   - Captures the wasmos_driver_api_t table for port.c (time/console).
 *   - Validates the native ABI magic/version (mirrors gfx_compositor).
 *   - Calls lwip_init() once to exercise the linked lwIP core.
 *   - Creates and registers the `net.stack` endpoint, then notifies ready.
 *   - Drains and dispatches every pending socket control message.
 *
 * DATA-PLANE BASELINE:
 *   - Binds one lwIP Ethernet netif to the virtio.net frame service.
 *   - Uses 10.0.2.15/24 with the QEMU SLIRP gateway at 10.0.2.2.
 *   - UDP sockets drain client TX datagram rings and deliver received datagrams
 *     into client RX rings.
 *   - TCP sockets connect asynchronously (deferred reply), stream client TX ring
 *     bytes through tcp_write/tcp_output with tcp_sndbuf backpressure, and copy
 *     inbound segments into the client RX ring, acknowledging via tcp_recved.
 */
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#include "lwip/init.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#include "net_stack_ifcfg.h"
#include "socket.h"
#include "wasmos/libsys_native.h"
#include "wasmos_native_driver.h"

/* The api table captured at initialize() time. Read by port.c (sys_now,
 * lwip_port_rand) and net_stack_lwip_diag(). NULL until initialize() runs. */
static wasmos_driver_api_t* g_api = NULL;
static net_socket_pool_t g_socket_pool;
static uint32_t g_endpoint = 0u;
static uint32_t g_control_endpoint = 0u;
static uint32_t g_netdrv_reply_endpoint = 0u;
static uint32_t g_proc_endpoint = 0u;
static uint32_t g_select_id = 0u;
static uint8_t g_select_ready = 0u;
static uint8_t g_net_want_block = 0u;
static uint32_t g_netdrv_endpoint = 0u;
typedef enum {
    NET_IFC_DISCOVERED = 0,
    NET_IFC_BUFFERS_GRANTED,
    NET_IFC_LINK_QUERIED,
    NET_IFC_NETIF_UP
} net_ifc_state_t;
#define NET_STACK_MAX_INTERFACES 8u
#define NET_STACK_TX_QUEUE_DEPTH 4u
typedef struct {
    uint32_t buffer_id;
    uint8_t* buffer;
    uint8_t pending;
} net_tx_slot_t;
typedef struct {
    uint8_t in_use;
    uint8_t link_up;
    uint16_t reserved;
    uint32_t endpoint;
    uint32_t instance;
    net_ifc_state_t state;
    struct netif netif;
    uint32_t rx_buffer_id;
    uint8_t* rx_buffer;
    net_tx_slot_t tx_slots[NET_STACK_TX_QUEUE_DEPTH];
    uint8_t rx_pending;
    uint32_t rx_poll_next_tick;
    uint8_t netif_ready;
    uint8_t netif_installed;
    uint8_t addr_ready;
    uint8_t dhcp_active;
    uint32_t dhcp_started_tick;
    uint8_t dhcp_timeout_logged;
    uint8_t link_get_pending;
    uint8_t link_get_retire;
    wasmos_native_coroutine_t link_get_coroutine;
    wasmos_sys_native_ipc_future_t link_get_future;
    uint8_t link_get_stack[4096] __attribute__((aligned(16)));
} net_interface_slot_t;
static net_interface_slot_t g_interfaces[NET_STACK_MAX_INTERFACES];
static net_interface_slot_t* g_active_ifc = NULL;
#define g_netif (g_active_ifc->netif)
static net_ifc_state_t g_ifc_state = NET_IFC_DISCOVERED;
static uint32_t g_netifc_lookup_buffer_id = 0u;
static uint8_t* g_netifc_lookup_buffer = NULL;
static uint32_t g_netifc_subscribe_buffer_id = 0u;
static uint8_t* g_netifc_subscribe_buffer = NULL;
static uint8_t g_netifc_lookup_pending = 0u;
static uint8_t g_netifc_subscribe_pending = 0u;
static uint8_t g_netifc_subscribed = 0u;
static uint8_t g_registered = 0u;
static uint8_t g_register_pending = 0u;
static uint8_t g_netdrv_lookup_pending = 0u;
static wasmos_native_coroutine_runtime_t* g_control_runtime = NULL;
static wasmos_native_coroutine_t g_netdrv_lookup_coroutine;
static wasmos_sys_native_ipc_future_t g_netdrv_lookup_future;
static uint8_t g_netdrv_lookup_stack[4096] __attribute__((aligned(16)));
static uint8_t g_service_root_stack[8192] __attribute__((aligned(16)));
int32_t wasmos_async_main(wasmos_driver_api_t* driver_api,
                          wasmos_native_coroutine_runtime_t* runtime, void* user);
static void net_stack_idle(void* user);
wasmos_sys_native_async_service_config_t wasmos_async_service = {
    .root_stack = g_service_root_stack,
    .root_stack_size = sizeof(g_service_root_stack),
    .main = wasmos_async_main,
    .idle = net_stack_idle,
};
static uint8_t g_hrng_lookup_pending = 0u;
static uint8_t g_hrng_seeded = 0u;
static uint32_t g_hrng_lookup_buffer_id = 0u;
static uint8_t* g_hrng_lookup_buffer = NULL;
static wasmos_sys_native_random_request_t g_hrng_request;
static uint32_t g_hrng_word = 0u;
static wasmos_sys_native_event_loop_t g_control_loop;
static wasmos_sys_native_event_loop_t g_netdrv_loop;

#define ND_IPC_OK 0
#define ND_IPC_EMPTY 1
#define NET_STACK_REGISTER_REQUEST_ID 0x4E530001u
#define NET_STACK_RX_POLL_REQUEST_ID 0x4E530003u
#define NET_STACK_TX_REQUEST_BASE 0x4E535000u
#define NET_STACK_FRAME_BYTES 2048u
#define NET_STACK_UDP_DATAGRAM_BYTES 1472u
/* One TX-drain chunk pulled from a stream socket's TX ring per tcp_write. */
#define NET_STACK_TCP_CHUNK_BYTES 1024u
#define NET_STACK_HRNG_LOOKUP_REQUEST_ID 0x4E530004u
#define NET_STACK_HRNG_REQUEST_ID 0x4E530005u
#define NET_STACK_RX_POLL_INTERVAL_TICKS 3u
/* Upper bound on the idle select-wait so the empty-RX poll and other periodic
 * work still run when no watched endpoint fires (~12 ms, matching the RX poll
 * cadence at 250 Hz). Incoming frames and IPC replies wake the wait earlier. */
#define NET_STACK_IDLE_WAIT_MS 12u
#define NET_STACK_IFCFG_PATH "/boot/system/net/interfaces"
#define NET_STACK_IFCFG_CAP 1024u
#define NET_STACK_IFCFG_RETRY_TICKS 25u    /* ~100 ms at 250 Hz between attempts */
#define NET_STACK_IFCFG_MAX_ATTEMPTS 50u   /* bounded post-up retry, then give up */
#define NET_STACK_DHCP_TIMEOUT_TICKS 3750u /* ~15 s at 250 Hz */

/* Interface config (/boot/system/net/interfaces), read when the interface
 * comes up. Strict: absence/failure or DHCP-no-lease leaves it unconfigured. */
static int32_t g_fs_endpoint = 0;
static uint8_t g_fs_lookup_pending = 0u;
static uint8_t g_fs_lookup_stage = 0u; /* 0 = try "fs.vfs", 1 = try legacy "fs" */
static uint8_t g_ifcfg_kicked = 0u;    /* link-up requested a load */
static uint8_t g_ifcfg_loaded = 0u;    /* config applied or given up */
static uint8_t g_ifcfg_read_pending = 0u;
static uint32_t g_ifcfg_buffer_id = 0u;
static uint8_t* g_ifcfg_buffer = NULL;
static uint32_t g_ifcfg_attempts = 0u;
static uint32_t g_ifcfg_next_tick = 0u;

static err_t net_stack_linkoutput(struct netif* netif, struct pbuf* p);
static void net_stack_start_rx_poll(net_interface_slot_t* interface, uint8_t immediate);
static void net_stack_begin_registration(void);
static void net_stack_try_bind_virtio(void);
static void net_stack_try_bind_interface(net_interface_slot_t* slot);
static void net_stack_try_discover_interfaces(void);
static void net_stack_unbind_interface(uint32_t endpoint);
static void net_stack_reap_interfaces(void);
static void net_stack_try_seed_random(void);
static void net_stack_udp_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p,
                               const ip_addr_t* addr, u16_t port);
static void net_stack_kick_ifcfg_load(void);
static void net_stack_try_load_ifcfg(void);
static void net_stack_apply_ifcfg(net_interface_slot_t* interface, const net_ifcfg_t* cfg);
static void net_stack_netif_status_cb(struct netif* netif);
static void net_stack_dispatch(const nd_ipc_message_t* request);
static void net_stack_netdrv_event(void* user, const nd_ipc_message_t* request);

wasmos_driver_api_t* net_stack_api(void) {
    return g_api;
}

static net_interface_slot_t* net_stack_interface_slot(uint32_t endpoint, uint32_t instance,
                                                      uint8_t create) {
    for (uint32_t i = 0u; i < NET_STACK_MAX_INTERFACES; ++i) {
        if (g_interfaces[i].in_use && !g_interfaces[i].link_get_retire &&
            g_interfaces[i].endpoint == endpoint)
            return &g_interfaces[i];
    }
    if (!create)
        return NULL;
    for (uint32_t i = 0u; i < NET_STACK_MAX_INTERFACES; ++i) {
        if (!g_interfaces[i].in_use) {
            __builtin_memset(&g_interfaces[i], 0, sizeof(g_interfaces[i]));
            g_interfaces[i].in_use = 1u;
            g_interfaces[i].endpoint = endpoint;
            g_interfaces[i].instance = instance;
            g_interfaces[i].state = NET_IFC_DISCOVERED;
            return &g_interfaces[i];
        }
    }
    return NULL; /* fixed eight-slot policy: ignore excess providers */
}

/* Minimal string length for the raw diag path below. */
static int ns_strlen(const char* s) {
    int n = 0;
    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

/* lwIP LWIP_PLATFORM_DIAG hook (see arch/cc.h). For this compile milestone we
 * only emit the raw format string over the native console hook; varargs are not
 * expanded (no minimal printf is pulled in yet). This is sufficient because the
 * milestone exercises no code paths that emit formatted lwIP diagnostics.
 * TODO(net_stack): route through a real minimal vprintf once the netif step
 * needs formatted diagnostics. */
void net_stack_lwip_diag(const char* fmt, ...) {
    (void)0;
    if (g_api != NULL && g_api->console_write != NULL && fmt != NULL) {
        g_api->console_write(fmt, ns_strlen(fmt));
    }
    /* Consume varargs to keep the signature honest even though unused. */
    va_list ap;
    va_start(ap, fmt);
    va_end(ap);
}

static void net_stack_send_reply(const nd_ipc_message_t* request, uint32_t type, int32_t status,
                                 uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    nd_ipc_message_t reply;
    uint32_t context_id;
    if (g_api == NULL || request == NULL || request->source == 0u ||
        request->source == 0xFFFFFFFFu || g_api->ipc_send == NULL ||
        g_api->sched_current_pid == NULL) {
        return;
    }
    reply.type = type;
    reply.source = g_endpoint;
    reply.destination = request->source;
    reply.request_id = request->request_id;
    reply.arg0 = (uint32_t)status;
    reply.arg1 = arg1;
    reply.arg2 = arg2;
    reply.arg3 = arg3;
    context_id = g_api->sched_current_pid();
    (void)g_api->ipc_send(context_id, request->source, &reply);
}

static void net_stack_reply_error(const nd_ipc_message_t* request, int32_t status) {
    net_stack_send_reply(request, NET_IPC_ERROR, status, 0u, 0u, 0u);
}

static void net_stack_notify_rx(net_socket_t* socket) {
    nd_ipc_message_t notice;
    if (socket == NULL || socket->owner_endpoint == 0u || g_api == NULL ||
        g_api->ipc_send == NULL || g_api->sched_current_pid == NULL) {
        return;
    }
    notice.type = NET_IPC_RX_NOTIFY;
    notice.source = g_endpoint;
    notice.destination = socket->owner_endpoint;
    notice.request_id = 0u;
    notice.arg0 = (uint32_t)(socket - g_socket_pool.sockets);
    notice.arg1 = 0u;
    notice.arg2 = 0u;
    notice.arg3 = 0u;
    (void)g_api->ipc_send(g_api->sched_current_pid(), socket->owner_endpoint, &notice);
}

static void net_stack_udp_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p,
                               const ip_addr_t* addr, u16_t port) {
    net_socket_t* socket = (net_socket_t*)arg;
    uint8_t datagram[sizeof(net_udp_datagram_record_v1_t) + NET_STACK_UDP_DATAGRAM_BYTES];
    net_udp_datagram_record_v1_t* record = (net_udp_datagram_record_v1_t*)datagram;
    u16_t copied;
    u16_t len;
    (void)pcb;
    if (socket == NULL || p == NULL || p->tot_len > NET_STACK_UDP_DATAGRAM_BYTES) {
        if (socket != NULL) {
            wasmos_ringbuf_set_flags(&socket->rx_ring, WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED);
        }
        if (p != NULL) {
            pbuf_free(p);
        }
        return;
    }
    len = p->tot_len;
    record->version = NET_UDP_DATAGRAM_RECORD_VERSION;
    record->flags = 0u;
    record->addr_v4 = addr != NULL ? ip4_addr_get_u32(ip_2_ip4(addr)) : 0u;
    record->port = port;
    record->payload_bytes = len;
    copied = pbuf_copy_partial(p, datagram + sizeof(*record), len, 0u);
    pbuf_free(p);
    if (copied != len ||
        wasmos_ringbuf_write_record(&socket->rx_ring, datagram, copied + sizeof(*record)) < 0) {
        wasmos_ringbuf_set_flags(&socket->rx_ring, WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED);
        return;
    }
    net_stack_notify_rx(socket);
}

static void net_stack_drain_udp_tx(net_socket_t* socket) {
    uint8_t datagram[sizeof(net_udp_datagram_record_v1_t) + NET_STACK_UDP_DATAGRAM_BYTES];
    uint32_t len;
    int32_t got;
    ip_addr_t address;
    if (socket == NULL || socket->type != NET_SOCKET_DGRAM || socket->pcb == NULL ||
        (socket->state != NET_SOCKET_BOUND && socket->state != NET_SOCKET_CONNECTED)) {
        return;
    }
    for (;;) {
        got = wasmos_ringbuf_read_record(&socket->tx_ring, datagram, sizeof(datagram), &len);
        if (got == -1) {
            return;
        }
        if (got == -2) {
            /* Drop an over-MTU record so one bad client write cannot wedge
             * the socket's ring forever. */
            (void)wasmos_ringbuf_skip(&socket->tx_ring, len + 4u);
            wasmos_ringbuf_set_flags(&socket->tx_ring, WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED);
            continue;
        }
        net_udp_datagram_record_v1_t* record = (net_udp_datagram_record_v1_t*)datagram;
        uint32_t payload_bytes;
        uint32_t dest_addr;
        uint16_t dest_port;
        if ((uint32_t)got < sizeof(*record) || record->version != NET_UDP_DATAGRAM_RECORD_VERSION ||
            record->payload_bytes != (uint16_t)((uint32_t)got - sizeof(*record))) {
            wasmos_ringbuf_set_flags(&socket->tx_ring, WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED);
            continue;
        }
        payload_bytes = record->payload_bytes;
        dest_addr = (record->flags & NET_UDP_DATAGRAM_FLAG_DESTINATION) ? record->addr_v4
                                                                        : socket->remote_addr_v4;
        dest_port = (record->flags & NET_UDP_DATAGRAM_FLAG_DESTINATION) ? record->port
                                                                        : socket->remote_port;
        if (dest_addr == 0u || dest_port == 0u) {
            wasmos_ringbuf_set_flags(&socket->tx_ring, WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED);
            continue;
        }
        struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)payload_bytes, PBUF_RAM);
        if (p == NULL || pbuf_take(p, datagram + sizeof(*record), (u16_t)payload_bytes) != ERR_OK) {
            if (p != NULL) {
                pbuf_free(p);
            }
            wasmos_ringbuf_set_flags(&socket->tx_ring, WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED);
            return;
        }
        ip_addr_set_ip4_u32(&address, dest_addr);
        if (udp_sendto((struct udp_pcb*)socket->pcb, p, &address, dest_port) != ERR_OK) {
            wasmos_ringbuf_set_flags(&socket->tx_ring, WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED);
        }
        pbuf_free(p);
    }
}

/* Answer a request whose reply was deferred (TCP connect completing in a
 * callback). The client's reply endpoint is its owner_endpoint; the original
 * request id was stashed at connect time. */
static void net_stack_reply_deferred(net_socket_t* socket, uint32_t type, int32_t status) {
    nd_ipc_message_t reply;
    if (g_api == NULL || socket == NULL || socket->owner_endpoint == 0u ||
        g_api->ipc_send == NULL || g_api->sched_current_pid == NULL) {
        return;
    }
    reply.type = type;
    reply.source = g_endpoint;
    reply.destination = socket->owner_endpoint;
    reply.request_id = socket->connect_request_id;
    reply.arg0 = (uint32_t)status;
    reply.arg1 = 0u;
    reply.arg2 = 0u;
    reply.arg3 = 0u;
    (void)g_api->ipc_send(g_api->sched_current_pid(), socket->owner_endpoint, &reply);
}

/* Move queued TX bytes from a stream socket's ring into the TCP send buffer.
 * tcp_sndbuf() bounds each pass; a short send-buffer resumes from tcp_sent.
 * Bytes are peeked and only consumed once tcp_write accepts them, so nothing
 * is lost when the stack is momentarily full. */
static void net_stack_drain_tcp_tx(net_socket_t* socket) {
    uint8_t chunk[NET_STACK_TCP_CHUNK_BYTES];
    struct tcp_pcb* pcb;
    int wrote_any = 0;
    if (socket == NULL || socket->type != NET_SOCKET_STREAM || socket->pcb == NULL ||
        socket->state != NET_SOCKET_CONNECTED) {
        return;
    }
    pcb = (struct tcp_pcb*)socket->pcb;
    for (;;) {
        uint32_t sndbuf = tcp_sndbuf(pcb);
        uint32_t used = wasmos_ringbuf_used(&socket->tx_ring);
        uint32_t want = sizeof(chunk);
        uint32_t got;
        err_t err;
        if (sndbuf == 0u || used == 0u) {
            break;
        }
        if (want > sndbuf) {
            want = sndbuf;
        }
        if (want > used) {
            want = used;
        }
        got = wasmos_ringbuf_peek(&socket->tx_ring, chunk, want);
        if (got == 0u) {
            break;
        }
        err = tcp_write(pcb, chunk, (u16_t)got, TCP_WRITE_FLAG_COPY);
        if (err == ERR_MEM) {
            /* No room in the send queue right now; retry from tcp_sent. */
            break;
        }
        if (err != ERR_OK) {
            wasmos_ringbuf_set_flags(&socket->tx_ring, WASMOS_RINGBUF_FLAG_RESET);
            break;
        }
        (void)wasmos_ringbuf_skip(&socket->tx_ring, got);
        wrote_any = 1;
    }
    if (wrote_any) {
        (void)tcp_output(pcb);
    }
}

/* SYN handshake completed: the socket is now writable. Answer the deferred
 * connect and flush anything the client queued while connecting. */
static err_t net_stack_tcp_connected(void* arg, struct tcp_pcb* pcb, err_t err) {
    net_socket_t* socket = (net_socket_t*)arg;
    (void)pcb;
    if (socket == NULL) {
        return ERR_OK;
    }
    if (err != ERR_OK) {
        /* tcp_err follows on a failed connect; let it own the reply/teardown. */
        return err;
    }
    socket->state = NET_SOCKET_CONNECTED;
    if (socket->connect_pending) {
        socket->connect_pending = 0u;
        net_stack_reply_deferred(socket, NET_IPC_RESP, NET_STATUS_OK);
    }
    net_stack_drain_tcp_tx(socket);
    return ERR_OK;
}

/* Inbound stream bytes (or a FIN when p == NULL). Copy the whole segment into
 * the client RX ring and acknowledge it; if the ring cannot hold it, refuse
 * with ERR_MEM so lwIP retains the data and redelivers (TCP flow control). */
static err_t net_stack_tcp_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    net_socket_t* socket = (net_socket_t*)arg;
    struct pbuf* q;
    if (socket == NULL) {
        if (p != NULL) {
            pbuf_free(p);
        }
        return ERR_OK;
    }
    if (err != ERR_OK) {
        if (p != NULL) {
            pbuf_free(p);
        }
        return err;
    }
    if (p == NULL) {
        /* Peer half-closed: surface EOF to the client and notify it. */
        wasmos_ringbuf_set_flags(&socket->rx_ring, WASMOS_RINGBUF_FLAG_PEER_CLOSED);
        net_stack_notify_rx(socket);
        return ERR_OK;
    }
    if (wasmos_ringbuf_free(&socket->rx_ring) < p->tot_len) {
        /* Backpressure: keep the data in lwIP and let it retry. */
        return ERR_MEM;
    }
    for (q = p; q != NULL; q = q->next) {
        (void)wasmos_ringbuf_write(&socket->rx_ring, q->payload, q->len);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    net_stack_notify_rx(socket);
    return ERR_OK;
}

/* Peer acknowledged sent bytes: send-buffer space freed, resume draining TX. */
static err_t net_stack_tcp_sent(void* arg, struct tcp_pcb* pcb, u16_t len) {
    net_socket_t* socket = (net_socket_t*)arg;
    (void)pcb;
    (void)len;
    net_stack_drain_tcp_tx(socket);
    return ERR_OK;
}

/* Fatal error: lwIP has already freed the pcb. Drop our reference, mark the
 * rings reset, answer any deferred connect, and wake the client. */
static void net_stack_tcp_err(void* arg, err_t err) {
    net_socket_t* socket = (net_socket_t*)arg;
    (void)err;
    if (socket == NULL) {
        return;
    }
    socket->pcb = NULL;
    wasmos_ringbuf_set_flags(&socket->tx_ring, WASMOS_RINGBUF_FLAG_RESET);
    wasmos_ringbuf_set_flags(&socket->rx_ring,
                             WASMOS_RINGBUF_FLAG_RESET | WASMOS_RINGBUF_FLAG_PEER_CLOSED);
    if (socket->connect_pending) {
        socket->connect_pending = 0u;
        net_stack_reply_deferred(socket, NET_IPC_ERROR, NET_STATUS_IO_ERROR);
    }
    socket->state = NET_SOCKET_CLOSING;
    net_stack_notify_rx(socket);
}

/* Find the earliest accept slot posted for a given listener (a socket that has
 * client rings but no connection yet). Returns NULL when none is waiting. */
static net_socket_t* net_stack_find_accept_slot(uint32_t listener_id) {
    for (uint32_t id = 0; id < NET_SOCKET_MAX; ++id) {
        net_socket_t* socket = &g_socket_pool.sockets[id];
        if (socket->state == NET_SOCKET_ACCEPTING && socket->connect_pending &&
            socket->accept_listener_id == listener_id) {
            return socket;
        }
    }
    return NULL;
}

static net_interface_slot_t* net_stack_interface_from_netif(struct netif* netif) {
    if (netif == NULL) {
        return NULL;
    }
    for (uint32_t i = 0u; i < NET_STACK_MAX_INTERFACES; ++i) {
        if (g_interfaces[i].in_use && &g_interfaces[i].netif == netif) {
            return &g_interfaces[i];
        }
    }
    return NULL;
}

static net_interface_slot_t* net_stack_interface_from_endpoint(uint32_t endpoint) {
    return net_stack_interface_slot(endpoint, 0u, 0u);
}

static net_interface_slot_t* net_stack_interface_from_index(uint32_t index) {
    if (index >= NET_STACK_MAX_INTERFACES || !g_interfaces[index].in_use) {
        return NULL;
    }
    return &g_interfaces[index];
}

/* An inbound connection completed its handshake on a listening socket. Pair it
 * with a posted accept slot (its client-owned rings) and answer the deferred
 * NET_IPC_ACCEPT with the new socket id. With no slot posted, reject the
 * connection (ERR_MEM) so lwIP aborts it; the peer can retry once a slot is
 * available. */
static err_t net_stack_tcp_accept(void* arg, struct tcp_pcb* newpcb, err_t err) {
    net_socket_t* listener = (net_socket_t*)arg;
    net_socket_t* slot;
    uint32_t listener_id;
    if (listener == NULL || newpcb == NULL || err != ERR_OK) {
        return ERR_VAL;
    }
    listener_id = (uint32_t)(listener - g_socket_pool.sockets);
    slot = net_stack_find_accept_slot(listener_id);
    if (slot == NULL) {
        return ERR_MEM;
    }
    slot->pcb = newpcb;
    slot->state = NET_SOCKET_CONNECTED;
    slot->remote_port = newpcb->remote_port;
    slot->remote_addr_v4 = ip4_addr_get_u32(ip_2_ip4(&newpcb->remote_ip));
    slot->local_port = newpcb->local_port;
    tcp_arg(newpcb, slot);
    tcp_recv(newpcb, net_stack_tcp_recv);
    tcp_sent(newpcb, net_stack_tcp_sent);
    tcp_err(newpcb, net_stack_tcp_err);
    /* Tell lwIP this backlog slot is consumed so it can accept the next one. */
    tcp_accepted((struct tcp_pcb*)listener->pcb);
    slot->connect_pending = 0u;
    net_stack_reply_deferred(slot, NET_IPC_RESP, (int32_t)(slot - g_socket_pool.sockets));
    /* A fast peer may already have data queued in the pcb. */
    net_stack_drain_tcp_tx(slot);
    return ERR_OK;
}

static err_t net_stack_netif_init(struct netif* netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = net_stack_linkoutput;
    netif->mtu = 1500u;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    return ERR_OK;
}

static err_t net_stack_linkoutput(struct netif* netif, struct pbuf* p) {
    net_interface_slot_t* interface = net_stack_interface_from_netif(netif);
    nd_ipc_message_t request;
    uint16_t copied = 0u;
    uint32_t request_id;
    net_tx_slot_t* slot = NULL;
    struct pbuf* q;
    if (p == NULL || p->tot_len > NET_STACK_FRAME_BYTES || interface == NULL ||
        interface->endpoint == 0u) {
        return ERR_BUF;
    }
    for (uint32_t i = 0u; i < NET_STACK_TX_QUEUE_DEPTH; ++i) {
        if (!interface->tx_slots[i].pending && interface->tx_slots[i].buffer != NULL) {
            slot = &interface->tx_slots[i];
            break;
        }
    }
    if (slot == NULL)
        return ERR_MEM;
    for (q = p; q != NULL; q = q->next) {
        uint8_t* src = (uint8_t*)q->payload;
        uint16_t i;
        for (i = 0u; i < q->len; ++i) {
            slot->buffer[copied + i] = src[i];
        }
        copied = (uint16_t)(copied + q->len);
    }
    request_id = NET_STACK_TX_REQUEST_BASE + (uint32_t)(slot - interface->tx_slots);
    request.type = NETDRV_IPC_TX_FRAME;
    request.source = g_netdrv_reply_endpoint;
    request.destination = interface->endpoint;
    request.request_id = request_id;
    request.arg0 = copied;
    request.arg1 = slot->buffer_id;
    request.arg2 = 0u;
    request.arg3 = 0u;
    if (g_api->ipc_send(g_api->sched_current_pid(), interface->endpoint, &request) != 0) {
        return ERR_IF;
    }
    slot->pending = 1u;
    return ERR_OK;
}

static void net_stack_deliver_rx(net_interface_slot_t* interface, uint32_t len) {
    struct pbuf* p;
    if (interface == NULL || len == 0u || len > NET_STACK_FRAME_BYTES) {
        return;
    }
    p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (p == NULL) {
        return;
    }
    if (pbuf_take(p, interface->rx_buffer, (u16_t)len) != ERR_OK) {
        pbuf_free(p);
        return;
    }
    if (len >= 14u && interface->rx_buffer[12] == 0x08u && interface->rx_buffer[13] == 0x06u &&
        g_api->console_write != NULL) {
        static const char msg[] = "[net-stack] arp rx\n";
        g_api->console_write(msg, (int)(sizeof(msg) - 1));
    }
    (void)ethernet_input(p, &interface->netif);
}

static void net_stack_start_rx_poll(net_interface_slot_t* interface, uint8_t immediate) {
    nd_ipc_message_t request;
    uint32_t now;
    if (interface == NULL || interface->rx_pending || interface->endpoint == 0u ||
        interface->rx_buffer_id == 0u) {
        return;
    }
    now = g_api->sched_ticks != NULL ? g_api->sched_ticks() : 0u;
    if (!immediate && g_api->sched_ticks != NULL &&
        (int32_t)(now - interface->rx_poll_next_tick) < 0) {
        return;
    }
    request.type = NETDRV_IPC_RX_POLL;
    request.source = g_netdrv_reply_endpoint;
    request.destination = interface->endpoint;
    request.request_id = NET_STACK_RX_POLL_REQUEST_ID;
    request.arg0 = interface->rx_buffer_id;
    request.arg1 = 0u;
    request.arg2 = 0u;
    request.arg3 = 0u;
    if (g_api->ipc_send(g_api->sched_current_pid(), interface->endpoint, &request) == 0) {
        interface->rx_pending = 1u;
        interface->rx_poll_next_tick = now + NET_STACK_RX_POLL_INTERVAL_TICKS;
    }
}

static void net_stack_finish_bind(net_interface_slot_t* interface, uint8_t link_up) {
    if (interface == NULL) {
        return;
    }
    if (!interface->netif_installed) {
        ip4_addr_t zero;
        IP4_ADDR(&zero, 0, 0, 0, 0);
        /* Address is assigned later from /boot/system/net/interfaces (static)
         * or via DHCP; bring the netif up unconfigured for now so DHCP has an
         * up, link-up interface to exchange DISCOVER/OFFER on. */
        if (netif_add(&interface->netif, &zero, &zero, &zero, NULL, net_stack_netif_init,
                      ethernet_input) == NULL) {
            return;
        }
        for (uint32_t i = 0u; i < ETH_HWADDR_LEN; ++i) {
            interface->netif.hwaddr[i] = interface->rx_buffer[i];
        }
        if (g_active_ifc == interface)
            netif_set_default(&interface->netif);
        netif_set_status_callback(&interface->netif, net_stack_netif_status_cb);
        interface->netif_installed = 1u;
    }
    netif_set_up(&interface->netif);
    if (link_up)
        netif_set_link_up(&interface->netif);
    else
        netif_set_link_down(&interface->netif);
    interface->netif_ready = 1u;
    interface->state = NET_IFC_NETIF_UP;
    interface->link_up = link_up;
    /* Read the interface config only when the interface comes up; the "ready"
     * banner and gateway ARP are emitted from the status callback once an
     * address is actually assigned (static apply or DHCP bind). */
    if (link_up && g_active_ifc == interface)
        net_stack_kick_ifcfg_load();
}

/* Format a network-order IPv4 word as "a.b.c.d" into dst; returns the length. */
static int net_stack_fmt_ipv4(uint32_t addr_no, char* dst, int cap) {
    int len = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t octet = (addr_no >> (i * 8)) & 0xFFu;
        if (i != 0 && len < cap)
            dst[len++] = '.';
        if (octet >= 100u && len < cap)
            dst[len++] = (char)('0' + (octet / 100u));
        if (octet >= 10u && len < cap)
            dst[len++] = (char)('0' + ((octet / 10u) % 10u));
        if (len < cap)
            dst[len++] = (char)('0' + (octet % 10u));
    }
    return len;
}

/* Count set bits in a (contiguous) netmask to derive the prefix length. */
static int net_stack_mask_prefix(uint32_t mask_no) {
    int count = 0;
    for (int i = 0; i < 32; ++i) {
        if ((mask_no >> i) & 1u)
            count++;
    }
    return count;
}

/* lwIP netif status callback: fires whenever the netif state changes. When a
 * real address first appears (static apply or DHCP bind), emit the ready banner
 * and prime ARP for the gateway. */
static void net_stack_netif_status_cb(struct netif* netif) {
    net_interface_slot_t* interface = net_stack_interface_from_netif(netif);
    uint32_t addr_no;
    if (netif == NULL || interface == NULL) {
        return;
    }
    addr_no = ip4_addr_get_u32(ip_2_ip4(&netif->ip_addr));
    if (addr_no == 0u || interface->addr_ready) {
        return;
    }
    interface->addr_ready = 1u;
    if (g_api->console_write != NULL) {
        char line[64];
        int n = 0;
        int prefix;
        static const char pfx[] = "[net-stack] eth0 ";
        static const char sfx[] = " ready\n";
        for (uint32_t i = 0u; i < sizeof(pfx) - 1u && n < (int)sizeof(line); ++i)
            line[n++] = pfx[i];
        n += net_stack_fmt_ipv4(addr_no, line + n, (int)sizeof(line) - n);
        if (n < (int)sizeof(line))
            line[n++] = '/';
        prefix = net_stack_mask_prefix(ip4_addr_get_u32(ip_2_ip4(&netif->netmask)));
        if (prefix >= 10 && n < (int)sizeof(line))
            line[n++] = (char)('0' + prefix / 10);
        if (n < (int)sizeof(line))
            line[n++] = (char)('0' + prefix % 10);
        for (uint32_t i = 0u; i < sizeof(sfx) - 1u && n < (int)sizeof(line); ++i)
            line[n++] = sfx[i];
        g_api->console_write(line, n);
    }
    if (ip4_addr_get_u32(ip_2_ip4(&netif->gw)) != 0u) {
        (void)etharp_request(netif, ip_2_ip4(&netif->gw));
    }
}

static void net_stack_apply_ifcfg(net_interface_slot_t* interface, const net_ifcfg_t* cfg) {
    if (cfg == NULL || interface == NULL || !interface->netif_installed) {
        return;
    }
    if (cfg->dhcp) {
        if (!interface->dhcp_active && dhcp_start(&interface->netif) == ERR_OK) {
            interface->dhcp_active = 1u;
            interface->dhcp_started_tick = g_api->sched_ticks != NULL ? g_api->sched_ticks() : 0u;
        }
        if (g_api->console_write != NULL) {
            static const char msg[] = "[net-stack] dhcp: requesting lease\n";
            g_api->console_write(msg, (int)(sizeof(msg) - 1));
        }
        return;
    }
    ip4_addr_t ip;
    ip4_addr_t mask;
    ip4_addr_t gw;
    IP4_ADDR(&ip, cfg->addr[0], cfg->addr[1], cfg->addr[2], cfg->addr[3]);
    IP4_ADDR(&mask, cfg->mask[0], cfg->mask[1], cfg->mask[2], cfg->mask[3]);
    IP4_ADDR(&gw, cfg->gw[0], cfg->gw[1], cfg->gw[2], cfg->gw[3]);
    netif_set_addr(&interface->netif, &ip, &mask, &gw); /* fires status cb -> banner + ARP */
}

static void net_stack_kick_ifcfg_load(void) {
    if (!g_ifcfg_loaded)
        g_ifcfg_kicked = 1u;
}

static void net_stack_fs_lookup_reply(void* user, const nd_ipc_message_t* reply) {
    (void)user;
    g_fs_lookup_pending = 0u;
    if (reply != NULL && reply->type == SVC_IPC_LOOKUP_RESP && reply->arg0 != 0xFFFFFFFFu) {
        g_fs_endpoint = (int32_t)reply->arg0; /* read starts on the next tick */
        return;
    }
    if (g_fs_lookup_stage == 0u) {
        g_fs_lookup_stage = 1u; /* fall back to legacy "fs" name */
    } else {
        g_fs_lookup_stage = 0u;
        g_ifcfg_attempts++;
        if (g_ifcfg_attempts >= NET_STACK_IFCFG_MAX_ATTEMPTS) {
            g_ifcfg_loaded = 1u; /* strict: give up unconfigured */
            if (g_api->console_write != NULL) {
                static const char msg[] = "[net-stack] no interface config (fs unavailable)\n";
                g_api->console_write(msg, (int)(sizeof(msg) - 1));
            }
        }
    }
    g_ifcfg_next_tick =
        (g_api->sched_ticks != NULL ? g_api->sched_ticks() : 0u) + NET_STACK_IFCFG_RETRY_TICKS;
}

static void net_stack_ifcfg_read_reply(void* user, const nd_ipc_message_t* reply) {
    (void)user;
    g_ifcfg_read_pending = 0u;
    if (reply != NULL && reply->type == FS_IPC_RESP && (int32_t)reply->arg0 >= 0 &&
        g_ifcfg_buffer != NULL) {
        uint32_t n = reply->arg0;
        net_ifcfg_t cfg;
        if (n >= NET_STACK_IFCFG_CAP)
            n = NET_STACK_IFCFG_CAP - 1u;
        if (net_ifcfg_parse((const char*)g_ifcfg_buffer, n, &cfg)) {
            /* FIXME(multinet-ifcfg): the current file format has no interface
             * selector, so boot configuration deliberately targets default. */
            net_stack_apply_ifcfg(g_active_ifc, &cfg);
        } else if (g_api->console_write != NULL) {
            static const char msg[] = "[net-stack] interface config invalid\n";
            g_api->console_write(msg, (int)(sizeof(msg) - 1));
        }
        g_ifcfg_loaded = 1u; /* strict: stop retrying once the file was read */
    } else {
        /* Read error: fs.vfs is up but /boot may not be mounted yet. Retry a
         * bounded number of times, then give up (strict: unconfigured). */
        g_ifcfg_attempts++;
        if (g_ifcfg_attempts >= NET_STACK_IFCFG_MAX_ATTEMPTS) {
            g_ifcfg_loaded = 1u;
            if (g_api->console_write != NULL) {
                static const char msg[] = "[net-stack] no interface config\n";
                g_api->console_write(msg, (int)(sizeof(msg) - 1));
            }
        }
        g_ifcfg_next_tick =
            (g_api->sched_ticks != NULL ? g_api->sched_ticks() : 0u) + NET_STACK_IFCFG_RETRY_TICKS;
    }
    if (g_ifcfg_buffer_id != 0u)
        (void)g_api->xfer_buffer_release(g_ifcfg_buffer_id);
    g_ifcfg_buffer_id = 0u;
    g_ifcfg_buffer = NULL;
}

static void net_stack_try_load_ifcfg(void) {
    uint32_t now;
    uint32_t args[4];
    if (!g_ifcfg_kicked || g_ifcfg_loaded || g_ifcfg_read_pending || g_fs_lookup_pending) {
        return;
    }
    now = g_api->sched_ticks != NULL ? g_api->sched_ticks() : 0u;
    if (g_api->sched_ticks != NULL && (int32_t)(now - g_ifcfg_next_tick) < 0) {
        return;
    }
    if (g_fs_endpoint == 0) {
        const char* name = g_fs_lookup_stage == 0u ? "fs.vfs" : "fs";
        uint32_t name_len = g_fs_lookup_stage == 0u ? 6u : 2u;
        wasmos_sys_ipc_pack_name16_native((const uint8_t*)name, name_len, args);
        if (wasmos_sys_native_intent_send(&g_control_loop, g_proc_endpoint, g_control_endpoint,
                                          SVC_IPC_LOOKUP_REQ, args[0], args[1], args[2], args[3],
                                          net_stack_fs_lookup_reply, NULL, NULL) == 0)
            g_fs_lookup_pending = 1u;
        return;
    }
    /* fs endpoint known: owner-push read of the interfaces file. */
    g_ifcfg_buffer = (uint8_t*)g_api->xfer_buffer_acquire(ND_BUFFER_KIND_XFER, NET_STACK_IFCFG_CAP,
                                                          &g_ifcfg_buffer_id);
    if (g_ifcfg_buffer == NULL) {
        g_ifcfg_next_tick = now + NET_STACK_IFCFG_RETRY_TICKS;
        return;
    }
    static const char path[] = NET_STACK_IFCFG_PATH;
    uint32_t path_len = (uint32_t)(sizeof(path) - 1u);
    int32_t borrow_id;
    __builtin_memcpy(g_ifcfg_buffer, path, path_len);
    borrow_id = g_api->xfer_buffer_borrow((uint32_t)g_fs_endpoint, g_ifcfg_buffer_id,
                                          ND_BUFFER_BORROW_READ | ND_BUFFER_BORROW_WRITE);
    if (borrow_id < 0 || wasmos_sys_native_intent_send(
                             &g_control_loop, (uint32_t)g_fs_endpoint, g_control_endpoint,
                             FS_IPC_READ_PATH_REQ, path_len, NET_STACK_IFCFG_CAP, g_ifcfg_buffer_id,
                             (uint32_t)borrow_id, net_stack_ifcfg_read_reply, NULL, NULL) != 0) {
        (void)g_api->xfer_buffer_release(g_ifcfg_buffer_id);
        g_ifcfg_buffer_id = 0u;
        g_ifcfg_buffer = NULL;
        g_ifcfg_next_tick = now + NET_STACK_IFCFG_RETRY_TICKS;
        return;
    }
    g_ifcfg_read_pending = 1u;
}

static int32_t net_stack_lookup_reply_status(void* user, const nd_ipc_message_t* reply) {
    (void)user;
    if (reply == NULL || reply->type != SVC_IPC_LOOKUP_RESP || reply->arg0 == 0xFFFFFFFFu) {
        return -1;
    }
    return 0;
}

static int32_t net_stack_link_get_reply_status(void* user, const nd_ipc_message_t* reply) {
    (void)user;
    return reply != NULL && reply->type == NETDRV_IPC_RESP ? 0 : -1;
}

static void net_stack_lookup_coroutine(void* arg) {
    uintptr_t value = 0u;
    nd_ipc_message_t* reply;

    (void)arg;
    if (wasmos_future_await(&g_netdrv_lookup_future.future, &value) != 0 || value == 0u) {
        g_netdrv_lookup_pending = 0u;
        return;
    }
    reply = (nd_ipc_message_t*)value;
    g_netdrv_endpoint = reply->arg0;
    if (g_active_ifc == NULL) {
        g_active_ifc = net_stack_interface_slot(reply->arg0, 0u, 1u);
    }
    g_netdrv_lookup_pending = 0u;
}

static void net_stack_link_get_coroutine(void* arg) {
    net_interface_slot_t* slot = (net_interface_slot_t*)arg;
    uintptr_t value = 0u;
    nd_ipc_message_t* reply;
    if (slot == NULL || wasmos_future_await(&slot->link_get_future.future, &value) != 0 ||
        value == 0u) {
        if (slot != NULL)
            slot->link_get_pending = 0u;
        return;
    }
    reply = (nd_ipc_message_t*)value;
    slot->link_get_pending = 0u;
    net_stack_finish_bind(slot, reply->arg0 != 0u);
}

static void net_stack_start_lookup_coroutine(void) {
    uint32_t args[4];

    if (g_netdrv_lookup_pending) {
        return;
    }
    wasmos_sys_ipc_pack_name16_native((const uint8_t*)"virtio.net", 10u, args);
    wasmos_sys_native_ipc_future_init(&g_netdrv_lookup_future, net_stack_lookup_reply_status, NULL);
    if (!wasmos_sys_native_ipc_future_send(&g_control_loop, &g_netdrv_lookup_future,
                                           g_proc_endpoint, g_control_endpoint, SVC_IPC_LOOKUP_REQ,
                                           args[0], args[1], args[2], args[3], NULL)) {
        return;
    }
    g_netdrv_lookup_pending = 1u;
    if (!g_control_runtime ||
        !wasmos_async_start(g_control_runtime, &g_netdrv_lookup_coroutine, g_netdrv_lookup_stack,
                            sizeof(g_netdrv_lookup_stack), net_stack_lookup_coroutine, NULL)) {
        wasmos_sys_native_ipc_future_cancel(&g_netdrv_lookup_future, -1);
        g_netdrv_lookup_pending = 0u;
    }
}

static void net_stack_class_lookup_reply(void* user, const nd_ipc_message_t* reply) {
    svc_class_entry_t entry;
    (void)user;
    g_netifc_lookup_pending = 0u;
    if (reply != NULL && reply->type == SVC_IPC_LOOKUP_CLASS_RESP && reply->arg0 != 0u &&
        g_netifc_lookup_buffer != NULL) {
        __builtin_memcpy(&entry, g_netifc_lookup_buffer, sizeof(entry));
        if (entry.endpoint != 0u) {
            net_interface_slot_t* slot =
                net_stack_interface_slot(entry.endpoint, entry.instance, 1u);
            if (g_active_ifc == NULL)
                g_active_ifc = slot;
            if (g_active_ifc == slot)
                g_netdrv_endpoint = entry.endpoint;
        }
    }
    if (g_netifc_lookup_buffer_id != 0u)
        (void)g_api->xfer_buffer_release(g_netifc_lookup_buffer_id);
    g_netifc_lookup_buffer_id = 0u;
    g_netifc_lookup_buffer = NULL;
}

static void net_stack_subscribe_reply(void* user, const nd_ipc_message_t* reply) {
    (void)user;
    g_netifc_subscribe_pending = 0u;
    if (reply != NULL && reply->type == SVC_IPC_SUBSCRIBE_CLASS_RESP)
        g_netifc_subscribed = 1u;
    if (g_netifc_subscribe_buffer_id != 0u)
        (void)g_api->xfer_buffer_release(g_netifc_subscribe_buffer_id);
    g_netifc_subscribe_buffer_id = 0u;
    g_netifc_subscribe_buffer = NULL;
    (void)reply;
}

static void net_stack_try_discover_interfaces(void) {
    if (!g_netifc_subscribed && !g_netifc_subscribe_pending && g_netifc_subscribe_buffer_id == 0u) {
        g_netifc_subscribe_buffer = (uint8_t*)g_api->xfer_buffer_acquire(
            ND_BUFFER_KIND_XFER, WASMOS_SVC_CLASS_MAX, &g_netifc_subscribe_buffer_id);
        if (g_netifc_subscribe_buffer != NULL) {
            __builtin_memcpy(g_netifc_subscribe_buffer, "net.ifc", 8u);
            if (wasmos_sys_native_intent_send(&g_control_loop, g_proc_endpoint, g_control_endpoint,
                                              SVC_IPC_SUBSCRIBE_CLASS_REQ, g_endpoint,
                                              g_netifc_subscribe_buffer_id, 0u, 0u,
                                              net_stack_subscribe_reply, NULL, NULL) == 0)
                g_netifc_subscribe_pending = 1u;
        }
    }
    if (g_netdrv_endpoint != 0u || g_netifc_lookup_pending || g_netifc_lookup_buffer_id != 0u)
        return;
    g_netifc_lookup_buffer = (uint8_t*)g_api->xfer_buffer_acquire(
        ND_BUFFER_KIND_XFER, sizeof(svc_class_entry_t), &g_netifc_lookup_buffer_id);
    if (g_netifc_lookup_buffer == NULL)
        return;
    __builtin_memcpy(g_netifc_lookup_buffer, "net.ifc", 8u);
    if (wasmos_sys_native_intent_send(&g_control_loop, g_proc_endpoint, g_control_endpoint,
                                      SVC_IPC_LOOKUP_CLASS_REQ, g_netifc_lookup_buffer_id, 1u, 0u,
                                      0u, net_stack_class_lookup_reply, NULL, NULL) == 0)
        g_netifc_lookup_pending = 1u;
}

static void net_stack_reap_interfaces(void) {
    for (uint32_t i = 0u; i < NET_STACK_MAX_INTERFACES; ++i) {
        net_interface_slot_t* slot = &g_interfaces[i];
        if (slot->in_use && slot->link_get_retire &&
            slot->link_get_coroutine.state == WASMOS_NATIVE_COROUTINE_DEAD) {
            __builtin_memset(slot, 0, sizeof(*slot));
        }
    }
}

static void net_stack_unbind_interface(uint32_t endpoint) {
    net_interface_slot_t* slot = net_stack_interface_from_endpoint(endpoint);
    uint8_t was_active;

    if (slot == NULL)
        return;
    was_active = slot == g_active_ifc;
    if (slot->netif_ready)
        netif_set_down(&slot->netif);
    slot->netif_ready = 0u;
    wasmos_sys_native_ipc_future_cancel(&slot->link_get_future, -1);
    slot->rx_pending = 0u;
    slot->rx_poll_next_tick = 0u;
    if (slot->rx_buffer_id != 0u)
        (void)g_api->xfer_buffer_release(slot->rx_buffer_id);
    slot->rx_buffer_id = 0u;
    slot->rx_buffer = NULL;
    for (uint32_t i = 0u; i < NET_STACK_TX_QUEUE_DEPTH; ++i) {
        if (slot->tx_slots[i].buffer_id != 0u)
            (void)g_api->xfer_buffer_release(slot->tx_slots[i].buffer_id);
        slot->tx_slots[i].buffer_id = 0u;
        slot->tx_slots[i].buffer = NULL;
        slot->tx_slots[i].pending = 0u;
    }
    slot->state = NET_IFC_DISCOVERED;
    if (was_active) {
        /* Force a fresh active-interface config read on replacement. */
        g_ifcfg_kicked = 0u;
        g_ifcfg_loaded = 0u;
        g_netdrv_endpoint = 0u;
        g_active_ifc = NULL;
    }
    if (slot->link_get_coroutine.state == WASMOS_NATIVE_COROUTINE_NEW ||
        slot->link_get_coroutine.state == WASMOS_NATIVE_COROUTINE_DEAD) {
        __builtin_memset(slot, 0, sizeof(*slot));
    } else {
        slot->link_get_retire = 1u;
    }
}

static void net_stack_registered(void) {
    g_register_pending = 0u;
    g_registered = 1u;
    if (g_api->console_write != NULL) {
        static const char msg[] = "[net-stack] registered net.stack\n";
        g_api->console_write(msg, (int)(sizeof(msg) - 1));
    }
}

static void net_stack_hrng_seed_complete(void* user, int32_t status) {
    (void)user;
    if (status == WASMOS_SYS_RANDOM_STATUS_OK) {
        lwip_port_seed(g_hrng_word);
        g_hrng_seeded = 1u;
    }
}

static void net_stack_hrng_lookup_reply(void* user, const nd_ipc_message_t* reply) {
    svc_class_entry_t entry;
    (void)user;
    g_hrng_lookup_pending = 0u;
    if (reply == NULL || reply->type != SVC_IPC_LOOKUP_CLASS_RESP || reply->arg0 == 0u ||
        g_hrng_lookup_buffer_id == 0u || g_hrng_lookup_buffer == NULL) {
        goto out;
    }
    __builtin_memcpy(&entry, g_hrng_lookup_buffer, sizeof(entry));
    if (entry.endpoint != 0u) {
        (void)wasmos_sys_native_random_int_async(&g_control_loop, entry.endpoint, &g_hrng_word,
                                                 &g_hrng_request, net_stack_hrng_seed_complete,
                                                 NULL);
    }
out:
    if (g_hrng_lookup_buffer_id != 0u) {
        (void)g_api->xfer_buffer_release(g_hrng_lookup_buffer_id);
        g_hrng_lookup_buffer_id = 0u;
    }
    g_hrng_lookup_buffer = NULL;
}

static void net_stack_begin_registration(void) {
    uint32_t args[4];
    nd_ipc_message_t msg;
    if (g_registered || g_register_pending || g_api->ipc_send == NULL ||
        g_api->sched_current_pid == NULL) {
        return;
    }
    /* Register the PUBLIC endpoint (g_endpoint) so client socket/ifaddr requests
     * land on the endpoint net_stack_dispatch drains. A plain SVC register uses
     * msg.source as the registered endpoint, so it must be sent from g_endpoint
     * (not the control endpoint); the REGISTER_RESP then arrives on g_endpoint
     * and is handled in net_stack_dispatch. */
    wasmos_sys_ipc_pack_name16_native((const uint8_t*)"net.stack", 9u, args);
    msg.type = SVC_IPC_REGISTER_REQ;
    msg.source = g_endpoint;
    msg.destination = g_proc_endpoint;
    msg.request_id = NET_STACK_REGISTER_REQUEST_ID;
    msg.arg0 = args[0];
    msg.arg1 = args[1];
    msg.arg2 = args[2];
    msg.arg3 = args[3];
    if (g_api->ipc_send(g_api->sched_current_pid(), g_proc_endpoint, &msg) == 0) {
        g_register_pending = 1u;
    }
}

static void net_stack_try_seed_random(void) {
    if (g_hrng_seeded || g_hrng_lookup_pending || g_hrng_request.buffer_id != 0u) {
        return;
    }
    g_hrng_lookup_buffer = (uint8_t*)g_api->xfer_buffer_acquire(
        ND_BUFFER_KIND_XFER, WASMOS_SVC_CLASS_MAX, &g_hrng_lookup_buffer_id);
    if (g_hrng_lookup_buffer == NULL || g_hrng_lookup_buffer_id == 0u) {
        return;
    }
    g_hrng_lookup_buffer[0] = 'h';
    g_hrng_lookup_buffer[1] = 'r';
    g_hrng_lookup_buffer[2] = 'n';
    g_hrng_lookup_buffer[3] = 'g';
    g_hrng_lookup_buffer[4] = '\0';
    if (wasmos_sys_native_intent_send(&g_control_loop, g_proc_endpoint, g_control_endpoint,
                                      SVC_IPC_LOOKUP_CLASS_REQ, g_hrng_lookup_buffer_id, 1u, 0u, 0u,
                                      net_stack_hrng_lookup_reply, NULL, NULL) != 0) {
        (void)g_api->xfer_buffer_release(g_hrng_lookup_buffer_id);
        g_hrng_lookup_buffer_id = 0u;
        g_hrng_lookup_buffer = NULL;
        return;
    }
    g_hrng_lookup_pending = 1u;
}

static void net_stack_try_bind_interface(net_interface_slot_t* slot) {
    if (slot != NULL && slot->netif_ready) {
        return;
    }
    if (slot == NULL || slot->endpoint == 0u || slot->link_get_pending) {
        return;
    }
    /* Cancellation wakes the awaiting coroutine on the next runtime turn.
     * Do not reinitialize its caller-owned future/stack until that turn has
     * retired the old operation. */
    if (slot->link_get_coroutine.state != WASMOS_NATIVE_COROUTINE_NEW &&
        slot->link_get_coroutine.state != WASMOS_NATIVE_COROUTINE_DEAD) {
        return;
    }
    if (slot->rx_buffer == NULL) {
        slot->rx_buffer = (uint8_t*)g_api->xfer_buffer_acquire(
            ND_BUFFER_KIND_XFER, NET_STACK_FRAME_BYTES, &slot->rx_buffer_id);
        if (slot->rx_buffer == NULL ||
            g_api->xfer_buffer_borrow(slot->endpoint, slot->rx_buffer_id,
                                      ND_BUFFER_BORROW_READ | ND_BUFFER_BORROW_WRITE) < 0) {
            return;
        }
        for (uint32_t i = 0u; i < NET_STACK_TX_QUEUE_DEPTH; ++i) {
            slot->tx_slots[i].buffer = (uint8_t*)g_api->xfer_buffer_acquire(
                ND_BUFFER_KIND_XFER, NET_STACK_FRAME_BYTES, &slot->tx_slots[i].buffer_id);
            if (slot->tx_slots[i].buffer == NULL ||
                g_api->xfer_buffer_borrow(slot->endpoint, slot->tx_slots[i].buffer_id,
                                          ND_BUFFER_BORROW_READ | ND_BUFFER_BORROW_WRITE) < 0) {
                return;
            }
        }
        slot->state = NET_IFC_BUFFERS_GRANTED;
    }
    wasmos_sys_native_ipc_future_init(&slot->link_get_future, net_stack_link_get_reply_status,
                                      NULL);
    if (!wasmos_sys_native_ipc_future_send(&g_netdrv_loop, &slot->link_get_future, slot->endpoint,
                                           g_netdrv_reply_endpoint, NETDRV_IPC_LINK_GET,
                                           slot->rx_buffer_id, 0u, 0u, 0u, NULL)) {
        return;
    }
    slot->link_get_pending = 1u;
    slot->state = NET_IFC_LINK_QUERIED;
    if (!g_control_runtime ||
        !wasmos_async_start(g_control_runtime, &slot->link_get_coroutine, slot->link_get_stack,
                            sizeof(slot->link_get_stack), net_stack_link_get_coroutine, slot)) {
        wasmos_sys_native_ipc_future_cancel(&slot->link_get_future, -1);
        slot->link_get_pending = 0u;
    }
}

static void net_stack_try_bind_virtio(void) {
    if (g_netdrv_endpoint == 0u) {
        net_stack_start_lookup_coroutine();
        return;
    }
    for (uint32_t i = 0u; i < NET_STACK_MAX_INTERFACES; ++i) {
        if (g_interfaces[i].in_use && !g_interfaces[i].link_get_retire) {
            net_stack_try_bind_interface(&g_interfaces[i]);
        }
    }
}

static int32_t net_stack_pcb_open(net_socket_t* socket) {
    if (socket == NULL || socket->family != NET_SOCKET_AF_INET) {
        return NET_STATUS_INVALID;
    }
    if (socket->type == NET_SOCKET_DGRAM) {
        socket->pcb = udp_new_ip_type(IPADDR_TYPE_V4);
        if (socket->pcb != NULL) {
            udp_recv((struct udp_pcb*)socket->pcb, net_stack_udp_recv, socket);
        }
    } else if (socket->type == NET_SOCKET_STREAM) {
        socket->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    }
    return socket->pcb != NULL ? NET_STATUS_OK : NET_STATUS_NO_MEM;
}

static int32_t net_stack_pcb_bind(net_socket_t* socket, uint16_t port, uint32_t addr_v4) {
    ip_addr_t address;
    err_t err;
    if (socket == NULL || socket->pcb == NULL) {
        return NET_STATUS_INVALID;
    }
    ip_addr_set_ip4_u32(&address, addr_v4);
    if (socket->type == NET_SOCKET_DGRAM) {
        err = udp_bind((struct udp_pcb*)socket->pcb, &address, port);
    } else {
        err = tcp_bind((struct tcp_pcb*)socket->pcb, &address, port);
    }
    return err == ERR_OK ? NET_STATUS_OK : NET_STATUS_ADDR_IN_USE;
}

static int32_t net_stack_pcb_connect(net_socket_t* socket, uint16_t port, uint32_t addr_v4) {
    ip_addr_t address;
    err_t err;
    if (socket == NULL || socket->pcb == NULL) {
        return NET_STATUS_INVALID;
    }
    ip_addr_set_ip4_u32(&address, addr_v4);
    if (socket->type == NET_SOCKET_DGRAM) {
        err = udp_connect((struct udp_pcb*)socket->pcb, &address, port);
        return err == ERR_OK ? NET_STATUS_OK : NET_STATUS_IO_ERROR;
    }
    /* TCP: install the per-socket callbacks and start the handshake. The reply
     * is deferred (NET_STATUS_WOULD_BLOCK) and delivered from tcp_connected or
     * tcp_err once the SYN exchange resolves. */
    {
        struct tcp_pcb* pcb = (struct tcp_pcb*)socket->pcb;
        tcp_arg(pcb, socket);
        tcp_recv(pcb, net_stack_tcp_recv);
        tcp_sent(pcb, net_stack_tcp_sent);
        tcp_err(pcb, net_stack_tcp_err);
        err = tcp_connect(pcb, &address, port, net_stack_tcp_connected);
    }
    return err == ERR_OK ? NET_STATUS_WOULD_BLOCK : NET_STATUS_IO_ERROR;
}

static void net_stack_pcb_close(net_socket_t* socket) {
    if (socket == NULL || socket->pcb == NULL) {
        return;
    }
    if (socket->type == NET_SOCKET_DGRAM) {
        udp_remove((struct udp_pcb*)socket->pcb);
    } else if (socket->state == NET_SOCKET_LISTENING) {
        /* A listen pcb has an accept callback, not recv/sent/err. */
        struct tcp_pcb* pcb = (struct tcp_pcb*)socket->pcb;
        tcp_arg(pcb, NULL);
        tcp_accept(pcb, NULL);
        (void)tcp_close(pcb);
    } else {
        /* Detach callbacks before closing: tcp_close may keep the pcb alive to
         * flush pending data / linger in TIME_WAIT, and the net_socket_t is
         * freed right after this. A late callback must never touch it. */
        struct tcp_pcb* pcb = (struct tcp_pcb*)socket->pcb;
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
        }
    }
    socket->pcb = NULL;
}

/* Passive-open: turn a bound stream pcb into a listening pcb and install the
 * accept callback. tcp_listen replaces the pcb with a smaller listen pcb. */
static int32_t net_stack_pcb_listen(net_socket_t* socket) {
    struct tcp_pcb* lpcb;
    if (socket == NULL || socket->pcb == NULL || socket->type != NET_SOCKET_STREAM) {
        return NET_STATUS_INVALID;
    }
    lpcb = tcp_listen((struct tcp_pcb*)socket->pcb);
    if (lpcb == NULL) {
        return NET_STATUS_NO_MEM;
    }
    socket->pcb = lpcb;
    tcp_arg(lpcb, socket);
    tcp_accept(lpcb, net_stack_tcp_accept);
    return NET_STATUS_OK;
}

static void net_stack_handle_open(const nd_ipc_message_t* request) {
    net_socket_open_descriptor_v1_t* descriptor;
    void* tx_base;
    void* rx_base;
    uint32_t socket_id = 0u;
    uint32_t tx_borrow_id;
    uint32_t rx_borrow_id;
    int32_t status;

    if (g_api == NULL || g_api->xfer_buffer_map_borrowed == NULL ||
        g_api->xfer_buffer_unmap_borrowed == NULL || request->arg2 != sizeof(*descriptor)) {
        net_stack_reply_error(request, NET_STATUS_INVALID);
        return;
    }
    descriptor = (net_socket_open_descriptor_v1_t*)g_api->xfer_buffer_map_borrowed(
        ND_BUFFER_KIND_XFER, request->arg0, request->arg1);
    if (descriptor == NULL) {
        net_stack_reply_error(request, NET_STATUS_DENIED);
        return;
    }
    if (descriptor->family != NET_SOCKET_AF_INET || descriptor->tx_bytes == 0u ||
        descriptor->rx_bytes == 0u) {
        (void)g_api->xfer_buffer_unmap_borrowed(request->arg1);
        net_stack_reply_error(request, NET_STATUS_INVALID);
        return;
    }
    tx_base = g_api->xfer_buffer_map_borrowed(ND_BUFFER_KIND_XFER, descriptor->tx_buffer_id,
                                              descriptor->tx_borrow_id);
    rx_base = g_api->xfer_buffer_map_borrowed(ND_BUFFER_KIND_XFER, descriptor->rx_buffer_id,
                                              descriptor->rx_borrow_id);
    tx_borrow_id = descriptor->tx_borrow_id;
    rx_borrow_id = descriptor->rx_borrow_id;
    if (tx_base == NULL || rx_base == NULL) {
        if (tx_base != NULL) {
            (void)g_api->xfer_buffer_unmap_borrowed(tx_borrow_id);
        }
        if (rx_base != NULL) {
            (void)g_api->xfer_buffer_unmap_borrowed(rx_borrow_id);
        }
        (void)g_api->xfer_buffer_unmap_borrowed(request->arg1);
        net_stack_reply_error(request, NET_STATUS_DENIED);
        return;
    }
    status =
        net_socket_open(&g_socket_pool, request->source, descriptor, tx_base, rx_base, &socket_id);
    (void)g_api->xfer_buffer_unmap_borrowed(request->arg1);
    if (status != NET_STATUS_OK) {
        (void)g_api->xfer_buffer_unmap_borrowed(tx_borrow_id);
        (void)g_api->xfer_buffer_unmap_borrowed(rx_borrow_id);
        net_stack_reply_error(request, status);
        return;
    }
    status = net_stack_pcb_open(&g_socket_pool.sockets[socket_id]);
    if (status != NET_STATUS_OK) {
        (void)net_socket_close(&g_socket_pool, request->source, socket_id);
        (void)g_api->xfer_buffer_unmap_borrowed(tx_borrow_id);
        (void)g_api->xfer_buffer_unmap_borrowed(rx_borrow_id);
        net_stack_reply_error(request, status);
        return;
    }
    net_stack_send_reply(request, NET_IPC_RESP, (int32_t)socket_id, 0u, 0u, 0u);
}

/* Post an accept slot on a listening socket. Mirrors handle_open (client-owned
 * TX/RX rings arrive in a descriptor), but the descriptor is in arg1..arg3
 * (arg0 selects the listener) and the created socket has no pcb yet: it waits in
 * NET_SOCKET_ACCEPTING until net_stack_tcp_accept pairs it with a connection and
 * answers this request with the accepted socket id. */
static void net_stack_handle_accept(const nd_ipc_message_t* request) {
    net_socket_open_descriptor_v1_t* descriptor;
    net_socket_t* listener;
    net_socket_t* slot;
    void* tx_base;
    void* rx_base;
    uint32_t socket_id = 0u;
    uint32_t tx_borrow_id;
    uint32_t rx_borrow_id;
    int32_t status;

    if (g_api == NULL || g_api->xfer_buffer_map_borrowed == NULL ||
        g_api->xfer_buffer_unmap_borrowed == NULL || request->arg3 != sizeof(*descriptor)) {
        net_stack_reply_error(request, NET_STATUS_INVALID);
        return;
    }
    if (request->arg0 >= NET_SOCKET_MAX) {
        net_stack_reply_error(request, NET_STATUS_DENIED);
        return;
    }
    listener = &g_socket_pool.sockets[request->arg0];
    if (listener->owner_endpoint != request->source || listener->type != NET_SOCKET_STREAM ||
        listener->state != NET_SOCKET_LISTENING) {
        net_stack_reply_error(request, NET_STATUS_INVALID);
        return;
    }
    descriptor = (net_socket_open_descriptor_v1_t*)g_api->xfer_buffer_map_borrowed(
        ND_BUFFER_KIND_XFER, request->arg1, request->arg2);
    if (descriptor == NULL) {
        net_stack_reply_error(request, NET_STATUS_DENIED);
        return;
    }
    if (descriptor->family != NET_SOCKET_AF_INET || descriptor->type != NET_SOCKET_STREAM ||
        descriptor->tx_bytes == 0u || descriptor->rx_bytes == 0u) {
        (void)g_api->xfer_buffer_unmap_borrowed(request->arg2);
        net_stack_reply_error(request, NET_STATUS_INVALID);
        return;
    }
    tx_base = g_api->xfer_buffer_map_borrowed(ND_BUFFER_KIND_XFER, descriptor->tx_buffer_id,
                                              descriptor->tx_borrow_id);
    rx_base = g_api->xfer_buffer_map_borrowed(ND_BUFFER_KIND_XFER, descriptor->rx_buffer_id,
                                              descriptor->rx_borrow_id);
    tx_borrow_id = descriptor->tx_borrow_id;
    rx_borrow_id = descriptor->rx_borrow_id;
    if (tx_base == NULL || rx_base == NULL) {
        if (tx_base != NULL) {
            (void)g_api->xfer_buffer_unmap_borrowed(tx_borrow_id);
        }
        if (rx_base != NULL) {
            (void)g_api->xfer_buffer_unmap_borrowed(rx_borrow_id);
        }
        (void)g_api->xfer_buffer_unmap_borrowed(request->arg2);
        net_stack_reply_error(request, NET_STATUS_DENIED);
        return;
    }
    status =
        net_socket_open(&g_socket_pool, request->source, descriptor, tx_base, rx_base, &socket_id);
    (void)g_api->xfer_buffer_unmap_borrowed(request->arg2);
    if (status != NET_STATUS_OK) {
        (void)g_api->xfer_buffer_unmap_borrowed(tx_borrow_id);
        (void)g_api->xfer_buffer_unmap_borrowed(rx_borrow_id);
        net_stack_reply_error(request, status);
        return;
    }
    slot = &g_socket_pool.sockets[socket_id];
    slot->state = NET_SOCKET_ACCEPTING;
    slot->accept_listener_id = request->arg0;
    slot->connect_request_id = request->request_id;
    slot->connect_pending = 1u;
    /* Reply deferred to net_stack_tcp_accept once a connection arrives. */
}

/* Interface-address control plane (the `ip` tool). Each operation selects its
 * target by interface index; the default interface is only for route choice. */
static int32_t net_stack_active_index(void) {
    if (g_active_ifc == NULL) {
        return -1;
    }
    return (int32_t)(g_active_ifc - g_interfaces);
}

static void net_stack_handle_ifaddr_add(const nd_ipc_message_t* request) {
    net_interface_slot_t* interface;
    net_ifaddr_record_v1_t* rec;
    ip4_addr_t ip;
    ip4_addr_t mask;
    ip4_addr_t gw;
    if (g_api == NULL || g_api->xfer_buffer_map_borrowed == NULL ||
        g_api->xfer_buffer_unmap_borrowed == NULL || request->arg2 != sizeof(*rec)) {
        net_stack_reply_error(request, NET_STATUS_INVALID);
        return;
    }
    rec = (net_ifaddr_record_v1_t*)g_api->xfer_buffer_map_borrowed(ND_BUFFER_KIND_XFER,
                                                                   request->arg0, request->arg1);
    if (rec == NULL) {
        net_stack_reply_error(request, NET_STATUS_DENIED);
        return;
    }
    if (rec->version != NET_IFADDR_RECORD_VERSION) {
        (void)g_api->xfer_buffer_unmap_borrowed(request->arg1);
        net_stack_reply_error(request, NET_STATUS_INVALID);
        return;
    }
    interface = net_stack_interface_from_index(rec->if_index);
    if (interface == NULL || !interface->netif_installed) {
        (void)g_api->xfer_buffer_unmap_borrowed(request->arg1);
        net_stack_reply_error(request, NET_STATUS_NOT_READY);
        return;
    }
    ip4_addr_set_u32(&ip, rec->addr_v4);
    ip4_addr_set_u32(&mask, rec->netmask_v4);
    ip4_addr_set_u32(&gw, rec->gateway_v4);
    /* A manual address stops any DHCP client so the two do not fight. */
    if (interface->dhcp_active) {
        dhcp_stop(&interface->netif);
        interface->dhcp_active = 0u;
    }
    netif_set_addr(&interface->netif, &ip, &mask, &gw);
    (void)g_api->xfer_buffer_unmap_borrowed(request->arg1);
    net_stack_send_reply(request, NET_IPC_RESP, NET_STATUS_OK, 0u, 0u, 0u);
}

static void net_stack_handle_if_set_state(const nd_ipc_message_t* request) {
    net_interface_slot_t* interface;
    /* arg0 = if_index, arg1 = 1 (up) / 0 (down). Administrative state only;
     * link state is driven by the driver's LINK_NOTIFY, not this. */
    interface = net_stack_interface_from_index(request->arg0);
    if (interface == NULL || !interface->netif_installed) {
        net_stack_reply_error(request, NET_STATUS_NOT_READY);
        return;
    }
    if (request->arg1 != 0u) {
        netif_set_up(&interface->netif);
    } else {
        netif_set_down(&interface->netif);
    }
    net_stack_send_reply(request, NET_IPC_RESP, NET_STATUS_OK, 0u, 0u, 0u);
}

static void net_stack_handle_ifaddr_del(const nd_ipc_message_t* request) {
    net_interface_slot_t* interface;
    ip4_addr_t zero;
    interface = net_stack_interface_from_index(request->arg0);
    if (interface == NULL || !interface->netif_installed) {
        net_stack_reply_error(request, NET_STATUS_NOT_READY);
        return;
    }
    if (interface->dhcp_active) {
        dhcp_stop(&interface->netif);
        interface->dhcp_active = 0u;
    }
    ip4_addr_set_zero(&zero);
    netif_set_addr(&interface->netif, &zero, &zero, &zero);
    interface->addr_ready = 0u;
    net_stack_send_reply(request, NET_IPC_RESP, NET_STATUS_OK, 0u, 0u, 0u);
}

static void net_stack_handle_dhcp_set(const nd_ipc_message_t* request) {
    net_interface_slot_t* interface;
    /* arg0 = if_index, arg1 = 1 (start) / 0 (stop). */
    interface = net_stack_interface_from_index(request->arg0);
    if (interface == NULL || !interface->netif_installed) {
        net_stack_reply_error(request, NET_STATUS_NOT_READY);
        return;
    }
    if (request->arg1 != 0u) {
        ip4_addr_t zero;
        /* Clear any static address so the fresh lease is not shadowed, then
         * (re)start the client. dhcp_start is idempotent for an active client. */
        ip4_addr_set_zero(&zero);
        netif_set_addr(&interface->netif, &zero, &zero, &zero);
        interface->addr_ready = 0u;
        if (dhcp_start(&interface->netif) != ERR_OK) {
            net_stack_reply_error(request, NET_STATUS_IO_ERROR);
            return;
        }
        interface->dhcp_active = 1u;
        interface->dhcp_started_tick = g_api->sched_ticks != NULL ? g_api->sched_ticks() : 0u;
        interface->dhcp_timeout_logged = 0u;
    } else {
        if (interface->dhcp_active) {
            dhcp_stop(&interface->netif);
            interface->dhcp_active = 0u;
        }
    }
    net_stack_send_reply(request, NET_IPC_RESP, NET_STATUS_OK, 0u, 0u, 0u);
}

static void net_stack_handle_ifaddr_list(const nd_ipc_message_t* request) {
    net_ifaddr_record_v1_t* out;
    uint32_t capacity;
    uint32_t count = 0u;
    if (g_api == NULL || g_api->xfer_buffer_map_borrowed == NULL ||
        g_api->xfer_buffer_unmap_borrowed == NULL || request->arg2 < sizeof(*out)) {
        net_stack_reply_error(request, NET_STATUS_INVALID);
        return;
    }
    out = (net_ifaddr_record_v1_t*)g_api->xfer_buffer_map_borrowed(ND_BUFFER_KIND_XFER,
                                                                   request->arg0, request->arg1);
    if (out == NULL) {
        net_stack_reply_error(request, NET_STATUS_DENIED);
        return;
    }
    capacity = request->arg2 / (uint32_t)sizeof(*out);
    for (uint32_t i = 0u; i < NET_STACK_MAX_INTERFACES && count < capacity; ++i) {
        if (!g_interfaces[i].in_use) {
            continue;
        }
        out[count].version = NET_IFADDR_RECORD_VERSION;
        out[count].if_index = i;
        out[count].flags = g_interfaces[i].link_up ? NET_IFADDR_FLAG_LINK_UP : 0u;
        if (g_interfaces[i].netif_installed) {
            if (netif_is_up(&g_interfaces[i].netif)) {
                out[count].flags |= NET_IFADDR_FLAG_ADMIN_UP;
            }
            if (g_interfaces[i].dhcp_active) {
                out[count].flags |= NET_IFADDR_FLAG_DHCP;
            }
            out[count].addr_v4 = ip4_addr_get_u32(ip_2_ip4(&g_interfaces[i].netif.ip_addr));
            out[count].netmask_v4 = ip4_addr_get_u32(ip_2_ip4(&g_interfaces[i].netif.netmask));
            out[count].gateway_v4 = ip4_addr_get_u32(ip_2_ip4(&g_interfaces[i].netif.gw));
        } else {
            out[count].addr_v4 = 0u;
            out[count].netmask_v4 = 0u;
            out[count].gateway_v4 = 0u;
        }
        count++;
    }
    (void)g_api->xfer_buffer_unmap_borrowed(request->arg1);
    net_stack_send_reply(request, NET_IPC_RESP, (int32_t)count, 0u, 0u, 0u);
}

static void net_stack_dispatch(const nd_ipc_message_t* request) {
    net_interface_slot_t* interface;
    int32_t status;
    if (request == NULL || request->source == 0u || request->source == 0xFFFFFFFFu) {
        return;
    }

    /* Driver replies and notification hints share this endpoint with socket
     * traffic. Keep them asynchronous: a blocking receive here could consume
     * and lose a socket request queued behind a netdrv reply. */
    interface = net_stack_interface_from_endpoint(request->source);
    if (interface != NULL) {
        if (request->type == NETDRV_IPC_RX_FRAME_NOTIFY) {
            net_stack_start_rx_poll(interface, 1u);
            return;
        }
        if (request->request_id == NET_STACK_RX_POLL_REQUEST_ID) {
            interface->rx_pending = 0u;
            if (request->type == NETDRV_IPC_RESP) {
                net_stack_deliver_rx(interface, request->arg0);
                /* Drain frames that the driver already queued without waiting
                 * for another notification. Empty responses wait for the
                 * timer cadence below, avoiding a request/reply spin loop. */
                if (request->arg1 != 0u)
                    net_stack_start_rx_poll(interface, 1u);
            }
            return;
        }
        if (request->request_id >= NET_STACK_TX_REQUEST_BASE) {
            uint32_t slot = request->request_id - NET_STACK_TX_REQUEST_BASE;
            if (slot < NET_STACK_TX_QUEUE_DEPTH)
                interface->tx_slots[slot].pending = 0u;
            return;
        }
        if (request->type == NETDRV_IPC_LINK_NOTIFY) {
            /* A link change updates the existing lwIP netif in place; it must
             * never re-add, re-register, or rebind the interface. The console
             * line reflects the netif transition and gives runtime tests an
             * observable signal without a source-text assertion. */
            if (interface->netif_ready) {
                if (request->arg0 != 0u) {
                    netif_set_link_up(&interface->netif);
                    if (g_api->console_write != NULL) {
                        static const char msg[] = "[net-stack] link up\n";
                        g_api->console_write(msg, (int)(sizeof(msg) - 1));
                    }
                    /* If the interface came up link-down and no config has been
                     * read yet, this up edge is the trigger to read it. */
                    net_stack_kick_ifcfg_load();
                } else {
                    netif_set_link_down(&interface->netif);
                    if (g_api->console_write != NULL) {
                        static const char msg[] = "[net-stack] link down\n";
                        g_api->console_write(msg, (int)(sizeof(msg) - 1));
                    }
                }
            }
            interface->link_up = request->arg0 != 0u;
            return;
        }
    }

    if (request->type == SVC_IPC_REGISTER_RESP) {
        net_stack_registered();
        return;
    }

    if (request->type == SVC_IPC_CLASS_EVENT) {
        if (request->arg0 == SVC_CLASS_EVENT_ADD) {
            net_interface_slot_t* slot = net_stack_interface_slot(request->arg2, request->arg1, 1u);
            if (g_active_ifc == NULL && slot != NULL) {
                g_active_ifc = slot;
                g_netdrv_endpoint = request->arg2;
            }
        } else if (request->arg0 == SVC_CLASS_EVENT_REMOVE) {
            net_stack_unbind_interface(request->arg2);
        }
        return;
    }

    switch (request->type) {
    case NET_IPC_SOCKET_OPEN:
        net_stack_handle_open(request);
        break;
    case NET_IPC_BIND:
        if (request->arg0 >= NET_SOCKET_MAX ||
            g_socket_pool.sockets[request->arg0].owner_endpoint != request->source) {
            net_stack_reply_error(request, NET_STATUS_DENIED);
            break;
        }
        status = net_stack_pcb_bind(&g_socket_pool.sockets[request->arg0], (uint16_t)request->arg1,
                                    request->arg2);
        if (status == NET_STATUS_OK) {
            status = net_socket_bind(&g_socket_pool, request->source, request->arg0,
                                     (uint16_t)request->arg1, request->arg2);
        }
        if (status == NET_STATUS_OK) {
            net_stack_send_reply(request, NET_IPC_RESP, status, 0u, 0u, 0u);
        } else {
            net_stack_reply_error(request, status);
        }
        break;
    case NET_IPC_CONNECT: {
        net_socket_t* socket;
        if (request->arg0 >= NET_SOCKET_MAX ||
            g_socket_pool.sockets[request->arg0].owner_endpoint != request->source) {
            net_stack_reply_error(request, NET_STATUS_DENIED);
            break;
        }
        socket = &g_socket_pool.sockets[request->arg0];
        /* Record the remote peer and advance the pool state (CONNECTING for a
         * stream socket, CONNECTED for a datagram socket). */
        status = net_socket_connect(&g_socket_pool, request->source, request->arg0,
                                    (uint16_t)request->arg1, request->arg2);
        if (status != NET_STATUS_OK) {
            net_stack_reply_error(request, status);
            break;
        }
        /* Stash the request id so a deferred TCP reply can find it. */
        socket->connect_request_id = request->request_id;
        socket->connect_pending = 1u;
        status = net_stack_pcb_connect(socket, (uint16_t)request->arg1, request->arg2);
        if (status == NET_STATUS_WOULD_BLOCK) {
            /* TCP handshake in flight: reply deferred to tcp_connected/tcp_err. */
            break;
        }
        socket->connect_pending = 0u;
        if (status == NET_STATUS_OK) {
            net_stack_send_reply(request, NET_IPC_RESP, status, 0u, 0u, 0u);
        } else {
            net_stack_reply_error(request, status);
        }
        break;
    }
    case NET_IPC_LISTEN:
        if (request->arg0 >= NET_SOCKET_MAX ||
            g_socket_pool.sockets[request->arg0].owner_endpoint != request->source) {
            net_stack_reply_error(request, NET_STATUS_DENIED);
            break;
        }
        /* net_socket_listen validates a BOUND stream socket and advances the
         * pool state; roll it back if the lwIP listen pcb cannot be created. */
        status = net_socket_listen(&g_socket_pool, request->source, request->arg0);
        if (status == NET_STATUS_OK) {
            status = net_stack_pcb_listen(&g_socket_pool.sockets[request->arg0]);
            if (status != NET_STATUS_OK) {
                g_socket_pool.sockets[request->arg0].state = NET_SOCKET_BOUND;
            }
        }
        if (status == NET_STATUS_OK) {
            net_stack_send_reply(request, NET_IPC_RESP, status, 0u, 0u, 0u);
        } else {
            net_stack_reply_error(request, status);
        }
        break;
    case NET_IPC_ACCEPT:
        net_stack_handle_accept(request);
        break;
    case NET_IPC_CLOSE:
        if (request->arg0 >= NET_SOCKET_MAX ||
            g_socket_pool.sockets[request->arg0].owner_endpoint != request->source) {
            net_stack_reply_error(request, NET_STATUS_DENIED);
            break;
        }
        uint32_t tx_borrow_id = g_socket_pool.sockets[request->arg0].tx_borrow_id;
        uint32_t rx_borrow_id = g_socket_pool.sockets[request->arg0].rx_borrow_id;
        net_stack_pcb_close(&g_socket_pool.sockets[request->arg0]);
        status = net_socket_close(&g_socket_pool, request->source, request->arg0);
        if (status == NET_STATUS_OK) {
            (void)g_api->xfer_buffer_unmap_borrowed(tx_borrow_id);
            (void)g_api->xfer_buffer_unmap_borrowed(rx_borrow_id);
            net_stack_send_reply(request, NET_IPC_RESP, status, 0u, 0u, 0u);
        } else {
            net_stack_reply_error(request, status);
        }
        break;
    case NET_IPC_IFADDR_ADD:
        net_stack_handle_ifaddr_add(request);
        break;
    case NET_IPC_IFADDR_DEL:
        net_stack_handle_ifaddr_del(request);
        break;
    case NET_IPC_IFADDR_LIST:
        net_stack_handle_ifaddr_list(request);
        break;
    case NET_IPC_IF_SET_STATE:
        net_stack_handle_if_set_state(request);
        break;
    case NET_IPC_DHCP_SET:
        net_stack_handle_dhcp_set(request);
        break;
    case NET_IPC_SEND:
    case NET_IPC_RECV:
    case NET_IPC_POLL:
    case NET_IPC_STACK_CREATE:
    case NET_IPC_STACK_DESTROY:
    case NET_IPC_STACK_SELECT:
    case NET_IPC_TX_NOTIFY: {
        net_socket_t* socket;
        if (request->arg0 >= NET_SOCKET_MAX ||
            g_socket_pool.sockets[request->arg0].owner_endpoint != request->source) {
            net_stack_reply_error(request, NET_STATUS_DENIED);
            break;
        }
        socket = &g_socket_pool.sockets[request->arg0];
        /* A TX doorbell just advances the data plane; there is no reply. The ring
         * type selects the transport drain. */
        if (socket->type == NET_SOCKET_DGRAM) {
            net_stack_drain_udp_tx(socket);
        } else if (socket->type == NET_SOCKET_STREAM) {
            net_stack_drain_tcp_tx(socket);
        }
        break;
    }
    case NET_IPC_RX_NOTIFY:
        net_stack_reply_error(request, NET_STATUS_NOT_READY);
        break;
    default:
        net_stack_reply_error(request, NET_STATUS_INVALID);
        break;
    }
}

/* The reply endpoint has one correlated LINK_GET request plus recurring driver
 * notifications. Event-loop intent matching consumes the former before this
 * handler runs; registered notification and default paths retain the latter. */
static void net_stack_netdrv_event(void* user, const nd_ipc_message_t* request) {
    (void)user;
    net_stack_dispatch(request);
}

/* Bound the idle block by lwIP's next timer so TCP/DHCP timers still fire, but
 * never longer than the empty-RX poll cadence, and never 0 (which ipc_select_wait
 * treats as "block forever"). */
static uint32_t net_stack_idle_wait_ms(void) {
    uint32_t ms = sys_timeouts_sleeptime();
    if (ms > NET_STACK_IDLE_WAIT_MS) {
        ms = NET_STACK_IDLE_WAIT_MS;
    }
    if (ms == 0u) {
        ms = 1u;
    }
    return ms;
}

/* Service idle hook. The pump calls this on the kernel-thread stack after the
 * root coroutine yields, so blocking here suspends the thread with a valid rsp.
 * Block on the watched endpoints only when the root reported the runtime idle;
 * otherwise cooperatively yield so ready child coroutines still get scheduled. */
static void net_stack_idle(void* user) {
    (void)user;
    if (g_net_want_block && g_select_ready && g_api != NULL && g_api->ipc_select_wait != NULL) {
        uint32_t ready_ep = 0u;
        int rc;
        rc = g_api->ipc_select_wait(g_select_id, g_api->sched_current_pid(), &ready_ep,
                                    net_stack_idle_wait_ms());
        (void)rc;
    } else if (g_api != NULL && g_api->sched_yield != NULL) {
        g_api->sched_yield();
    }
}

/* Native async root. libsys's async_initialize is the ELF entry and invokes
 * this inside the predefined root coroutine. */
int32_t wasmos_async_main(wasmos_driver_api_t* driver_api,
                          wasmos_native_coroutine_runtime_t* runtime, void* user) {
    (void)user;

    g_api = driver_api;
    if (driver_api == NULL || driver_api->abi_magic != WASMOS_NATIVE_ABI_MAGIC ||
        driver_api->abi_version != WASMOS_NATIVE_ABI_VERSION) {
        return -2;
    }

    /* Bring up the lwIP core (raw API, NO_SYS). This allocates the static
     * memory pools sized by lwipopts.h. No netif is added yet. */
    lwip_init();
    net_socket_pool_init(&g_socket_pool);

    if (driver_api->console_write != NULL) {
        static const char msg[] = "[net-stack] lwip_init ok\n";
        driver_api->console_write(msg, (int)(sizeof(msg) - 1));
    }

    wasmos_spawn_info_t spawn_info;
    if (driver_api->spawn_info == NULL || driver_api->spawn_info(&spawn_info, NULL, 0u) != 0 ||
        spawn_info.magic != WASMOS_SPAWN_INFO_MAGIC ||
        spawn_info.version != WASMOS_SPAWN_INFO_VERSION || spawn_info.proc_endpoint == 0u) {
        return -1;
    }
    g_endpoint = driver_api->ipc_create_endpoint();
    if (g_endpoint == 0xFFFFFFFFu) {
        return -1;
    }

    g_control_endpoint = driver_api->ipc_create_endpoint();
    if (g_control_endpoint == 0xFFFFFFFFu) {
        return -1;
    }
    g_netdrv_reply_endpoint = driver_api->ipc_create_endpoint();
    if (g_netdrv_reply_endpoint == 0xFFFFFFFFu) {
        return -1;
    }
    g_proc_endpoint = spawn_info.proc_endpoint;
    /* Device-manager starts bootstrap services synchronously. Release that
     * spawn before asking the process manager to service our registration; the
     * PM cannot reply while it is still waiting for this service's ready ack. */
    if (driver_api->proc_notify_ready != NULL) {
        driver_api->proc_notify_ready();
    }
    wasmos_sys_native_event_loop_init(&g_control_loop, driver_api, g_control_endpoint,
                                      NET_STACK_REGISTER_REQUEST_ID);
    wasmos_sys_native_event_loop_init(&g_netdrv_loop, driver_api, g_netdrv_reply_endpoint,
                                      NET_STACK_TX_REQUEST_BASE + NET_STACK_TX_QUEUE_DEPTH);
    if (wasmos_sys_native_event_register(&g_netdrv_loop, NETDRV_IPC_RX_FRAME_NOTIFY,
                                         net_stack_netdrv_event, NULL) != 0 ||
        wasmos_sys_native_event_register(&g_netdrv_loop, NETDRV_IPC_LINK_NOTIFY,
                                         net_stack_netdrv_event, NULL) != 0 ||
        wasmos_sys_native_event_set_default(&g_netdrv_loop, net_stack_netdrv_event, NULL) != 0) {
        return -1;
    }
    g_control_runtime = runtime;

    /* Watch all three endpoints net-stack listens on so the idle path can block
     * on the set rather than yield-spinning. A failure here is non-fatal: the
     * loop falls back to cooperative yielding. */
    if (driver_api->ipc_select_listen != NULL) {
        const uint32_t endpoints[3] = {g_endpoint, g_control_endpoint, g_netdrv_reply_endpoint};
        if (driver_api->ipc_select_listen(driver_api->sched_current_pid(), endpoints, 3u,
                                          &g_select_id) == ND_IPC_OK) {
            g_select_ready = 1u;
        }
    }

    /* Drain socket requests and both native event loops before blocking. A
     * service must not assume a later readiness edge will re-signal work that
     * was already queued. */
    for (;;) {
        nd_ipc_message_t request;
        int drained = 0;
        net_stack_reap_interfaces();
        for (;;) {
            int rc = driver_api->ipc_recv(driver_api->sched_current_pid(), g_endpoint, &request);
            if (rc == ND_IPC_EMPTY) {
                break;
            }
            if (rc != ND_IPC_OK) {
                return -1;
            }
            drained = 1;
            net_stack_dispatch(&request);
        }
        if (wasmos_sys_native_event_loop_poll(&g_control_loop, 8u) < 0) {
            return -1;
        }
        if (wasmos_sys_native_event_loop_poll(&g_netdrv_loop, 8u) < 0) {
            return -1;
        }
        net_stack_begin_registration();
        net_stack_try_seed_random();
        net_stack_try_discover_interfaces();
        /* Keep the old name lookup below as a compatibility fallback for
         * drivers that predate the net.ifc class registration. */
        net_stack_try_bind_virtio();
        net_stack_try_load_ifcfg();
        sys_check_timeouts();
        for (uint32_t i = 0u; i < NET_STACK_MAX_INTERFACES; ++i) {
            net_interface_slot_t* interface = &g_interfaces[i];
            if (interface->in_use && interface->dhcp_active && !interface->addr_ready &&
                !interface->dhcp_timeout_logged && g_api->sched_ticks != NULL &&
                (uint32_t)(g_api->sched_ticks() - interface->dhcp_started_tick) >
                    NET_STACK_DHCP_TIMEOUT_TICKS) {
                /* Strict: no lease within the timeout leaves the interface
                 * unconfigured. lwIP keeps retrying, so a late lease still binds. */
                interface->dhcp_timeout_logged = 1u;
                if (driver_api->console_write != NULL) {
                    static const char msg[] = "[net-stack] dhcp: no lease\n";
                    driver_api->console_write(msg, (int)(sizeof(msg) - 1));
                }
            }
        }
        for (uint32_t i = 0u; i < NET_STACK_MAX_INTERFACES; ++i) {
            if (g_interfaces[i].in_use && g_interfaces[i].netif_ready) {
                net_stack_start_rx_poll(&g_interfaces[i], 0u);
            }
        }
        (void)drained;
        /* Record whether the runtime is idle (no runnable child coroutine)
         * while we can still observe it accurately - the root is RUNNING here,
         * so has_ready() reflects only other coroutines. The service idle hook,
         * which runs on the kernel-thread stack after this yield, uses the flag
         * to decide whether to block on the endpoints or keep cooperating.
         * The blocking wait must NOT happen here: it would run on the coroutine
         * stack and the scheduler rejects a thread suspended with that rsp. */
        g_net_want_block = !wasmos_native_coroutine_runtime_has_ready(runtime);
        wasmos_native_coroutine_yield();
    }

    /* Not reached. */
    return 0;
}
