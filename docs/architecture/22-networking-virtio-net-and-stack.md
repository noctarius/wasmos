## Networking via Virtio-Net and User-Space Stack

> **Documentation status: Mixed reference and proposal.** The virtio-net
> transport baseline is implemented. Net-stack protocol service behavior,
> sockets, TCP, IPv6, and multi-instance support remain future work.

### Goal
Introduce a deterministic, minimal networking baseline for WASMOS using:
- a dedicated `virtio-net` driver process for device transport
- a separate user-space network stack service for L2/L3/L4 protocol state
- IPC-based socket-style APIs for apps/services

This preserves the microkernel split (mechanism in kernel, policy/protocol in
user space) and aligns with the existing device-manager and service-registry
startup model.

### Scope and Non-Goals
In scope:
- explicit QEMU NIC/network configuration for deterministic bring-up
- `virtio-net` PCI driver baseline (feature negotiation, queue init, RX/TX)
- network stack service baseline (ARP, IPv4, ICMP echo, UDP, then TCP)
- IPv6 support as part of the full design (phased delivery after IPv4 baseline)
- multi-address interface model (multiple IPv4 and IPv6 addresses per NIC)
- multi-stack instances (independent network stack contexts for isolation)
- app-facing IPC contract for datagram and stream sockets
- boot-time integration through existing `device-manager` policy rules

Out of scope for initial rollout:
- TLS in kernel or first-pass stack service
- high-throughput offload features (TSO/GSO/GRO/LRO)
- advanced firewall/NAT policy

### Current Baseline
- QEMU run/test targets configure a user-mode network backend and a selectable
  NIC model; `virtio-net-pci` is the default.
- `virtio-serial` already exists as a PCI-matched WASM service and proves the
  probe/register-access pattern for early virtio device bring-up. Note it does
  **not** yet set up virtqueues (no ring allocation or queue-PFN programming),
  so it is a discovery baseline, not a virtqueue-DMA precedent.
- The DMA capability and borrow-buffer DMA lifecycle are implemented. The
  driver-owned pinned `region_alloc` primitive supplies virtqueue ring and
  packet-pool memory; `virtio-net` uses the transport-neutral `vring` core.
- `virtio-net` initializes RX/TX queues, routes the device IRQ, performs an ARP
  smoke exchange through QEMU SLIRP, and offers pull plus notification-hinted
  RX delivery. PCI INTx polarity/trigger configuration remains incomplete, so
  consumers must poll defensively.
- `net-stack` is a native lwIP baseline. It enumerates and subscribes to the
  `net.ifc` class (retaining `virtio.net` lookup as a compatibility fallback), reads its
  MAC/link state, and installs `eth0` with static SLIRP addressing
  `10.0.2.15/24` (gateway `10.0.2.2`). Its linkoutput flattens pbuf chains into
  a driver-granted transfer buffer; RX polling/notifications feed
  `ethernet_input`, and its idle loop runs `sys_check_timeouts()`. Socket
  payload callbacks and TCP handshake delivery remain future work.
- The bootstrap device-manager rule starts `net-stack` at boot. Its service
  registration, `net.ifc` discovery/subscription, `hrng` lookup, and initial link query
  are all asynchronous requests resolved by its control endpoint; startup does
  not perform blocking PM or driver request/reply calls.
- IPC opcode space 0x000–0x9FF is allocated; networking opcodes begin at 0xA00.

---

### QEMU Bring-Up Contract (Normative)
Default developer/test profile should use explicit user-mode networking and
virtio NIC:

```sh
-netdev user,id=net0
-device virtio-net-pci,netdev=net0
```

Policy:
- Never rely on implicit NIC defaults for validation targets.
- Keep a compatibility toggle to boot with a legacy model (`e1000`) when
  bisecting regressions.

---

### Device-Manager Match Rule

Add to `scripts/system/devmgr/rules/default.rules`:
```
SUBSYSTEM=="pci", ATTR{class}=="0x02", ATTR{subclass}=="0x00", ATTR{vendor}=="0x1AF4", RUN+="system/drivers/virtio_net.wap"
```

The rule matches by PCI class 0x02 (Network controller), subclass 0x00
(Ethernet), vendor 0x1AF4 (Red Hat / virtio), covering both legacy device IDs
(0x1000) and modern transitional IDs (0x1041).

Capability profile supplied at spawn (using `PROC_IPC_SPAWN_PATH_CAPS` /
`PROC_IPC_SPAWN_PATH_CAPS_SYNC` / `PROC_IPC_SPAWN_CAPS_V2`):
- `DEVMGR_CAP_IO_PORT`: I/O port range covering PCI config access (0xCF8–0xCFF)
  and BAR0 I/O register window (io_port_min=BAR0_base, io_port_max=BAR0_base+0x1F)
- `DEVMGR_CAP_IRQ`: IRQ line from PCI config 0x3C (typically 11 under QEMU)
- `DEVMGR_CAP_DMA`: BIDIR, covers low memory window for virtqueue and packet
  buffers (initial: 0x100000–0x4000000, i.e., 1 MB–64 MB)

The device manager reads BAR0 and the IRQ line from the PCI device record
(already populated during PCI scan), preserves the driver's declared
`dma.buffer` capability for the rule spawn, and includes the resulting
I/O/IRQ/DMA profile in the spawn capability descriptor.

---

### PCI Probe and BAR0 Layout

`virtio-net` uses the same I/O port-based PCI config-space scan pattern as
`virtio-serial`. BAR0 is type I/O (bit 0 of BAR0 value is 1):

```c
uint32_t bar0 = pci_config_read32(bus, slot, fn, 0x10);
if ((bar0 & 0x1u) == 0u) { /* MMIO BAR, skip for legacy I/O path */ }
uint16_t io_base = (uint16_t)(bar0 & 0xFFFCu);
```

The I/O port space at `io_base` maps the following virtio legacy registers:

| Offset | Width | Access | Register             |
|--------|-------|--------|----------------------|
| 0x00   | 32    | R      | DEVICE_FEATURES      |
| 0x04   | 32    | W      | DRIVER_FEATURES      |
| 0x08   | 32    | R/W    | QUEUE_ADDRESS (PFN)  |
| 0x0C   | 16    | R      | QUEUE_SIZE           |
| 0x0E   | 16    | W      | QUEUE_SELECT         |
| 0x10   | 16    | W      | QUEUE_NOTIFY         |
| 0x12   | 8     | R/W    | DEVICE_STATUS        |
| 0x13   | 8     | R/clr  | ISR_STATUS           |
| 0x14+  | –     | R/W    | device-specific cfg  |

virtio-net device-specific config at offset 0x14:

| Offset | Width | Field          | Condition                    |
|--------|-------|----------------|------------------------------|
| 0x14   | 48    | mac[6]         | always                       |
| 0x1A   | 16    | status         | VIRTIO_NET_F_STATUS set      |
| 0x1C   | 16    | max_vq_pairs   | VIRTIO_NET_F_MQ set          |

Read MAC as 6 individual byte-wide I/O reads at offsets 0x14–0x19.

---

### Feature Negotiation

Feature bits written to DRIVER_FEATURES (0x04) after reading DEVICE_FEATURES
(0x00). Baseline (phase 1) negotiates only:

| Bit | Name                   | Value     | Notes                          |
|-----|------------------------|-----------|--------------------------------|
| 5   | VIRTIO_NET_F_MAC       | (1 << 5)  | device provides MAC            |
| 16  | VIRTIO_NET_F_STATUS    | (1 << 16) | link status field available    |

Do not negotiate: CSUM (0), GUEST_CSUM (1), MRG_RXBUF (15), or any GSO/offload
bits. Keeping negotiated features minimal prevents the device from expecting
extended header fields or ring semantics.

Define these constants in the driver header:
```c
#define VIRTIO_NET_F_CSUM        (1u << 0)
#define VIRTIO_NET_F_GUEST_CSUM  (1u << 1)
#define VIRTIO_NET_F_MAC         (1u << 5)
#define VIRTIO_NET_F_MRG_RXBUF   (1u << 15)
#define VIRTIO_NET_F_STATUS      (1u << 16)

#define VIRTIO_NET_FEATURES_DRIVER (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS)
```

---

### Device Initialization Sequence

```
1.  io_out8(io_base + 0x12, 0)                    /* reset */
2.  io_out8(io_base + 0x12, VIRTIO_STATUS_ACK)    /* = 1 */
3.  io_out8(io_base + 0x12, VIRTIO_STATUS_ACK
                           | VIRTIO_STATUS_DRIVER) /* = 3 */
4.  dev_features = io_in32(io_base + 0x00)
5.  drv_features = dev_features & VIRTIO_NET_FEATURES_DRIVER
6.  io_out32(io_base + 0x04, drv_features)
7.  init_virtqueue(0)                             /* RX queue */
8.  init_virtqueue(1)                             /* TX queue */
9.  read_mac()                                    /* 6 bytes at 0x14–0x19 */
10. io_out8(io_base + 0x12, VIRTIO_STATUS_ACK
                           | VIRTIO_STATUS_DRIVER
                           | VIRTIO_STATUS_DRIVER_OK) /* = 7 */
11. arm_rx_queue()                               /* populate RX descriptors */
12. io_out16(io_base + 0x10, 0)                  /* kick RX queue */
```

DEVICE_STATUS bit definitions:
```c
#define VIRTIO_STATUS_ACK        1u
#define VIRTIO_STATUS_DRIVER     2u
#define VIRTIO_STATUS_DRIVER_OK  4u
#define VIRTIO_STATUS_FAILED   128u
```

If DRIVER_FEATURES read-back indicates an unsupported bit, set FAILED and abort.

---

### Virtqueue Memory Layout

For queue index `q` with chosen size `N` (must be power of 2; read max from
QUEUE_SIZE, cap at 256 for initial baseline):

```
Offset 0:
  Descriptor table: N × 16 bytes         = N × 16

Offset (N × 16), aligned to 2:
  Available ring:  6 + N × 2 bytes

Pad to next 4096-byte boundary:
  Used ring:       6 + N × 8 bytes
```

For N = 256:
- Descriptor table: 4096 bytes
- Available ring:   518 bytes (offset 4096)
- Pad to 8192
- Used ring:        2054 bytes (offset 8192)
- Total allocation: 10246 bytes → allocate 12288 (3 pages) per queue

Allocate ring memory from a **driver-owned pinned DMA region**, not the borrow
path. The borrow path (`wasmos_buffer_borrow` + `dma_map_borrow`) maps a peer's
transient buffer and refuses the caller's own memory, so it cannot back a
persistent ring; and there is no `WASMOS_BUFFER_KIND_NET_QUEUE_*` buffer kind
(only `WASMOS_BUFFER_KIND_XFER` exists today). Use the implemented
`region_alloc` primitive (see [DMA Transfers → Driver-Owned DMA Regions](12-dma-transfers.md)):
```c
/* contiguous, page-aligned, pinned, low-2GB, mapped into linear memory */
struct dma_region r = region_alloc(VQ_ALLOC_PAGES, CACHE_WB, DMA_CAPABLE);
uint64_t phys_addr = r.phys_addr;   /* stable for the driver's lifetime */
```
Then write `phys_addr >> 12` to QUEUE_ADDRESS after selecting the queue index.
Per-packet buffers handed off from a client may still use the borrow path.

Initialization for queue `q`:
```c
io_out16(io_base + 0x0E, q);                      /* QUEUE_SELECT */
uint16_t qsz = io_in16(io_base + 0x0C);           /* QUEUE_SIZE */
if (qsz > 256) qsz = 256;                         /* cap at 256 for baseline */
/* ... allocate aligned memory, get phys_addr ... */
io_out32(io_base + 0x08, (uint32_t)(phys_addr >> 12)); /* QUEUE_ADDRESS */
```

Descriptor entry structure:
```c
typedef struct __attribute__((packed)) {
    uint64_t addr;    /* physical buffer address */
    uint32_t len;     /* buffer length in bytes */
    uint16_t flags;   /* VIRTQ_DESC_F_NEXT=1 | VIRTQ_DESC_F_WRITE=2 */
    uint16_t next;    /* next descriptor index (valid if NEXT set) */
} virtq_desc_t;
```

Available ring structure (driver → device):
```c
typedef struct __attribute__((packed)) {
    uint16_t flags;       /* VIRTQ_AVAIL_F_NO_INTERRUPT=1 */
    uint16_t idx;         /* monotonically increasing; device reads ring[idx % N] */
    uint16_t ring[/* N */];
} virtq_avail_t;
```

Used ring structure (device → driver):
```c
typedef struct __attribute__((packed)) {
    uint16_t flags;       /* VIRTQ_USED_F_NO_NOTIFY=1 */
    uint16_t idx;
    struct { uint32_t id; uint32_t len; } ring[/* N */];
} virtq_used_t;
```

Queue index assignments:
- Queue 0: RX (receiveq0)
- Queue 1: TX (transmitq0)
- Queue 2+: control queues (not used in baseline)

---

### Virtio-Net Packet Header

Every packet in both directions (RX and TX) is prefixed with a
`virtio_net_hdr_t`. Without `VIRTIO_NET_F_MRG_RXBUF`, the header is 10 bytes:

```c
typedef struct __attribute__((packed)) {
    uint8_t  flags;        /* VIRTIO_NET_HDR_F_NEEDS_CSUM=1 (set to 0 for baseline) */
    uint8_t  gso_type;     /* VIRTIO_NET_HDR_GSO_NONE=0 */
    uint16_t hdr_len;      /* set to 0 for baseline */
    uint16_t gso_size;     /* set to 0 for baseline */
    uint16_t csum_start;   /* set to 0 for baseline */
    uint16_t csum_offset;  /* set to 0 for baseline */
} virtio_net_hdr_t;        /* 10 bytes; num_buffers only present with MRG_RXBUF */
```

For TX: driver prepends a zero-filled `virtio_net_hdr_t` to each frame before
placing it in the descriptor.
For RX: driver strips the 10-byte prefix before forwarding to the net-stack.

Buffer size per RX slot: `sizeof(virtio_net_hdr_t) + 1514` = 1524 bytes
(1514 = max Ethernet frame excluding FCS).

---

### Packet Buffer Pool

For the copy-first path (phase A/B), the driver allocates a fixed pool of
pre-mapped DMA buffers at startup rather than per-packet DMA mapping:

```c
#define NET_Q_SIZE      256
#define NET_RX_BUF_SIZE 1524   /* virtio_net_hdr + max Ethernet frame */
#define NET_TX_BUF_SIZE 1524

/* Allocated as one contiguous DMA borrow per pool */
static uint8_t *g_rx_buf_virt;         /* virtual base of RX buffer pool */
static uint64_t g_rx_buf_phys[NET_Q_SIZE]; /* per-slot physical address */
static int32_t  g_rx_borrow_id;

static uint8_t *g_tx_buf_virt;
static uint64_t g_tx_buf_phys[NET_Q_SIZE];
static int32_t  g_tx_borrow_id;
static uint16_t g_tx_free_head;        /* free-list head (descriptor index) */
static uint16_t g_tx_next[NET_Q_SIZE]; /* free-list next pointers */
static uint16_t g_tx_free_count;
```

Pool size: `NET_Q_SIZE × NET_RX_BUF_SIZE` = 256 × 1524 ≈ 381 KB per direction.
Total DMA allocation at driver init: ≈762 KB plus virtqueue rings (≈24 KB).

RX queue is pre-populated: all `NET_Q_SIZE` descriptors point into the pool and
are added to the available ring before writing DRIVER_OK.

TX descriptors are managed with the free-list. On frame send: take from
free-list; on TX used-ring progress: return to free-list.

---

### Memory Barrier Rules

Virtio queue updates require ordering between descriptor writes and ring index
updates. On x86 (TSO model), hardware store ordering is strong, but the
compiler must not reorder. Use a compiler barrier at each ordering point:

```c
/* In WASM-compiled driver code */
#define virtio_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
```

Required ordering:
1. Write all descriptor fields before incrementing `avail->idx`.
2. Read `used->idx` before reading used ring entries.
3. After writing `avail->idx`, issue QUEUE_NOTIFY if the device hasn't disabled
   notifications (`used->flags & VIRTQ_USED_F_NO_NOTIFY == 0`).

---

### RX Processing Loop

Called when the ISR register bit 0 is set (queue interrupt):

```c
static void process_rx_completions(void)
{
    while (g_rx_last_used != g_vq_used_rx->idx) {
        uint32_t slot = g_vq_used_rx->ring[g_rx_last_used % NET_Q_SIZE].id;
        uint32_t full_len = g_vq_used_rx->ring[g_rx_last_used % NET_Q_SIZE].len;
        uint32_t frame_len = full_len - (uint32_t)sizeof(virtio_net_hdr_t);

        uint8_t *frame = g_rx_buf_virt + slot * NET_RX_BUF_SIZE
                         + sizeof(virtio_net_hdr_t);

        /* Forward frame to net-stack via xfer buffer + IPC */
        wasmos_sys_xfer_buffer_write_to_endpoint(g_stack_endpoint,
                                                 frame, (int32_t)frame_len, 0);
        wasmos_ipc_send(g_stack_endpoint, g_endpoint,
                        NETDRV_IPC_RX_FRAME_NOTIFY, g_next_req_id++,
                        (int32_t)frame_len, 0, 0, 0);

        /* Re-arm: put descriptor back in available ring */
        g_vq_desc_rx[slot].addr = g_rx_buf_phys[slot];
        g_vq_desc_rx[slot].len  = NET_RX_BUF_SIZE;
        g_vq_desc_rx[slot].flags = VIRTQ_DESC_F_WRITE;
        g_vq_desc_rx[slot].next  = 0;
        g_vq_avail_rx->ring[g_vq_avail_rx->idx % NET_Q_SIZE] = (uint16_t)slot;
        virtio_mb();
        g_vq_avail_rx->idx++;

        g_stats.rx_packets++;
        g_rx_last_used++;
    }
    /* Kick RX queue if descriptors were re-armed */
    io_out16(g_dev.io_base + VIRTIO_PCI_QUEUE_NOTIFY, VIRTIO_NET_Q_RX);
}
```

---

### TX Path (handling NETDRV_IPC_TX_FRAME)

```c
static void handle_tx_frame(int32_t source, int32_t request_id, int32_t frame_len)
{
    if (frame_len <= 0 || frame_len > 1514) {
        wasmos_ipc_send(source, g_endpoint, NETDRV_IPC_ERROR,
                        request_id, NET_STATUS_INVALID, 0, 0, 0);
        return;
    }
    if (g_tx_free_count == 0) {
        wasmos_ipc_send(source, g_endpoint, NETDRV_IPC_ERROR,
                        request_id, NET_STATUS_QUEUE_FULL, 0, 0, 0);
        return;
    }

    /* Reclaim completed TX descriptors first */
    reclaim_tx_completions();

    uint16_t slot = g_tx_free_head;
    g_tx_free_head = g_tx_next[slot];
    g_tx_free_count--;

    uint8_t *tx_buf = g_tx_buf_virt + slot * NET_TX_BUF_SIZE;
    virtio_net_hdr_t *hdr = (virtio_net_hdr_t *)tx_buf;
    memset(hdr, 0, sizeof(*hdr));

    /* Copy frame from caller's xfer buffer */
    wasmos_sys_xfer_buffer_copy_from_endpoint(source,
                                              tx_buf + sizeof(*hdr),
                                              frame_len, 0);

    g_vq_desc_tx[slot].addr  = g_tx_buf_phys[slot];
    g_vq_desc_tx[slot].len   = (uint32_t)sizeof(*hdr) + (uint32_t)frame_len;
    g_vq_desc_tx[slot].flags = 0; /* read-only, no NEXT */
    g_vq_desc_tx[slot].next  = 0;

    g_vq_avail_tx->ring[g_vq_avail_tx->idx % NET_Q_SIZE] = slot;
    virtio_mb();
    g_vq_avail_tx->idx++;

    io_out16(g_dev.io_base + VIRTIO_PCI_QUEUE_NOTIFY, VIRTIO_NET_Q_TX);

    g_stats.tx_packets++;
    wasmos_ipc_send(source, g_endpoint, NETDRV_IPC_RESP,
                    request_id, NET_STATUS_OK, 0, 0, 0);
}
```

TX completions are reclaimed lazily (before the next TX) by scanning
`g_tx_last_used != g_vq_used_tx->idx` and returning slots to the free-list.

---

### Net-Stack as Interface Broker (planned)

The net-stack service is the networking analog of the filesystem manager: the
single broker between NIC drivers below and applications above. Drivers stay
dumb frame movers; the net-stack owns everything policy.

Layering — who talks to whom:

```
apps ──NET_IPC_* sockets──▶ net-stack ──netdrv IPC──▶ virtio-net driver ──▶ device
        (open/send/recv)     (eth0, IPs, routes,        (dumb frame mover,
                              multiplexes N NICs)         one net.ifc instance)
```

- **Apps never look up drivers.** They open sockets against the net-stack, so no
  application is coupled to `virtio.net` (or any concrete driver). Direct
  driver lookup remains only in low-level driver *conformance tests*
  (`net_smoke`), which is the correct layer for those.
- **The net-stack discovers interfaces by class, not by name.** It calls
  `svc_lookup_class("net.ifc")` and `svc_subscribe_class("net.ifc")` (see
  [Process and IPC → Class-Based Discovery](09-process-and-ipc.md)); each NIC
  driver registers `class="net.ifc"` with an instance index. A second NIC or a
  different driver (e.g. `e1000`) appears as another instance with no net-stack
  change.
- **Interface storage is bounded and explicit.** The initial broker has eight
  fixed interface slots. Each slot owns stable lwIP `netif` storage plus the
  provider endpoint, class instance, carrier state, and binding lifecycle;
  this avoids a global singleton netif while preserving freestanding bounds.
- **Two event streams, two sources.** *Existence* events (a `net.ifc` provider
  registered / unregistered / died) come from the kernel registry's class
  subscription. *Domain* events (link up/down, media change) come from each
  bound driver over its own protocol (`NETDRV_IPC_LINK_NOTIFY`, alongside
  `NETDRV_IPC_LINK_GET`) — the registry stays networking-ignorant.
- **The net-stack owns all policy:** human-facing interface naming (`eth0`),
  address assignment (`NET_IPC_IFADDR_*`), routing, and which interface a flow
  egresses. Drivers expose only a frame in/out path plus link state.

---

### Socket Data Plane — Shared-Memory Ring Transport (planned, canonical)

The app↔net-stack **data plane** is a pair of shared-memory SPSC ring buffers
per socket. Socket payload never travels in an IPC message: IPC carries only the
**control plane** (open/bind/connect/close) and lightweight **doorbells**.

The reusable SPSC implementation is present in
`src/libsys/wasm/include/wasmos/ringbuf.h` and is covered by
`tests/unit/test_ringbuf.c`. The net-stack socket pool and versioned open
descriptor are implemented in `src/services/net_stack/socket.{h,c}` and covered
by `tests/unit/test_net_socket.c`. Connected UDP sockets drain complete TX-ring
datagram records into lwIP `pbuf`s and `udp_sendto`; the UDP receive callback
writes complete RX-ring records and sends `NET_IPC_RX_NOTIFY`. TCP payload
callbacks remain future work.

**Rationale.** A persistent per-socket ring avoids per-datagram borrow/release
churn, generalizes to TCP byte streaming without an ABI change, and keeps the
app-facing contract stable across the UDP→TCP progression. The ring indices
double as flow control, so the producer cannot overrun the consumer regardless
of relative rate.

#### Ownership and direction

- The **app owns both rings** (TX and RX) as xfer-buffer objects and borrows
  both to net-stack at socket setup. net-stack is a pure grantee: it reads TX,
  writes RX. This mirrors, one layer up, how net-stack relates to the driver
  (the *client* owns the buffers; the *server* is a grantee). It also means a
  dead app cascade-revokes net-stack's borrows automatically — net-stack leaks
  nothing.
- **Push, both directions, no polling.** Producer writes payload into its ring
  and rings a doorbell only on an empty→non-empty edge:
  - TX ring: app is producer → doorbell `app → net-stack`.
  - RX ring: net-stack is producer → doorbell `net-stack → app` (the app
    wakeup).
  Because each producer respects the consumer's read index and only writes free
  space, push has **no overwrite hazard** — the ring indices *are* the flow
  control (TCP window backpressure / UDP drop fall out of "ring full").

At open time, the client writes this descriptor into a control transfer buffer,
grants that descriptor read access to net-stack, and sends its buffer/borrow IDs
with `NET_IPC_SOCKET_OPEN`. The descriptor's TX/RX grants remain live for the
socket lifetime:

```c
typedef struct __attribute__((packed)) {
    uint16_t version;  /* NET_SOCKET_OPEN_DESCRIPTOR_VERSION */
    uint16_t bytes;    /* sizeof(net_socket_open_descriptor_v1_t) */
    uint32_t family;
    uint32_t type;
    uint32_t stack_id;
    uint32_t flags;
    uint32_t tx_buffer_id;
    uint32_t tx_borrow_id;
    uint32_t tx_bytes;
    uint32_t rx_buffer_id;
    uint32_t rx_borrow_id;
    uint32_t rx_bytes;
} net_socket_open_descriptor_v1_t;
```

#### Ring layout

Each ring is one xfer-buffer object sized explicitly (**~128 KiB**, not the
2 MiB `XFER_TRANSFER_CAPACITY` default; two rings/socket → ~256 KiB/socket, and
the shmem zone is `[0, 64 MiB)`, so ~256 sockets is the budget). Layout:

```
+0            64-byte header (see below)
+64           data region (capacity bytes, power-of-two)
```

Header (fixed 64 bytes, room reserved deliberately so the ABI is not re-cut for
close/reset/stats later):

```c
typedef struct __attribute__((packed, aligned(64))) {
    uint32_t magic;        /* 'NRNG' */
    uint16_t version;
    uint16_t hdr_bytes;    /* = 64 */
    uint32_t capacity;     /* data-region bytes; power of two */
    uint32_t flags;        /* state: PEER_CLOSED, RESET, OVERFLOW_DROPPED, ... */
    /* producer-owned, on its own cache line to avoid false sharing */
    uint32_t write;        /* free-running; NOT stored modulo */
    uint8_t  _pad_w[28];
    /* consumer-owned, separate cache line */
    uint32_t read;         /* free-running; NOT stored modulo */
    uint8_t  _pad_r[/* to 64 */];
} net_ring_hdr_t;
```

#### Index and framing rules

- **Free-running u32 counters.** `write`/`read` are monotonic and never stored
  modulo; index into the data region with `pos & (capacity - 1)`. `empty =
  (write == read)`; `full = (write - read == capacity)`. Unsigned wrap at 2³² is
  harmless with power-of-two capacity. This removes the read==write full/empty
  ambiguity.
- **Ordering is directional acquire/release, not "a barrier".** Producer:
  write payload → *release*-store `write`. Consumer: *acquire*-load `write` →
  read payload; then *release*-store `read`. Symmetric on the other index. On
  x86 aligned u32 loads/stores are atomic and TSO supplies most ordering, so in
  practice this is compiler barriers plus doing the ops in that order — but the
  ordering discipline is what makes it correct, not the atomicity alone.
- **Framing:** a **byte-oriented ring**, not fixed slots. TCP is a raw byte
  stream (no framing). UDP/datagram sockets prefix each record with a small
  length header inside the ring. Fixed equal-size slots are rejected: they waste
  space and cannot hold a datagram larger than one slot.

#### Doorbells and wakeups

- Two doorbell directions (control-plane IPC messages carrying no payload).
- Edge-triggered with the standard lost-wakeup discipline: the producer
  publishes its index **then** checks was-empty-and-signals; the consumer
  re-checks the index **after** arming its wait. (This codebase has a history
  of IPC lost-wakeup stalls — see `docs/STATUS.md` — so this must be explicit.)

#### Mapping baseline (implemented and validated)

Both endpoints must read/write the **same physical pages** for the life of the
socket. net-stack is a native service with a stable address space, so its
mapping never moves; all the volatility is on the WASM-app side, where the ring
must be overlaid into the app's linear memory. That overlay satisfies the
**pinned shared-window invariant** through the implemented WARP linear
address-space rework — see
[WARP Ring3 Implementation → Linear Memory: Reserve-and-Commit](31-warp-ring3-implementation.md#15--linear-memory-reserve-and-commit-no-relocation)
and [Memory Management → Pinned VA Arena](06-memory-management.md#pinned-va-arena-shmem-rings-and-any-stable-mapping).
The rings are one consumer of that arena, alongside shmem surfaces. The mapping
contract is already a validated baseline for both wasm3 and WARP: shared
buffers remain coherent across app-heap growth, and the fixed mapping is not
relocated. The implementation anchors are the wasm3 in-place linear-memory
growth path (`src/kernel/wasm3/shim.c`) and WARP's pinned shared-window mapping
path (`src/kernel/warp/link.cpp`). Socket-ring work may rely on this baseline;
it must not reopen it as a prerequisite unless either mapping implementation
changes.

### IPC Opcode Allocation

All networking opcodes occupy the 0xA00–0xBFF range in `wasmos_driver_abi.h`.

```c
enum {
    /* virtio-net driver: 0xA00–0xAFF */
    NETDRV_IPC_LINK_GET          = 0xA00, /* req: –; resp: arg0=link arg2=mtu; MAC in xfer buf */
    NETDRV_IPC_TX_FRAME          = 0xA01, /* req: arg0=frame_len; frame in xfer buf */
    NETDRV_IPC_RX_POLL           = 0xA02, /* req: –; resp: arg0=frame_len (0=empty); frame in xfer buf */
    NETDRV_IPC_STATS_GET         = 0xA03, /* req: –; resp: stats struct in xfer buf */
    NETDRV_IPC_RX_FRAME_NOTIFY   = 0xA04, /* push driver→stack: arg0=frame_len; frame in xfer buf */
    NETDRV_IPC_LINK_NOTIFY       = 0xA05, /* planned; push driver→stack: arg0=link up/down */
    NETDRV_IPC_RESP              = 0xA80,
    NETDRV_IPC_ERROR             = 0xAFF,

    /* net-stack service: 0xB00–0xBFF */
    NET_IPC_SOCKET_OPEN          = 0xB00, /* arg0=family arg1=type arg2=stack_id arg3=0 */
    NET_IPC_BIND                 = 0xB01, /* arg0=sock_id arg1=port arg2=addr_v4_nbo arg3=0 */
    NET_IPC_CONNECT              = 0xB02, /* arg0=sock_id arg1=port arg2=addr_v4_nbo arg3=0 */
    NET_IPC_SEND                 = 0xB03, /* arg0=sock_id arg1=data_len arg2=flags arg3=0; data in xfer buf */
    NET_IPC_RECV                 = 0xB04, /* arg0=sock_id arg1=max_len arg2=flags arg3=0 */
    NET_IPC_CLOSE                = 0xB05, /* arg0=sock_id */
    NET_IPC_POLL                 = 0xB06, /* arg0=sock_id; resp: arg0=readable|writable flags */
    NET_IPC_IFADDR_ADD           = 0xB07, /* arg0=if_idx arg1=pfx_len arg2=origin arg3=state; addr in xfer buf */
    NET_IPC_IFADDR_DEL           = 0xB08, /* arg0=addr_handle */
    NET_IPC_IFADDR_LIST          = 0xB09, /* arg0=if_idx; resp: addr list in xfer buf */
    NET_IPC_STACK_CREATE         = 0xB0A, /* arg0=flags; resp: arg0=stack_id */
    NET_IPC_STACK_DESTROY        = 0xB0B, /* arg0=stack_id */
    NET_IPC_STACK_SELECT         = 0xB0C, /* arg0=stack_id (sets default for this client endpoint) */
    NET_IPC_DATA_NOTIFY          = 0xB0D, /* push stack→client: arg0=sock_id arg1=bytes_avail */
    NET_IPC_TX_NOTIFY            = 0xB0E, /* push client→stack: arg0=sock_id */
    NET_IPC_RX_NOTIFY            = 0xB0F, /* push stack→client: arg0=sock_id */
    NET_IPC_IF_SET_STATE         = 0xB10, /* arg0=if_idx arg1=1 up / 0 down (admin state) */
    NET_IPC_DHCP_SET             = 0xB11, /* arg0=if_idx arg1=1 start / 0 stop DHCP client */
    NET_IPC_RESP                 = 0xB80,
    NET_IPC_ERROR                = 0xBFF
};
```

Common return codes (packed in arg0 of NET_IPC_ERROR):
```c
enum {
    NET_STATUS_OK          =  0,
    NET_STATUS_WOULD_BLOCK = -1,
    NET_STATUS_INVALID     = -2,
    NET_STATUS_NOT_READY   = -3,
    NET_STATUS_DENIED      = -4,
    NET_STATUS_IO_ERROR    = -5,
    NET_STATUS_QUEUE_FULL  = -6,
    NET_STATUS_NO_MEM      = -7,
    NET_STATUS_ADDR_IN_USE = -8,
    NET_STATUS_TIMEOUT     = -9
};
```

---

### Concrete IPC Field Layouts

All arg fields are `int32_t`. Addresses and lengths that exceed 32 bits go
through the xfer buffer.

> **Note:** the socket *data-plane* opcodes below (`NET_IPC_SEND`,
> `NET_IPC_RECV`, and their xfer-buffer payload path) are **superseded** by the
> [Socket Data Plane — Shared-Memory Ring Transport](#socket-data-plane--shared-memory-ring-transport-planned-canonical)
> above: payload moves through the per-socket rings, not through these
> messages. The control-plane opcodes (`SOCKET_OPEN`, `BIND`, `CONNECT`,
> `CLOSE`, `IFADDR_*`, `STACK_*`) remain as specified, with `SOCKET_OPEN`
> extended to carry the app-owned TX/RX ring `buffer_id`s. The driver-facing
> `NETDRV_IPC_*` layouts are unaffected.

#### NETDRV_IPC_LINK_GET response
```
type=NETDRV_IPC_RESP
arg0 = link_state  (0=down, 1=up)
arg1 = status_word (raw device status reg, for diagnostics)
arg2 = mtu         (1500 for baseline)
arg3 = reserved(0)
xfer buf[0..5] = MAC address (6 bytes, network byte order)
```

#### NETDRV_IPC_STATS_GET response
```
type=NETDRV_IPC_RESP
arg0 = 0 (ok)
xfer buf = netdrv_stats_t:
  uint32_t rx_packets
  uint32_t tx_packets
  uint32_t rx_drops
  uint32_t tx_drops
  uint32_t rx_errors
  uint32_t tx_errors
```

#### NET_IPC_SOCKET_OPEN
```
Request:
  arg0 = family   (2=AF_INET, 10=AF_INET6)
  arg1 = type     (1=SOCK_STREAM/TCP, 2=SOCK_DGRAM/UDP)
  arg2 = stack_id (0=default)
  arg3 = reserved(0)
Response (NET_IPC_RESP):
  arg0 = socket_id (≥0 on success)
```

#### NET_IPC_BIND
```
Request:
  arg0 = socket_id
  arg1 = port (host byte order; 0=any)
  arg2 = IPv4 address in network byte order (0=INADDR_ANY)
         For IPv6: arg2=0, address (16 bytes) in xfer buf[0..15]
  arg3 = reserved(0)
Response (NET_IPC_RESP): arg0=0
```

#### NET_IPC_CONNECT
```
Request:
  arg0 = socket_id
  arg1 = remote port (host byte order)
  arg2 = remote IPv4 addr (network byte order)
         For IPv6: arg2=0, addr in xfer buf[0..15]
  arg3 = reserved(0)
Response (NET_IPC_RESP): arg0=0
  For TCP: sent after SYN-ACK exchange completes (blocking from client view)
```

#### NET_IPC_SEND
```
Request:
  arg0 = socket_id
  arg1 = data_len (bytes)
  arg2 = flags (0=default, 1=MSG_DONTWAIT)
  arg3 = reserved(0)
  xfer buf[0..data_len-1] = payload
Response (NET_IPC_RESP): arg0=bytes_sent
Error (NET_IPC_ERROR): arg0=NET_STATUS_WOULD_BLOCK | NET_STATUS_IO_ERROR
```

#### NET_IPC_RECV
```
Request:
  arg0 = socket_id
  arg1 = max_len (max bytes caller will accept)
  arg2 = flags (0=non-blocking poll, 1=blocking)
  arg3 = reserved(0)
Response (NET_IPC_RESP):
  arg0 = bytes_received (0 means no data, caller retries)
  xfer buf[0..bytes_received-1] = payload
```

#### NET_IPC_IFADDR_ADD
```
Request:
  arg0 = if_index (0=first/only interface)
  arg1 = prefix_len
  arg2 = origin (0=static, 1=dhcp, 2=slaac)
  arg3 = state  (0=preferred, 1=tentative, 2=deprecated)
  xfer buf[0..3]   = IPv4 address (network byte order), or
  xfer buf[0..15]  = IPv6 address (network byte order)
Response (NET_IPC_RESP): arg0=addr_handle (opaque, used for IFADDR_DEL)
```

---

### Address and Stack-Instance Data Structures

```c
/* Address record (net-stack internal) */
#define NET_ADDR_FAMILY_V4  2
#define NET_ADDR_FAMILY_V6  10

#define NET_ADDR_ORIGIN_STATIC  0
#define NET_ADDR_ORIGIN_DHCP    1
#define NET_ADDR_ORIGIN_SLAAC   2

#define NET_ADDR_STATE_PREFERRED   0
#define NET_ADDR_STATE_TENTATIVE   1
#define NET_ADDR_STATE_DEPRECATED  2

typedef struct {
    uint8_t  in_use;
    uint8_t  family;
    uint8_t  prefix_len;
    uint8_t  origin;
    uint8_t  state;
    uint8_t  is_preferred_src;  /* 1 = default source for this family */
    union {
        uint32_t v4;            /* network byte order */
        uint8_t  v6[16];        /* network byte order */
    } addr;
} net_ifaddr_t;

#define NET_IFADDR_MAX 8        /* per interface */

/* Stack-instance record */
typedef struct {
    uint8_t      in_use;
    uint8_t      if_count;
    net_ifaddr_t addrs[NET_IFADDR_MAX];
    uint8_t      addr_count;
    /* lwIP struct netif netif; — one per instance for initial baseline */
    /* lwIP routing table and neighbor cache per instance */
} net_stack_instance_t;

#define NET_STACK_INSTANCE_MAX 4
```

Initial bring-up uses one default instance (`instance_id=0`). The ABI and
in-memory structure are multi-instance from day one so the later extension is
additive only.

---

### Socket State Machine

Each `net_socket_t` in the stack service tracks:

```c
typedef enum {
    SOCK_STATE_FREE       = 0,
    SOCK_STATE_OPEN       = 1, /* created, no addr */
    SOCK_STATE_BOUND      = 2, /* local addr/port assigned */
    SOCK_STATE_CONNECTED  = 3, /* UDP connected or TCP established */
    SOCK_STATE_LISTENING  = 4, /* TCP listen */
    SOCK_STATE_CLOSING    = 5  /* TCP FIN exchange in progress */
} sock_state_t;

typedef struct {
    sock_state_t  state;
    uint8_t       family;           /* AF_INET or AF_INET6 */
    uint8_t       type;             /* SOCK_STREAM or SOCK_DGRAM */
    int32_t       stack_id;
    int32_t       client_endpoint;  /* endpoint to push DATA_NOTIFY to */
    uint16_t      local_port;
    uint16_t      remote_port;
    uint32_t      local_addr_v4;
    uint32_t      remote_addr_v4;
    uint8_t       local_addr_v6[16];
    uint8_t       remote_addr_v6[16];
    /* lwIP PCB pointer: struct udp_pcb * or struct tcp_pcb * */
} net_socket_t;

#define NET_SOCKET_MAX 32
```

Transitions:
- `OPEN` → `BOUND` via `NET_IPC_BIND`
- `OPEN`/`BOUND` → `CONNECTED` via `NET_IPC_CONNECT` (UDP) or after TCP
  handshake
- `OPEN`/`BOUND` → `LISTENING` via an implicit `NET_IPC_BIND` + future
  `NET_IPC_LISTEN` (phase 3)
- `CONNECTED` → `CLOSING` via `NET_IPC_CLOSE` on TCP
- `CLOSING` → `FREE` after FIN-ACK exchange (TCP) or immediately (UDP)

The stack service pushes `NET_IPC_DATA_NOTIFY` to the `client_endpoint` when
lwIP's receive callback fires. The client then calls `NET_IPC_RECV`.

---

### lwIP Integration Model

The `net-stack` service embeds lwIP compiled with `NO_SYS=1` (cooperative,
no RTOS integration). The netif glue:

```c
/* Called by lwIP to send a frame on the wire */
static err_t lwip_linkoutput(struct netif *nif, struct pbuf *p)
{
    /* Flatten pbuf chain into contiguous xfer buffer */
    uint32_t total = 0;
    /* borrow xfer buffer, write pbuf payload */
    wasmos_sys_xfer_buffer_write_to_endpoint(g_driver_ep, frame_buf, total, 0);
    wasmos_ipc_send(g_driver_ep, g_ep, NETDRV_IPC_TX_FRAME,
                    g_req_id++, (int32_t)total, 0, 0, 0);
    /* wait for NETDRV_IPC_RESP (or poll loop handles it) */
    return ERR_OK;
}

/* Called when NETDRV_IPC_RX_FRAME_NOTIFY arrives */
static void on_rx_frame(int32_t frame_len)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (uint16_t)frame_len, PBUF_POOL);
    wasmos_sys_xfer_buffer_copy_from_endpoint(g_driver_ep, p->payload, frame_len, 0);
    g_netif.input(p, &g_netif);  /* lwIP ethernet_input / ip_input */
}
```

`lwipopts.h` key settings for WASMOS service embedding:
```c
#define NO_SYS               1
#define MEM_LIBC_MALLOC      0     /* use pbuf pools, not malloc */
#define PBUF_POOL_SIZE       128
#define MEMP_NUM_UDP_PCB     16
#define MEMP_NUM_TCP_PCB     16
#define MEMP_NUM_TCP_SEG     64
#define TCP_WND              (4 * TCP_MSS)
#define TCP_SND_BUF          (4 * TCP_MSS)
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_ARP             1
#define LWIP_IPV4            1
#define LWIP_ICMP            1
#define LWIP_UDP             1
#define LWIP_TCP             1
#define LWIP_IPV6            0     /* phase 4: set to 1 */
```

Initial static IP configuration for QEMU user-mode networking:
- IP:      10.0.2.15 / 24
- Gateway: 10.0.2.2
- DNS:     10.0.2.3 (not consumed by stack service in phase 2)

---

### Buffer and DMA Model

Phase A/B (phases 1–3):
- Virtqueue ring memory: one `dma_map_borrow` per queue at driver startup.
  Physical address written to QUEUE_ADDRESS register.
- Packet data: pre-allocated pool of DMA-mapped borrow buffers (one per RX
  slot, one per TX slot). Physical addresses stored in `g_rx_buf_phys[]` and
  `g_tx_buf_phys[]`. Data copied through the xfer buffer on the IPC path.
- `dma_sync_borrow` with `WASMOS_DMA_SYNC_FROM_DEVICE` called after each RX
  completion before reading frame data; `WASMOS_DMA_SYNC_TO_DEVICE` called
  before writing TX descriptor.

Phase C+ (post-baseline optimization):
- Replace per-slot pool with dynamic borrow-per-frame; use
  `dma_map_borrow`/`dma_unmap_borrow` in the TX completion path.
- Eliminate the copy through the xfer buffer on the RX path by forwarding the
  borrow handle directly to the stack and unlocking the descriptor only after
  the stack releases the borrow.

---

### Component Model

#### 1. `virtio-net` Driver Service
Responsibilities:
- discover and bind `virtio-net` PCI function via BAR0 I/O scan
- negotiate features (MAC + STATUS baseline only)
- initialize RX/TX virtqueues and pre-populate RX descriptors
- handle IRQ (ISR read-and-clear at io_base + 0x13)
- publish link/MAC info and packet ingress/egress IPC endpoints
- register as `virtio.net` via `wasmos_svc_register`

Non-responsibilities:
- no TCP retransmission logic
- no ARP/IP routing tables
- no socket lifecycle semantics

#### 2. `net-stack` Service
Responsibilities:
- own protocol state machines and packet classification (via lwIP)
- maintain ARP/NDP, IPv4/IPv6 config, ICMP/ICMPv6, UDP, and TCP state
- expose socket-style IPC to clients
- mediate packet flow to/from `virtio-net` via `NETDRV_IPC_TX_FRAME` and
  `NETDRV_IPC_RX_FRAME_NOTIFY`
- support multiple addresses per interface
- support multiple isolated stack instances with explicit instance selection

Non-responsibilities:
- no direct PCI/virtqueue access
- no privileged hardware config outside driver IPC contract

Isolation note: the `net-stack` ships as a **native `.wap` service** — a C
binary embedding lwIP, linked against `libsys_native` and packed with
`native = true`, loaded like `gfx_compositor`. This avoids compiling lwIP to
WASM and reuses the native-service toolchain. Like all native services today it
runs **ring-0 (interim)**, which is a weak posture given it parses untrusted
network input from an unaudited (subtree) lwIP. That is not net-stack-specific:
the fix is the shared *native service isolation* work (run native services in
ring-3), see
[Native Service Isolation](11-ring3-isolation-and-separation.md#native-service-isolation-planned).

#### 3. Client Apps/Services
Responsibilities:
- use net-stack IPC APIs for open/bind/connect/send/recv/close
- handle `NET_IPC_DATA_NOTIFY` push for incoming data and follow with `NET_IPC_RECV`
- handle explicit non-blocking/retry statuses (`NET_STATUS_WOULD_BLOCK`)

---

### Driver Main Loop Structure

```
initialize():
  probe PCI → get io_base, irq
  init device (reset, ack, features, queues, driver_ok)
  read MAC, populate g_dev
  register 'virtio.net' with svc registry
  lookup 'net.stack' endpoint (retry with backoff)
  notify_ready()
  loop:
    wasmos_ipc_recv(g_endpoint)
    read ISR → if bit0: process_rx_completions()
    reclaim_tx_completions()
    dispatch message type:
      NETDRV_IPC_LINK_GET  → handle_link_get(source, req_id)
      NETDRV_IPC_TX_FRAME  → handle_tx_frame(source, req_id, arg0=len)
      NETDRV_IPC_RX_POLL   → handle_rx_poll(source, req_id)
      NETDRV_IPC_STATS_GET → handle_stats_get(source, req_id)
      default              → send NETDRV_IPC_ERROR
```

The driver reads the ISR register on every recv iteration (not only on IRQ
delivery), because WASM IRQ delivery is mediated by the kernel waking the
process—the driver simply polls the ISR after each wake to check for hardware
events.

---

### Stack Service Main Loop Structure

```
initialize():
  init lwIP
  register 'net.stack' with svc registry
  notify_ready()
  loop:
    drain every pending message from g_endpoint
    dispatch message type:
      NETDRV_IPC_RX_FRAME_NOTIFY  → on_rx_frame(arg0=len)
      NET_IPC_SOCKET_OPEN         → map descriptor/rings, handle_socket_open(...)
      NET_IPC_BIND                → handle_bind(...)
      NET_IPC_CONNECT             → handle_connect(...)
      NET_IPC_SEND                → handle_send(...)
      NET_IPC_RECV                → handle_recv(...)
      NET_IPC_CLOSE               → handle_close(...)
      NET_IPC_POLL                → handle_poll(...)
      NET_IPC_IFADDR_ADD/DEL/LIST → handle_ifaddr(...)
      NET_IPC_STACK_CREATE/...    → handle_stack_mgmt(...)
      default                     → send NET_IPC_ERROR
    sys_check_timeouts()          /* lwIP timers: ARP, TCP retransmit, etc. */
```

`sys_check_timeouts()` is a lwIP function that must be called periodically
(every ~250 ms). Since the event loop blocks on `wasmos_ipc_recv`, the
service needs either a periodic IPC self-wakeup or a timer hostcall to bound
the timeout check interval.  Use an existing RTC or timer service notification
for the periodic tick.

---

### Observability Markers

```
[virtio-net] probe ok bus=%u slot=%u dev=0x%04X irq=%u
[virtio-net] mac %02X:%02X:%02X:%02X:%02X:%02X io=0x%04X
[virtio-net] features dev=0x%08X drv=0x%08X
[virtio-net] queue[%u] size=%u phys_pfn=0x%08X
[virtio-net] queue init fail q=%u rc=%d
[virtio-net] driver ok link=%s mtu=%u
[virtio-net] tx ok len=%u free=%u
[virtio-net] tx queue full free=%u
[virtio-net] rx frame len=%u
[virtio-net] isr bit0 rx completions=%u
[net-stack] netif up ip=10.0.2.15
[net-stack] ifaddr add ok family=%u pfx=%u
[net-stack] socket open ok id=%d family=%u type=%u
[net-stack] arp ok ip=%u.%u.%u.%u
[net-stack] icmp echo ok src=%u.%u.%u.%u
[net-stack] udp recv sock=%d len=%u src=%u.%u.%u.%u:%u
[net-stack] tcp connect ok sock=%d
[net-stack] tcp accept sock=%d
[net-stack] data notify sock=%d avail=%u
```

---

### Stack Implementation Options

#### Option A: `lwIP` (Recommended Initial Path)
Why:
- C-first integration fits current service/driver codebase
- mature IPv4/UDP/TCP behavior with clear raw/netif integration seams
- lower FFI friction for initial WASMOS service integration

Tradeoffs:
- requires disciplined memory/pbuf configuration
- weaker compile-time safety than Rust-first approach

#### Option B: `smoltcp` (Rust-First Alternative)
Why:
- memory-safe stack logic and explicit state-machine style
- good for constrained embedded networking experiments

Tradeoffs:
- Rust service integration and C ABI boundary increase initial complexity
- feature scope differs from full legacy stacks and may require protocol-policy
  adaptations

Decision:
- start with `lwIP` for first end-to-end baseline, then re-evaluate `smoltcp`
  after the driver/IPC contracts are stable.

---

### Rollout Plan

Phase 0: Deterministic platform wiring
- Add explicit QEMU netdev + NIC model in `run-qemu*` targets.
- Add build toggle for `virtio-net` vs `e1000` model selection.
- Add boot markers for NIC/device visibility.

Done gate:
- `run-qemu-test` remains green with explicit NIC config.

Phase 1: `virtio-net` transport baseline
- Phase 1a (probe baseline, DONE): `virtio-net` driver package + devmgr match
  rule; PCI probe, feature negotiation (MAC + STATUS only), MAC/link markers,
  `virtio.net` svc registration, and QEMU NIC wiring. Phase 1b subsequently
  added RX/TX transport.
- Phase 1b (transport): the prerequisite is **not** reusing the existing borrow
  DMA plumbing — it is building (1) the driver-owned pinned DMA region primitive
  ([DMA Transfers](12-dma-transfers.md)) for ring/pool memory, and (2) the
  transport-neutral vring core + PCI backend
  ([Process and IPC](09-process-and-ipc.md)). Then: queue init (program
  QUEUE_ADDRESS from the region's physical base), RX/TX descriptor loops.
  Status: DONE. Both primitives landed (`region_alloc`, vring core); the driver
  `region_alloc`s the RX(0)/TX(1) rings and RX/TX packet pools, lays the rings
  out with the vring core, programs `QUEUE_PFN`, pre-posts RX buffers, and
  handles `NETDRV_IPC_RX_POLL`/`TX_FRAME`. It routes its device IRQ (line 11) to
  its endpoint, and a boot-time ARP probe of the SLIRP gateway proves the full
  path end-to-end with the reply delivered via the interrupt:
  `[virtio-net] irq routed line=11` / `arp request sent` /
  `[virtio-net] irq rx=64 ethertype=0x0806 gw_mac=52:55:0A:00:02:02`
  (guarded by `tests/test_virtio_net_e2e.py`). The IRQ handler reads ISR to
  de-assert the level-triggered line, reaps TX, drains RX, then `irq_ack`s;
  frames are counted and recycled for now, with delivery to a consumer via
  `NETDRV_IPC_RX_FRAME_NOTIFY` coming with the Phase 2 net-stack.

Done gate:
- driver emits MAC/link markers and can TX/RX raw Ethernet frames in smoke path.

Phase 2: net-stack service baseline (L2/L3/ICMP/UDP)
- Add `net-stack` service package and startup policy.
- Integrate lwIP with netif glue.
- Implement ARP + IPv4 + ICMP echo + UDP send/recv via IPC.
- Add simple UDP echo sample app for validation.

Done gate:
- guest ping/UDP echo works on QEMU user-mode network.

Phase 3: TCP baseline
- Client path (DONE): asynchronous `connect` (reply deferred to the lwIP
  `connected`/`err` callback), `send`/`recv` streamed over the client TX/RX byte
  rings (`tcp_write`/`tcp_output` with `tcp_sndbuf` backpressure; inbound
  segments copied into the RX ring and acked via `tcp_recved`, refused with
  `ERR_MEM` under ring pressure), and graceful `close` after detaching
  callbacks. Covered by the SLIRP TCP echo smoke test.
- Server path (PENDING): `listen`/`accept` opcodes and the accept callback that
  mints a new socket/ring pair.
- Timeouts: `sys_check_timeouts()` is advanced from the idle loop; dedicated
  RTC/timer pacing and retransmit/close-path hardening remain to be validated.
- Add explicit error mapping and TCP server smoke tests.

Done gate:
- stable TCP echo in `run-qemu` validation without boot regressions (client path
  met); server-side gate pending listen/accept.

Phase 4: IPv6 + multi-address + multi-instance enablement
- Set `LWIP_IPV6 1` in `lwipopts.h`; add NDP + ICMPv6 + SLAAC/static v6 config.
- Enable dual-stack sockets (`AF_INET` + `AF_INET6`) and family-aware
  bind/connect (IPv6 address in xfer-buffer path).
- Enable multiple addresses per interface with explicit preferred-source rules.
- Enable multiple stack instances with `NET_IPC_STACK_CREATE/SELECT`.

Done gate:
- dual-stack UDP/TCP validation passes with at least two addresses on one NIC
  and at least two isolated stack instances.

Phase 5: hardening + performance
- Add negative-path tests (queue full, malformed frames, link down, stack
  restart).
- Add counters/diagnostics (`netstat`-style endpoint later).
- Optional DMA-backed fast path rollout: forward RX borrow handle to stack,
  eliminate xfer-buffer copy on RX path.

Done gate:
- regression matrix passes and no startup chain liveness regressions.

---

### Validation Matrix
- Baseline boot regression:
  - `cmake --build build --target run-qemu-test`
- Networking smoke (new target, sequential with existing QEMU tests):
  - boot + NIC detect + MAC marker + net-stack register + ICMP echo + UDP echo
- TCP smoke:
  - TCP connect + echo + close
- IPv6 + multi-address smoke:
  - ICMPv6 echo + UDPv6/TCPv6 + multiple addresses on same NIC
- Multi-instance isolation smoke:
  - two stack instances with isolated socket/route state
- Negative behavior:
  - queue saturation returns `NET_STATUS_QUEUE_FULL`
  - link-down path returns `NET_STATUS_NOT_READY`
  - malformed frame path rejected with explicit status
  - driver without `dma.buffer` cap fails at virtqueue init (startup aborts)

---

### Risks and Mitigations
- Risk: large first integration scope causes boot instability.
  - Mitigation: strict phased rollout and isolated networking smoke target.
- Risk: ambiguous ownership between driver and stack.
  - Mitigation: lock clear transport-vs-protocol boundary at IPC opcode level.
- Risk: DMA path introduces hard-to-debug faults early.
  - Mitigation: copy-first correctness path with DMA only for virtqueue rings;
    packet-side DMA deferred to phase 5.
- Risk: lwIP periodic timer not fired, causing TCP retransmit / ARP expiry stall.
  - Mitigation: wire periodic wakeup via RTC service in phase 3; document as
    known gap until then.
- Risk: xfer-buffer contention between concurrent TX requests from multiple clients.
  - Mitigation: net-stack serializes all driver TX calls; one outstanding TX
    frame at a time in phase 1/2 baseline.

---

### Open Decisions
- Final endpoint naming for driver/stack services (`virtio.net`, `net.stack`).
- Static IP first vs DHCP-first in initial user-mode networking profile.
- Whether TCP listen/accept should be in phase 3 baseline or phase 4 hardening.
- Timer wakeup mechanism for `sys_check_timeouts()` (RTC service poll vs new
  timer hostcall).

---

### Task Checklist (Execution Order)
1. Make QEMU NIC settings explicit in all run/test targets.
2. Add IPC opcode block (0xA00–0xBFF) to `wasmos_driver_abi.h`.
3. Add `virtio-net` driver package skeleton: probe, feature negotiation, queue
   init with DMA pool, MAC read, svc register.
4. Land driver RX/TX loop and ISR-based completion processing.
5. Add devmgr PCI match rule and capability grant for virtio-net.
6. Add `net-stack` service with lwIP, ARP/IPv4/ICMP/UDP, socket IPC.
7. Add TCP baseline and timer tick wiring.
8. Add IPv6 + multi-address + multi-stack instance support (phase 4).
9. Evaluate DMA fast path (eliminate xfer-buffer copy on RX) in phase 5.
