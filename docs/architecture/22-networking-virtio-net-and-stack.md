## Networking via Virtio-Net and User-Space Stack

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
- No explicit NIC model is configured in `run-qemu*`; QEMU defaults are used.
- `virtio-serial` already exists as a PCI-matched WASM service and proves the
  transport pattern for early virtio device bring-up.
- DMA capability plumbing and borrow-buffer DMA lifecycle exist and can be
  reused by `virtio-net` for virtqueue and packet buffer physical addressing.
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
`PROC_IPC_SPAWN_CAPS_V2`):
- `DEVMGR_CAP_IO_PORT`: I/O port range covering PCI config access (0xCF8–0xCFF)
  and BAR0 I/O register window (io_port_min=BAR0_base, io_port_max=BAR0_base+0x1F)
- `DEVMGR_CAP_IRQ`: IRQ line from PCI config 0x3C (typically 11 under QEMU)
- `DEVMGR_CAP_DMA`: BIDIR, covers low memory window for virtqueue and packet
  buffers (initial: 0x100000–0x4000000, i.e., 1 MB–64 MB)

The device manager reads BAR0 and the IRQ line from the PCI device record (already
populated during PCI scan) and includes them in the spawn capability descriptor.

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

Allocate with the DMA borrow path:
```c
int32_t borrow_id = wasmos_buffer_borrow(WASMOS_BUFFER_KIND_NET_QUEUE_RX, drv_ep, WASMOS_BUFFER_GRANT_BIDIR);
uint64_t phys_addr;
int rc = dma_map_borrow(borrow_id, device_id, 0, VQ_ALLOC_SIZE, WASMOS_DMA_DIR_BIDIR, &phys_addr);
```
Then write `phys_addr >> 12` to QUEUE_ADDRESS after selecting the queue index.

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

        /* Forward frame to net-stack via FS buffer + IPC */
        wasmos_sys_fs_buffer_write_to_endpoint(g_stack_endpoint,
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

    /* Copy frame from caller's FS buffer */
    wasmos_sys_fs_buffer_copy_from_endpoint(source,
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

### IPC Opcode Allocation

All networking opcodes occupy the 0xA00–0xBFF range in `wasmos_driver_abi.h`.

```c
enum {
    /* virtio-net driver: 0xA00–0xAFF */
    NETDRV_IPC_LINK_GET          = 0xA00, /* req: –; resp: arg0=link arg2=mtu; MAC in FS buf */
    NETDRV_IPC_TX_FRAME          = 0xA01, /* req: arg0=frame_len; frame in FS buf */
    NETDRV_IPC_RX_POLL           = 0xA02, /* req: –; resp: arg0=frame_len (0=empty); frame in FS buf */
    NETDRV_IPC_STATS_GET         = 0xA03, /* req: –; resp: stats struct in FS buf */
    NETDRV_IPC_RX_FRAME_NOTIFY   = 0xA04, /* push driver→stack: arg0=frame_len; frame in FS buf */
    NETDRV_IPC_RESP              = 0xA80,
    NETDRV_IPC_ERROR             = 0xAFF,

    /* net-stack service: 0xB00–0xBFF */
    NET_IPC_SOCKET_OPEN          = 0xB00, /* arg0=family arg1=type arg2=stack_id arg3=0 */
    NET_IPC_BIND                 = 0xB01, /* arg0=sock_id arg1=port arg2=addr_v4_nbo arg3=0 */
    NET_IPC_CONNECT              = 0xB02, /* arg0=sock_id arg1=port arg2=addr_v4_nbo arg3=0 */
    NET_IPC_SEND                 = 0xB03, /* arg0=sock_id arg1=data_len arg2=flags arg3=0; data in FS buf */
    NET_IPC_RECV                 = 0xB04, /* arg0=sock_id arg1=max_len arg2=flags arg3=0 */
    NET_IPC_CLOSE                = 0xB05, /* arg0=sock_id */
    NET_IPC_POLL                 = 0xB06, /* arg0=sock_id; resp: arg0=readable|writable flags */
    NET_IPC_IFADDR_ADD           = 0xB07, /* arg0=if_idx arg1=pfx_len arg2=origin arg3=state; addr in FS buf */
    NET_IPC_IFADDR_DEL           = 0xB08, /* arg0=addr_handle */
    NET_IPC_IFADDR_LIST          = 0xB09, /* arg0=if_idx; resp: addr list in FS buf */
    NET_IPC_STACK_CREATE         = 0xB0A, /* arg0=flags; resp: arg0=stack_id */
    NET_IPC_STACK_DESTROY        = 0xB0B, /* arg0=stack_id */
    NET_IPC_STACK_SELECT         = 0xB0C, /* arg0=stack_id (sets default for this client endpoint) */
    NET_IPC_DATA_NOTIFY          = 0xB0D, /* push stack→client: arg0=sock_id arg1=bytes_avail */
    NET_IPC_RESP                 = 0xB80,
    NET_IPC_ERROR                = 0xBFF
};
```

Common return codes (packed in arg0 of NET_IPC_ERROR):
```c
enum {
    NET_STATUS_OK         =  0,
    NET_STATUS_WOULD_BLOCK = -1,
    NET_STATUS_INVALID    = -2,
    NET_STATUS_NOT_READY  = -3,
    NET_STATUS_DENIED     = -4,
    NET_STATUS_IO_ERROR   = -5,
    NET_STATUS_QUEUE_FULL = -6,
    NET_STATUS_NO_MEM     = -7,
    NET_STATUS_ADDR_IN_USE = -8,
    NET_STATUS_TIMEOUT    = -9
};
```

---

### Concrete IPC Field Layouts

All arg fields are `int32_t`. Addresses and lengths that exceed 32 bits go
through the FS buffer.

#### NETDRV_IPC_LINK_GET response
```
type=NETDRV_IPC_RESP
arg0 = link_state  (0=down, 1=up)
arg1 = status_word (raw device status reg, for diagnostics)
arg2 = mtu         (1500 for baseline)
arg3 = reserved(0)
FS buf[0..5] = MAC address (6 bytes, network byte order)
```

#### NETDRV_IPC_STATS_GET response
```
type=NETDRV_IPC_RESP
arg0 = 0 (ok)
FS buf = netdrv_stats_t:
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
         For IPv6: arg2=0, address (16 bytes) in FS buf[0..15]
  arg3 = reserved(0)
Response (NET_IPC_RESP): arg0=0
```

#### NET_IPC_CONNECT
```
Request:
  arg0 = socket_id
  arg1 = remote port (host byte order)
  arg2 = remote IPv4 addr (network byte order)
         For IPv6: arg2=0, addr in FS buf[0..15]
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
  FS buf[0..data_len-1] = payload
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
  FS buf[0..bytes_received-1] = payload
```

#### NET_IPC_IFADDR_ADD
```
Request:
  arg0 = if_index (0=first/only interface)
  arg1 = prefix_len
  arg2 = origin (0=static, 1=dhcp, 2=slaac)
  arg3 = state  (0=preferred, 1=tentative, 2=deprecated)
  FS buf[0..3]   = IPv4 address (network byte order), or
  FS buf[0..15]  = IPv6 address (network byte order)
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
    /* Flatten pbuf chain into contiguous FS buffer */
    uint32_t total = 0;
    /* borrow FS buffer, write pbuf payload */
    wasmos_sys_fs_buffer_write_to_endpoint(g_driver_ep, frame_buf, total, 0);
    wasmos_ipc_send(g_driver_ep, g_ep, NETDRV_IPC_TX_FRAME,
                    g_req_id++, (int32_t)total, 0, 0, 0);
    /* wait for NETDRV_IPC_RESP (or poll loop handles it) */
    return ERR_OK;
}

/* Called when NETDRV_IPC_RX_FRAME_NOTIFY arrives */
static void on_rx_frame(int32_t frame_len)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (uint16_t)frame_len, PBUF_POOL);
    wasmos_sys_fs_buffer_copy_from_endpoint(g_driver_ep, p->payload, frame_len, 0);
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
  `g_tx_buf_phys[]`. Data copied through FS buffer on the IPC path.
- `dma_sync_borrow` with `WASMOS_DMA_SYNC_FROM_DEVICE` called after each RX
  completion before reading frame data; `WASMOS_DMA_SYNC_TO_DEVICE` called
  before writing TX descriptor.

Phase C+ (post-baseline optimization):
- Replace per-slot pool with dynamic borrow-per-frame; use
  `dma_map_borrow`/`dma_unmap_borrow` in the TX completion path.
- Eliminate the copy through the FS buffer on the RX path by forwarding the
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
  lookup 'virtio.net' endpoint (retry)
  init lwIP, create default netif, set IP config
  register 'net.stack' with svc registry
  notify_ready()
  loop:
    wasmos_ipc_recv(g_endpoint)
    dispatch message type:
      NETDRV_IPC_RX_FRAME_NOTIFY → on_rx_frame(arg0=len)
      NET_IPC_SOCKET_OPEN        → handle_socket_open(...)
      NET_IPC_BIND               → handle_bind(...)
      NET_IPC_CONNECT            → handle_connect(...)
      NET_IPC_SEND               → handle_send(...)
      NET_IPC_RECV               → handle_recv(...)
      NET_IPC_CLOSE              → handle_close(...)
      NET_IPC_POLL               → handle_poll(...)
      NET_IPC_IFADDR_ADD/DEL/LIST → handle_ifaddr(...)
      NET_IPC_STACK_CREATE/...   → handle_stack_mgmt(...)
      default                    → send NET_IPC_ERROR
    sys_check_timeouts()         /* lwIP timers: ARP, TCP retransmit, etc. */
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
- Add `virtio-net` driver skeleton package and devmgr match rule.
- Implement PCI probe, feature negotiation (MAC + STATUS only), queue init,
  DMA buffer pool allocation, RX/TX loop.
- Register `virtio.net` endpoint via svc registry.

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
- Add minimal TCP connect/listen/accept/send/recv/close behavior.
- Wire `sys_check_timeouts()` periodic tick via RTC/timer service.
- Add timeout/retry/close-path handling and explicit error mapping.
- Add TCP echo client/server smoke tests.

Done gate:
- stable TCP echo in `run-qemu` validation without boot regressions.

Phase 4: IPv6 + multi-address + multi-instance enablement
- Set `LWIP_IPV6 1` in `lwipopts.h`; add NDP + ICMPv6 + SLAAC/static v6 config.
- Enable dual-stack sockets (`AF_INET` + `AF_INET6`) and family-aware
  bind/connect (IPv6 address in FS buffer path).
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
  eliminate FS-buffer copy on RX path.

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
- Risk: FS buffer contention between concurrent TX requests from multiple clients.
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
9. Evaluate DMA fast path (eliminate FS-buffer copy on RX) in phase 5.
