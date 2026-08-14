/* socket.h - the net-stack's socket table: the bookkeeping half of a socket.
 *
 * A socket has two halves.  This table owns the client-facing half — the state
 * machine, the owning endpoint, the addresses, the xfer-buffer/borrow ids and
 * the two SPSC data rings — and knows nothing about lwIP.  The lwIP half (the
 * udp_pcb / altcp_pcb reached through net_socket_t.pcb) is created, driven and
 * torn down by net_stack.c's net_stack_pcb_* helpers.  Every operation below is
 * therefore only one of two steps: net_stack.c pairs each call with the matching
 * pcb call and rolls the table state back if the pcb half fails.
 *
 * A socket_id is the index into net_socket_pool_t.sockets[], so it is stable for
 * the socket's lifetime and is what clients pass in NET_IPC_* arg0.
 *
 * All functions here are non-blocking and take no locks; the net-stack is a
 * single-threaded NO_SYS reactor.  They return WASMOS_ERR_NONE (0) on success
 * and a negative packed net-domain code from abi/errors.yaml otherwise.
 */
#ifndef WASMOS_NET_STACK_SOCKET_H
#define WASMOS_NET_STACK_SOCKET_H

#include <stdint.h>

#include "wasmos/ringbuf.h"
#include "wasmos_driver_abi.h"

/* Sockets per net-stack instance, i.e. the highest valid socket_id plus one.
 * The pool is a fixed inline array in net_socket_pool_t, so this also fixes the
 * pool's size; net_socket_open fails with WASMOS_ERR_NET_NO_MEM once every slot
 * is taken. */
#define NET_SOCKET_MAX 32u

/* Lifecycle of one socket slot.  NET_SOCKET_FREE means the slot is unallocated;
 * every other value implies owner_endpoint is set.  Transitions: FREE -> OPEN
 * (net_socket_open) -> BOUND (bind) -> CONNECTING/CONNECTED (connect) or
 * LISTENING (listen); any state -> FREE (close).  A datagram socket goes
 * straight from OPEN/BOUND to CONNECTED; only a stream socket passes through
 * CONNECTING while the TCP handshake runs.  NET_SOCKET_CLOSING is entered from
 * the lwIP side only (net_stack_tcp_err, after lwIP has already freed the pcb);
 * the slot stays occupied in that state until the client issues NET_IPC_CLOSE. */
typedef enum {
    NET_SOCKET_FREE = 0,
    NET_SOCKET_OPEN,
    NET_SOCKET_BOUND,
    NET_SOCKET_CONNECTING,
    NET_SOCKET_CONNECTED,
    NET_SOCKET_LISTENING,
    /* Rings posted via NET_IPC_ACCEPT, waiting to be paired with an inbound
     * connection on its listener. */
    NET_SOCKET_ACCEPTING,
    NET_SOCKET_CLOSING
} net_socket_state_t;

/* One socket slot.  Zeroed while NET_SOCKET_FREE; net_socket_close memsets it
 * back to that.  The pool index of this slot is the socket_id clients use. */
typedef struct {
    net_socket_state_t state;
    /* The client endpoint that opened the socket.  Every later operation must
     * present the same endpoint or it is refused; 0 is never a valid owner. */
    uint32_t owner_endpoint;
    uint32_t family; /* NET_SOCKET_AF_INET / NET_SOCKET_AF_INET6 */
    uint32_t type;   /* NET_SOCKET_STREAM / NET_SOCKET_DGRAM */
    /* Opaque interface/stack selector copied from the open descriptor; carried
     * for the client and not interpreted here. */
    uint32_t stack_id;
    uint16_t local_port;  /* host byte order, 0 until bound */
    uint16_t remote_port; /* host byte order, 0 until connected */
    /* IPv4 addresses in NETWORK byte order (first octet in the low byte), the
     * form lwIP's ip_addr_set_ip4_u32 and the `ip` tool both use. */
    uint32_t local_addr_v4;
    uint32_t remote_addr_v4;
    /* The client's two data rings, as xfer buffers.  *_buffer_id is the client's
     * buffer handle; *_borrow_id is this service's borrow of it and is what must
     * be passed to xfer_buffer_unmap_borrowed when the socket is closed —
     * net_socket_close does not unmap, its caller does. */
    uint32_t tx_buffer_id;
    uint32_t tx_borrow_id;
    uint32_t rx_buffer_id;
    uint32_t rx_borrow_id;
    /* TCP connect is asynchronous: the reply is deferred until the SYN
     * handshake completes (or fails) in a lwIP callback. Store the pending
     * request id so the callback can answer the original NET_IPC_CONNECT. */
    uint32_t connect_request_id;
    uint8_t connect_pending;
    /* For a NET_SOCKET_ACCEPTING socket: index of the listening socket whose
     * next inbound connection it will be paired with. */
    uint32_t accept_listener_id;
    /* NUL-terminated SNI / verification hostname for a TLS stream socket; empty
     * for plain TCP/UDP. Consumed by net_stack_pcb_open to drive
     * mbedtls_ssl_set_hostname before the handshake; a TLS open with sni_len 0
     * is refused. */
    uint16_t sni_len;
    uint8_t sni[NET_SOCKET_SNI_MAX];
    /* The lwIP half, set by net_stack_pcb_open and cleared by net_stack_tcp_err
     * when lwIP has already freed it.  NULL until the pcb half is opened, and
     * after a fatal TCP error; nothing in this file dereferences or frees it. */
    void* pcb; /* struct udp_pcb* or struct altcp_pcb*, owned by net-stack */
    /* SPSC byte rings overlaid on the borrowed client xfer buffers: tx_ring is
     * written by the client and drained here, rx_ring the other way round.
     * Attached by net_socket_open; the memory belongs to the client. */
    wasmos_ringbuf_t tx_ring;
    wasmos_ringbuf_t rx_ring;
} net_socket_t;

/* The service's whole socket table, one flat array indexed by socket_id. */
typedef struct {
    net_socket_t sockets[NET_SOCKET_MAX];
} net_socket_pool_t;

/* Zero every slot, putting the pool in the all-NET_SOCKET_FREE state.  Does not
 * touch pcbs or rings, so it is a startup call, not a teardown one.  A NULL
 * pool is ignored. */
void net_socket_pool_init(net_socket_pool_t* pool);
/* Claim the first free slot for `owner_endpoint` and attach the client's two
 * rings to it.
 *
 * `descriptor` is borrowed for the call only; it is validated strictly (version
 * and byte count must match the v1 ABI, family must be AF_INET/AF_INET6, type
 * must be STREAM/DGRAM, and all four buffer/borrow ids must be non-zero).
 * `tx_base`/`rx_base` are the mapped bases of those borrowed buffers and must
 * both be non-NULL; the ring headers are read from them, so a malformed ring
 * fails the call.  descriptor->sni is copied into the slot truncated to
 * NET_SOCKET_SNI_MAX-1 bytes and NUL terminated.
 *
 * On success returns WASMOS_ERR_NONE and writes the slot index to
 * *out_socket_id.  Returns WASMOS_ERR_NET_INVALID for a NULL/zero argument, a
 * rejected descriptor, or a ring that will not attach, and
 * WASMOS_ERR_NET_NO_MEM when every slot is in use.  The pcb half is NOT opened
 * here — net_stack.c calls net_stack_pcb_open next and closes the slot again if
 * that fails. */
int32_t net_socket_open(net_socket_pool_t* pool, uint32_t owner_endpoint,
                        const net_socket_open_descriptor_v1_t* descriptor, void* tx_base,
                        void* rx_base, uint32_t* out_socket_id);
/* Record the local address (network byte order) and port (host byte order) and
 * move the socket OPEN -> BOUND.  Returns WASMOS_ERR_NET_INVALID if the id is
 * out of range, the slot is free, `owner_endpoint` does not own it, or the
 * socket is not in NET_SOCKET_OPEN — so a second bind is refused. */
int32_t net_socket_bind(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id,
                        uint16_t port, uint32_t addr_v4);
/* Record the remote address (network byte order) and port (host byte order) and
 * advance the state: a stream socket to NET_SOCKET_CONNECTING (the reply to the
 * client is deferred until the lwIP handshake callback), a datagram socket
 * straight to NET_SOCKET_CONNECTED.  Valid only from OPEN or BOUND; any other
 * state, a foreign owner, or an out-of-range id yields
 * WASMOS_ERR_NET_INVALID.  Does not start the handshake — that is
 * net_stack_pcb_connect. */
int32_t net_socket_connect(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id,
                           uint16_t port, uint32_t addr_v4);
/* Move a BOUND stream socket to NET_SOCKET_LISTENING.  Refuses a datagram
 * socket, an unbound or already-listening socket, a foreign owner and an
 * out-of-range id with WASMOS_ERR_NET_INVALID.  The caller creates the lwIP
 * listen pcb afterwards and rolls the state back to BOUND if that fails. */
int32_t net_socket_listen(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id);
/* Release the slot: flag both rings WASMOS_RINGBUF_FLAG_PEER_CLOSED so the
 * client sees the disconnect, then zero the slot back to NET_SOCKET_FREE.
 * Succeeds from any non-free state.  Returns WASMOS_ERR_NET_DENIED when the id
 * is out of range, the slot is already free, or `owner_endpoint` does not own
 * it.  The caller is responsible for closing the pcb first and for unmapping
 * the two borrows afterwards — read tx_borrow_id/rx_borrow_id BEFORE this call,
 * because it zeroes them. */
int32_t net_socket_close(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id);

#endif
