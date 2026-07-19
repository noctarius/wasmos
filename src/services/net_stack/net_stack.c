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
 *   - No receive callbacks, payload-ring processing, or TCP handshake state.
 */
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#include "lwip/init.h"
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

#include "socket.h"
#include "wasmos/libsys_native.h"
#include "wasmos_native_driver.h"

/* The api table captured at initialize() time. Read by port.c (sys_now,
 * lwip_port_rand) and net_stack_lwip_diag(). NULL until initialize() runs. */
static wasmos_driver_api_t* g_api = NULL;
static net_socket_pool_t g_socket_pool;
static uint32_t g_endpoint = 0u;
static uint32_t g_control_endpoint = 0u;
static uint32_t g_proc_endpoint = 0u;
static uint32_t g_netdrv_endpoint = 0u;
static uint32_t g_rx_buffer_id = 0u;
static uint8_t* g_rx_buffer = NULL;
static uint32_t g_tx_buffer_id = 0u;
static uint8_t* g_tx_buffer = NULL;
static uint8_t g_rx_pending = 0u;
static uint8_t g_tx_pending = 0u;
static uint8_t g_netif_ready = 0u;
static struct netif g_netif;

#define ND_IPC_OK 0
#define ND_IPC_EMPTY 1
#define NET_STACK_REGISTER_REQUEST_ID 0x4E530001u
#define NET_STACK_LINK_GET_REQUEST_ID 0x4E530002u
#define NET_STACK_RX_POLL_REQUEST_ID 0x4E530003u
#define NET_STACK_TX_REQUEST_BASE 0x4E535000u
#define NET_STACK_FRAME_BYTES 2048u

static err_t net_stack_linkoutput(struct netif* netif, struct pbuf* p);
static void net_stack_start_rx_poll(void);

wasmos_driver_api_t* net_stack_api(void) {
    return g_api;
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

static err_t net_stack_netif_init(struct netif* netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = net_stack_linkoutput;
    netif->mtu = 1500u;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static int net_stack_driver_call(uint32_t request_id, uint32_t type, uint32_t arg0,
                                 uint32_t arg1, nd_ipc_message_t* reply) {
    if (g_netdrv_endpoint == 0u || reply == NULL) {
        return -1;
    }
    if (wasmos_sys_ipc_call_native(g_api, g_control_endpoint, g_netdrv_endpoint, request_id, type, arg0,
                                   arg1, 0u, 0u, reply) != 0) {
        return -1;
    }
    return reply->type == NETDRV_IPC_RESP ? 0 : -1;
}

static err_t net_stack_linkoutput(struct netif* netif, struct pbuf* p) {
    nd_ipc_message_t request;
    uint16_t copied = 0u;
    uint32_t request_id;
    struct pbuf* q;
    (void)netif;
    if (p == NULL || g_tx_buffer == NULL || g_tx_pending || p->tot_len > NET_STACK_FRAME_BYTES) {
        return ERR_BUF;
    }
    for (q = p; q != NULL; q = q->next) {
        uint8_t* src = (uint8_t*)q->payload;
        uint16_t i;
        for (i = 0u; i < q->len; ++i) {
            g_tx_buffer[copied + i] = src[i];
        }
        copied = (uint16_t)(copied + q->len);
    }
    request_id = NET_STACK_TX_REQUEST_BASE + (uint32_t)sys_now();
    request.type = NETDRV_IPC_TX_FRAME;
    request.source = g_endpoint;
    request.destination = g_netdrv_endpoint;
    request.request_id = request_id;
    request.arg0 = copied;
    request.arg1 = g_tx_buffer_id;
    request.arg2 = 0u;
    request.arg3 = 0u;
    if (g_api->ipc_send(g_api->sched_current_pid(), g_netdrv_endpoint, &request) != 0) {
        return ERR_IF;
    }
    g_tx_pending = 1u;
    return ERR_OK;
}

static void net_stack_deliver_rx(uint32_t len) {
    struct pbuf* p;
    if (len == 0u || len > NET_STACK_FRAME_BYTES) {
        return;
    }
    p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (p == NULL) {
        return;
    }
    if (pbuf_take(p, g_rx_buffer, (u16_t)len) != ERR_OK) {
        pbuf_free(p);
        return;
    }
    if (len >= 14u && g_rx_buffer[12] == 0x08u && g_rx_buffer[13] == 0x06u &&
        g_api->console_write != NULL) {
        static const char msg[] = "[net-stack] arp rx\n";
        g_api->console_write(msg, (int)(sizeof(msg) - 1));
    }
    (void)ethernet_input(p, &g_netif);
}

static void net_stack_start_rx_poll(void) {
    nd_ipc_message_t request;
    if (g_rx_pending || g_netdrv_endpoint == 0u) {
        return;
    }
    request.type = NETDRV_IPC_RX_POLL;
    request.source = g_endpoint;
    request.destination = g_netdrv_endpoint;
    request.request_id = NET_STACK_RX_POLL_REQUEST_ID;
    request.arg0 = g_rx_buffer_id;
    request.arg1 = 0u;
    request.arg2 = 0u;
    request.arg3 = 0u;
    if (g_api->ipc_send(g_api->sched_current_pid(), g_netdrv_endpoint, &request) == 0) {
        g_rx_pending = 1u;
    }
}

static void net_stack_bind_virtio(void) {
    nd_ipc_message_t link_reply;
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gateway;

    if (g_netif_ready) {
        return;
    }
    g_netdrv_endpoint = (uint32_t)wasmos_sys_svc_lookup_native(
        g_api, g_proc_endpoint, g_control_endpoint, (const uint8_t*)"virtio.net", 10u,
        NET_STACK_LINK_GET_REQUEST_ID - 1u);
    if ((int32_t)g_netdrv_endpoint < 0) {
        g_netdrv_endpoint = 0u;
        return;
    }
    g_rx_buffer = (uint8_t*)g_api->xfer_buffer_acquire(ND_BUFFER_KIND_XFER, NET_STACK_FRAME_BYTES,
                                                         &g_rx_buffer_id);
    g_tx_buffer = (uint8_t*)g_api->xfer_buffer_acquire(ND_BUFFER_KIND_XFER, NET_STACK_FRAME_BYTES,
                                                         &g_tx_buffer_id);
    if (g_rx_buffer == NULL || g_tx_buffer == NULL ||
        g_api->xfer_buffer_borrow(g_netdrv_endpoint, g_rx_buffer_id,
                                  ND_BUFFER_BORROW_READ | ND_BUFFER_BORROW_WRITE) < 0 ||
        g_api->xfer_buffer_borrow(g_netdrv_endpoint, g_tx_buffer_id,
                                  ND_BUFFER_BORROW_READ | ND_BUFFER_BORROW_WRITE) < 0 ||
        net_stack_driver_call(NET_STACK_LINK_GET_REQUEST_ID, NETDRV_IPC_LINK_GET, g_rx_buffer_id,
                              0u, &link_reply) != 0 ||
        link_reply.arg0 == 0u) {
        return;
    }
    IP4_ADDR(&ipaddr, 10, 0, 2, 15);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 10, 0, 2, 2);
    if (netif_add(&g_netif, &ipaddr, &netmask, &gateway, NULL, net_stack_netif_init,
                  ethernet_input) == NULL) {
        return;
    }
    for (uint32_t i = 0u; i < ETH_HWADDR_LEN; ++i) {
        g_netif.hwaddr[i] = g_rx_buffer[i];
    }
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    etharp_request(&g_netif, &gateway);
    g_netif_ready = 1u;
    if (g_api->console_write != NULL) {
        static const char msg[] = "[net-stack] eth0 10.0.2.15/24 ready\n";
        g_api->console_write(msg, (int)(sizeof(msg) - 1));
    }
}

static int32_t net_stack_pcb_open(net_socket_t* socket) {
    if (socket == NULL || socket->family != NET_SOCKET_AF_INET) {
        return NET_STATUS_INVALID;
    }
    if (socket->type == NET_SOCKET_DGRAM) {
        socket->pcb = udp_new_ip_type(IPADDR_TYPE_V4);
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
    } else {
        /* TODO(net_stack): install TCP receive/error callbacks and defer the
         * response until the SYN handshake completes once a netif is bound. */
        err = tcp_connect((struct tcp_pcb*)socket->pcb, &address, port, NULL);
    }
    return err == ERR_OK ? NET_STATUS_OK : NET_STATUS_IO_ERROR;
}

static void net_stack_pcb_close(net_socket_t* socket) {
    if (socket == NULL || socket->pcb == NULL) {
        return;
    }
    if (socket->type == NET_SOCKET_DGRAM) {
        udp_remove((struct udp_pcb*)socket->pcb);
    } else {
        tcp_abort((struct tcp_pcb*)socket->pcb);
    }
    socket->pcb = NULL;
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
    status = net_socket_open(&g_socket_pool, request->source, descriptor, tx_base, rx_base,
                             &socket_id);
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

static void net_stack_dispatch(const nd_ipc_message_t* request) {
    int32_t status;
    if (request == NULL || request->source == 0u || request->source == 0xFFFFFFFFu) {
        return;
    }

    /* Driver replies and notification hints share this endpoint with socket
     * traffic. Keep them asynchronous: a blocking receive here could consume
     * and lose a socket request queued behind a netdrv reply. */
    if (request->source == g_netdrv_endpoint) {
        if (request->type == NETDRV_IPC_RX_FRAME_NOTIFY) {
            net_stack_start_rx_poll();
            return;
        }
        if (request->request_id == NET_STACK_RX_POLL_REQUEST_ID) {
            g_rx_pending = 0u;
            if (request->type == NETDRV_IPC_RESP) {
                net_stack_deliver_rx(request->arg0);
            }
            return;
        }
        if (request->request_id >= NET_STACK_TX_REQUEST_BASE) {
            g_tx_pending = 0u;
            return;
        }
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
        status = net_stack_pcb_bind(&g_socket_pool.sockets[request->arg0],
                                    (uint16_t)request->arg1, request->arg2);
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
    case NET_IPC_CONNECT:
        if (request->arg0 >= NET_SOCKET_MAX ||
            g_socket_pool.sockets[request->arg0].owner_endpoint != request->source) {
            net_stack_reply_error(request, NET_STATUS_DENIED);
            break;
        }
        status = net_stack_pcb_connect(&g_socket_pool.sockets[request->arg0],
                                       (uint16_t)request->arg1, request->arg2);
        if (status == NET_STATUS_OK) {
            status = net_socket_connect(&g_socket_pool, request->source, request->arg0,
                                        (uint16_t)request->arg1, request->arg2);
        }
        if (status == NET_STATUS_OK) {
            net_stack_send_reply(request, NET_IPC_RESP, status, 0u, 0u, 0u);
        } else {
            net_stack_reply_error(request, status);
        }
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
    case NET_IPC_SEND:
    case NET_IPC_RECV:
    case NET_IPC_POLL:
    case NET_IPC_IFADDR_ADD:
    case NET_IPC_IFADDR_DEL:
    case NET_IPC_IFADDR_LIST:
    case NET_IPC_STACK_CREATE:
    case NET_IPC_STACK_DESTROY:
    case NET_IPC_STACK_SELECT:
    case NET_IPC_TX_NOTIFY:
    case NET_IPC_RX_NOTIFY:
        net_stack_reply_error(request, NET_STATUS_NOT_READY);
        break;
    default:
        net_stack_reply_error(request, NET_STATUS_INVALID);
        break;
    }
}

/* Native service entry. Signature/ABI match gfx_compositor's initialize():
 *   int initialize(wasmos_driver_api_t *api, int module_count, int, int)
 * The loader jumps here via ELF e_entry (-e initialize). */
int initialize(wasmos_driver_api_t* driver_api, int module_count, int arg2, int arg3) {
    (void)module_count;
    (void)arg2;
    (void)arg3;

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
    if (driver_api->spawn_info == NULL ||
        driver_api->spawn_info(&spawn_info, NULL, 0u) != 0 ||
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
    g_proc_endpoint = spawn_info.proc_endpoint;
    /* Device-manager starts bootstrap services synchronously. Release that
     * spawn before asking the process manager to service our registration; the
     * PM cannot reply while it is still waiting for this service's ready ack. */
    if (driver_api->proc_notify_ready != NULL) {
        driver_api->proc_notify_ready();
    }
    if (wasmos_sys_svc_register_native(driver_api, spawn_info.proc_endpoint, g_endpoint,
                                       (const uint8_t*)"net.stack", 9u,
                                       NET_STACK_REGISTER_REQUEST_ID) < 0) {
        return -1;
    }

    if (driver_api->console_write != NULL) {
        static const char msg[] = "[net-stack] registered net.stack\n";
        driver_api->console_write(msg, (int)(sizeof(msg) - 1));
    }

    /* Drain every message before yielding. A service must not assume a later
     * readiness edge will re-signal a request that was already queued; that
     * exact mistake previously deadlocked the virtio-net synchronous control
     * path. */
    for (;;) {
        nd_ipc_message_t request;
        int drained = 0;
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
        net_stack_bind_virtio();
        sys_check_timeouts();
        if (g_netif_ready) {
            net_stack_start_rx_poll();
        }
        if (!drained && driver_api->sched_yield != NULL) {
            driver_api->sched_yield();
        }
    }

    /* Not reached. */
    return 0;
}
