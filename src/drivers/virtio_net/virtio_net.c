/* virtio_net.c - Minimal VirtIO network WASM driver.
 * Provides adapter-neutral netdrv IPC for link/status queries while keeping
 * transport details private to this backend. */
#include <stdint.h>
#include <stdio.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos/vring.h"
#include "wasmos_driver_abi.h"

/* PCI identity of a virtio-net function. 0x1AF4 is the Red Hat / virtio vendor
 * id. A legacy device reports 0x1000 (the transitional device id range 0x1000 +
 * subsystem), a modern one 0x1041 (0x1040 + virtio device type 1 = network).
 * Both are accepted; this driver speaks the legacy register interface either
 * way. */
#define VIRTIO_PCI_VENDOR_ID 0x1AF4u
#define VIRTIO_NET_DEV_LEGACY 0x1000u
#define VIRTIO_NET_DEV_TRANSITIONAL 0x1041u

/* Legacy virtio PCI common configuration (virtio 0.9.5, "Virtio Header"), as
 * byte offsets into the device's I/O BAR. Feature negotiation is a 32-bit
 * bitmask each way: the device advertises through DEVICE_FEATURES (read-only)
 * and the driver accepts a subset by writing DRIVER_FEATURES. */
#define VIRTIO_PCI_DEVICE_FEATURES 0x00u
#define VIRTIO_PCI_DRIVER_FEATURES 0x04u
/* Legacy virtqueue registers (no MSI-X: device config starts at 0x14). */
#define VIRTIO_PCI_QUEUE_PFN 0x08u    /* u32: ring PFN (phys >> 12) */
#define VIRTIO_PCI_QUEUE_SIZE 0x0Cu   /* u16: selected queue size (0 = absent) */
#define VIRTIO_PCI_QUEUE_SELECT 0x0Eu /* u16: select the queue to configure */
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10u /* u16: doorbell — write the queue index */
#define VIRTIO_PCI_DEVICE_STATUS 0x12u
#define VIRTIO_PCI_ISR_STATUS 0x13u
/* Enabling MSI-X inserts two 16-bit vector registers at 0x14/0x16 and pushes the
 * device-specific config region from 0x14 to 0x18 (virtio 0.9.5, "MSI-X vector
 * configuration"). Everything below 0x14 is unaffected, so only the
 * device-config base moves — see cfg_base(). */
#define VIRTIO_PCI_MSIX_CONFIG_VECTOR 0x14u /* u16: vector for config changes */
#define VIRTIO_PCI_MSIX_QUEUE_VECTOR 0x16u  /* u16: vector for the selected queue */
#define VIRTIO_NET_CFG_BASE_INTX 0x14u
#define VIRTIO_NET_CFG_BASE_MSIX 0x18u
#define VIRTIO_NET_CFG_MAC_OFF 0x00u
#define VIRTIO_NET_CFG_STATUS_OFF 0x06u
#define VIRTIO_MSIX_NO_VECTOR 0xFFFFu

/* MSI-X table entries this driver uses, one per interrupt source. The entry
 * index names the source, which INTx cannot do: on a shared wire the driver has
 * to inspect the device to find out what happened. */
#define VIRTIO_NET_MSIX_ENTRY_RX 0u
#define VIRTIO_NET_MSIX_ENTRY_TX 1u
#define VIRTIO_NET_MSIX_ENTRY_CONFIG 2u
#define VIRTIO_NET_MSIX_ENTRIES 3u

/* Legacy vring layout alignment (virtio 0.9.5): the used ring must start on a
 * 4096-byte boundary within the queue's contiguous allocation, and the queue's
 * address is programmed as a page frame number, so 4096 is fixed by the spec's
 * page granularity rather than chosen here.
 *
 * Queue indices are fixed by the virtio-net device type: queue 0 is receive,
 * queue 1 is transmit, both named from the DRIVER's point of view. */
#define VIRTIO_PCI_VRING_ALIGN 4096u
#define VIRTIO_NET_RX_QUEUE 0u
#define VIRTIO_NET_TX_QUEUE 1u

/* Legacy virtio-net header (no VIRTIO_NET_F_MRG_RXBUF) prepended to every
 * RX/TX buffer. The device writes it on RX and reads it on TX. */
#define VIRTIO_NET_HDR_LEN 10u
#define VIRTIO_NET_RX_BUF_SIZE 2048u /* hdr + up to 1514-byte Ethernet frame */
#define VIRTIO_NET_RX_BUF_COUNT 64u  /* pre-posted receive buffers */
#define VIRTIO_NET_TX_BUF_SIZE 2048u
#define VIRTIO_NET_TX_BUF_COUNT 64u /* in-flight transmit buffers */
#define VIRTIO_NET_MAX_QUEUE 256u   /* max supported queue size (desc map) */
#define VIRTIO_NET_MAX_FRAME 1514u  /* Ethernet frame w/o FCS */
#define NET_RX_POLL_INTERVAL_MS 10  /* timer-driven RX drain cadence (INTx workaround) */

/* Kernel delivers a routed hardware IRQ as an IPC message of this type, with
 * arg0/request_id = irq line and source = IPC_ENDPOINT_NONE (see
 * src/kernel/arch/x86_64/irq_x86_64.c). The line stays masked until irq_ack.
 * An MSI arrives as WASMOS_IPC_MSI_EVENT_TYPE instead (arg0 = table entry, no
 * ack owed); the two never both apply, because binding an MSI vector sets the
 * device's INTX_DISABLE. */
#define IPC_IRQ_EVENT_TYPE 0xFF00

/* Device-status bits, written to VIRTIO_PCI_DEVICE_STATUS in ascending order as
 * bring-up progresses: ACK ("I see the device"), DRIVER ("I can drive it"),
 * then DRIVER_OK once the queues are live. The bits ACCUMULATE -- each write
 * ORs the next one in, and writing DRIVER_OK alone would retract the earlier
 * acknowledgements. FAILED is the driver telling the device it gave up; a
 * status of 0 is a device reset. */
#define VIRTIO_STATUS_ACK 1u
#define VIRTIO_STATUS_DRIVER 2u
#define VIRTIO_STATUS_DRIVER_OK 4u
#define VIRTIO_STATUS_FAILED 128u

/* virtio-net feature bits this driver understands. F_MAC means the device
 * supplies a MAC address in its config region (without it the driver would have
 * to invent one); F_STATUS means the config region carries a live link-status
 * word. VIRTIO_NET_FEATURES_DRIVER is the mask offered back to the device, so a
 * bit absent here is declined even when the device advertises it -- notably
 * MRG_RXBUF, whose absence is what fixes the RX header at
 * VIRTIO_NET_HDR_LEN bytes. */
#define VIRTIO_NET_F_MAC (1u << 5)
#define VIRTIO_NET_F_STATUS (1u << 16)
#define VIRTIO_NET_FEATURES_DRIVER (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS)

/* Bit 0 of the config status word (valid only when F_STATUS was negotiated):
 * set means the link is up. VIRTIO_NET_MTU_BASELINE is the standard Ethernet
 * payload MTU reported to clients; it is the frame limit
 * (VIRTIO_NET_MAX_FRAME, 1514) less the 14-byte Ethernet header. */
#define VIRTIO_NET_S_LINK_UP 1u
#define VIRTIO_NET_MTU_BASELINE 1500u

typedef struct {
    uint8_t present;
    uint8_t ready;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t irq;
    uint16_t io_base;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t device_features;
    uint32_t driver_features;
    uint16_t status_word;
    uint8_t mac[6];
    uint8_t msix_enabled; /* MSI-X bound: config registers shift, INTx is off */
    uint32_t msix_vectors[VIRTIO_NET_MSIX_ENTRIES]; /* kernel vectors, for release */
} virtio_net_device_t;

static int32_t g_endpoint = -1;
static int32_t g_pci_endpoint = -1; /* pci-bus: owns config space, programs MSI-X */
static virtio_net_device_t g_dev;
static netdrv_stats_t g_stats;

/* One configured virtqueue: the vring core state plus the device queue index
 * (needed by the doorbell notify callback). */
typedef struct {
    vring_t vq;
    uint16_t queue_idx;
    uint8_t ready;
} virtio_net_queue_t;

static virtio_net_queue_t g_rxq; /* queue 0: device -> driver */
static virtio_net_queue_t g_txq; /* queue 1: driver -> device */

/* RX packet pool: a pinned DMA region carved into VIRTIO_NET_RX_BUF_COUNT
 * fixed-size buffers, each posted to the RX queue as a device-writable
 * descriptor. g_rx_pool is the driver's linmem view; g_rx_pool_phys the device
 * address programmed into descriptors. */
static uint8_t* g_rx_pool;
static uint64_t g_rx_pool_phys;

/* TX packet pool. Buffers are handed out from a free stack; g_tx_desc_buf maps
 * an in-flight descriptor id back to its buffer index so completed transmits
 * can be reaped and their buffers returned. */
static uint8_t* g_tx_pool;
static uint64_t g_tx_pool_phys;
static uint16_t g_tx_buf_free[VIRTIO_NET_TX_BUF_COUNT];
static uint32_t g_tx_buf_top;
static uint16_t g_tx_desc_buf[VIRTIO_NET_MAX_QUEUE];
static uint32_t g_tx_completed; /* completed transmits reaped from the used ring */

/* Received-frame delivery. Each consumer that has subscribed (via RX_POLL) gets
 * its OWN queue, and a drained frame is copied into every one of them; RX_POLL
 * then serves the calling consumer from its own queue and posts
 * RX_FRAME_NOTIFY to each subscriber. Before any subscriber exists (e.g. the
 * boot ARP self-probe reply) frames are logged once and dropped.
 *
 * Per-consumer queues rather than one shared ring, because consumers here are
 * independent: net-stack polls on a timer as well as on notification, and a
 * single ring let whichever consumer asked first take a frame another was
 * waiting for -- and a single subscriber slot let the same poll silently
 * reassign the notification channel out from under a second consumer, which is
 * why an app subscribing alongside net-stack never received a push. Depth is
 * halved so two queues cost what one used to. */
#define VIRTIO_NET_RXQ_DEPTH 8u
#define VIRTIO_NET_RX_SUBS_MAX 2u
typedef struct {
    uint16_t len;
    uint8_t data[VIRTIO_NET_MAX_FRAME];
} net_rx_slot_t;
typedef struct {
    int32_t endpoint; /* -1 when the slot is free */
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    net_rx_slot_t slots[VIRTIO_NET_RXQ_DEPTH];
} net_rx_sub_t;
static net_rx_sub_t g_rx_subs[VIRTIO_NET_RX_SUBS_MAX];
static uint8_t g_rx_subs_initialised;
static int32_t g_link_sub_endpoint = -1;

static uint16_t io_read16(uint16_t port) {
    uint16_t value = 0xFFFFu; /* an absent device reads back all-ones */
    (void)wasmos_io_in16((int32_t)port, &value);
    return value;
}

static uint32_t io_read32(uint16_t port) {
    uint32_t value = 0xFFFFFFFFu;
    (void)wasmos_io_in32((int32_t)port, &value);
    return value;
}

static void io_write8(uint16_t port, uint8_t value) {
    (void)wasmos_io_out8((int32_t)port, (int32_t)value);
}

static void io_write16(uint16_t port, uint16_t value) {
    (void)wasmos_io_out16((int32_t)port, (int32_t)value);
}

static void io_write32(uint16_t port, uint32_t value) {
    (void)wasmos_io_out32((int32_t)port, (int32_t)value);
}

static int is_virtio_net_device(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id != VIRTIO_PCI_VENDOR_ID) {
        return 0;
    }
    return device_id == VIRTIO_NET_DEV_LEGACY || device_id == VIRTIO_NET_DEV_TRANSITIONAL;
}

static int parse_hex_n(const char* s, uint32_t digits, uint32_t* out) {
    uint32_t value = 0;
    if (!s || !out || digits == 0u) {
        return -1;
    }
    for (uint32_t i = 0; i < digits; ++i) {
        char ch = s[i];
        uint32_t nibble = 0;
        if (ch >= '0' && ch <= '9') {
            nibble = (uint32_t)(ch - '0');
        } else if (ch >= 'A' && ch <= 'F') {
            nibble = 10u + (uint32_t)(ch - 'A');
        } else if (ch >= 'a' && ch <= 'f') {
            nibble = 10u + (uint32_t)(ch - 'a');
        } else {
            return -1;
        }
        value = (value << 4) | nibble;
    }
    *out = value;
    return 0;
}

static const char* find_token_value(const char* args, const char* key) {
    uint32_t i = 0;
    uint32_t key_len = 0;
    if (!args || !key || key[0] == '\0') {
        return 0;
    }
    while (key[key_len] != '\0') {
        key_len++;
    }
    for (;;) {
        uint32_t j = 0;
        while (args[i] == ' ') {
            i++;
        }
        if (args[i] == '\0') {
            return 0;
        }
        while (key[j] != '\0' && args[i + j] == key[j]) {
            j++;
        }
        if (j == key_len) {
            return &args[i + key_len];
        }
        while (args[i] != '\0' && args[i] != ' ') {
            i++;
        }
    }
}

/* Startup args carry the device identity: pci=BB:SS.FF vendor= device= io= irq=
 * The driver is bound to that device and to no other. */
static int probe_virtio_net_from_startup_args(void) {
    char args[128];
    const char* pci = 0;
    const char* vendor = 0;
    const char* device = 0;
    const char* io = 0;
    const char* irq = 0;
    uint32_t bus = 0, slot = 0, function = 0;
    uint32_t vendor_id = 0, device_id = 0, io_base = 0, irq_line = 0;

    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return -1;
    }
    pci = find_token_value(args, "pci=");
    vendor = find_token_value(args, "vendor=");
    device = find_token_value(args, "device=");
    io = find_token_value(args, "io=");
    irq = find_token_value(args, "irq=");
    if (!pci || !vendor || !device || !io || !irq) {
        return -1;
    }
    if (parse_hex_n(pci, 2u, &bus) != 0 || pci[2] != ':' || parse_hex_n(pci + 3, 2u, &slot) != 0 ||
        pci[5] != '.' || parse_hex_n(pci + 6, 2u, &function) != 0 ||
        parse_hex_n(vendor, 4u, &vendor_id) != 0 || parse_hex_n(device, 4u, &device_id) != 0 ||
        parse_hex_n(io, 4u, &io_base) != 0 || parse_hex_n(irq, 2u, &irq_line) != 0) {
        return -1;
    }
    if (!is_virtio_net_device((uint16_t)vendor_id, (uint16_t)device_id) || io_base == 0u) {
        return -1;
    }
    g_dev.present = 1u;
    g_dev.bus = (uint8_t)bus;
    g_dev.slot = (uint8_t)slot;
    g_dev.function = (uint8_t)function;
    g_dev.io_base = (uint16_t)io_base;
    g_dev.irq = (uint8_t)irq_line;
    g_dev.vendor_id = (uint16_t)vendor_id;
    g_dev.device_id = (uint16_t)device_id;
    return 0;
}

/* Base of the device-specific config region. Enabling MSI-X shifts it by 4 to
 * make room for the config/queue vector registers, so every access to MAC or
 * link status has to go through here rather than a fixed offset. */
static uint16_t cfg_base(void) {
    return g_dev.msix_enabled ? VIRTIO_NET_CFG_BASE_MSIX : VIRTIO_NET_CFG_BASE_INTX;
}

/* The one port read whose failure would otherwise become data: storing the
 * 0xFF of a refused read gives the interface a plausible-looking all-ones MAC.
 * The refusal is reported and bring-up stops instead. */
static int read_mac(void) {
    uint16_t mac_port = (uint16_t)(g_dev.io_base + cfg_base() + VIRTIO_NET_CFG_MAC_OFF);
    for (uint32_t i = 0; i < 6u; ++i) {
        uint8_t byte = 0u;
        int32_t rc = wasmos_io_in8((int32_t)(mac_port + i), &byte);
        if (rc != 0) {
            return rc;
        }
        g_dev.mac[i] = byte;
    }
    return 0;
}

static uint16_t read_status_word(void) {
    return io_read16((uint16_t)(g_dev.io_base + cfg_base() + VIRTIO_NET_CFG_STATUS_OFF));
}

/* vring doorbell: tell the device which queue has new available buffers. */
static void virtio_net_notify(void* user) {
    virtio_net_queue_t* q = (virtio_net_queue_t*)user;
    io_write16(g_dev.io_base + VIRTIO_PCI_QUEUE_NOTIFY, q->queue_idx);
}

/* Configure one virtqueue: select it, read its size, allocate a pinned,
 * contiguous DMA region for the ring, lay the vring out over it, and program
 * the device's QUEUE_PFN with the ring's physical page-frame number. Returns
 * the queue size on success, or -1 if the queue is absent or setup fails. */
static int setup_queue(virtio_net_queue_t* q, uint16_t idx) {
    io_write16(g_dev.io_base + VIRTIO_PCI_QUEUE_SELECT, idx);
    uint16_t qsize = io_read16(g_dev.io_base + VIRTIO_PCI_QUEUE_SIZE);
    if (qsize == 0u || qsize > VIRTIO_NET_MAX_QUEUE) {
        return -1; /* queue absent or larger than we support (desc-map bound) */
    }

    uint64_t ring_bytes = vring_size(qsize, VIRTIO_PCI_VRING_ALIGN);
    int32_t pages = (int32_t)((ring_bytes + 0xFFFu) / 0x1000u);

    uint64_t ring_phys = 0;
    int32_t off = wasmos_region_alloc(pages, WASMOS_REGION_CACHE_WB, &ring_phys);
    if (off < 0) {
        return -1; /* region_alloc failed (cap/window/no-linmem-window) */
    }
    /* In wasm32 a pointer is the linear-memory byte offset region_alloc returned. */
    uint8_t* ring = ptr_cast(uint8_t, (uint32_t)off);
    if (vring_layout(&q->vq, ring, ring_phys, (uint64_t)pages * 0x1000u, qsize,
                     VIRTIO_PCI_VRING_ALIGN) != 0) {
        return -1;
    }
    q->queue_idx = idx;
    vring_set_notify(&q->vq, virtio_net_notify, q);

    /* Legacy transport: hand the device the ring's physical PFN. */
    io_write32(g_dev.io_base + VIRTIO_PCI_QUEUE_PFN, (uint32_t)(ring_phys >> 12));

    /* Bind this queue to its own MSI-X entry, so a completion on it raises a
     * vector that names the queue. The register only exists while MSI-X is
     * enabled, and QUEUE_SELECT above still points at this queue. Virtio signals
     * refusal through the readback, not a status bit. */
    if (g_dev.msix_enabled) {
        uint16_t entry = (idx == (uint16_t)VIRTIO_NET_RX_QUEUE) ? VIRTIO_NET_MSIX_ENTRY_RX
                                                                : VIRTIO_NET_MSIX_ENTRY_TX;
        io_write16(g_dev.io_base + VIRTIO_PCI_MSIX_QUEUE_VECTOR, entry);
        if (io_read16(g_dev.io_base + VIRTIO_PCI_MSIX_QUEUE_VECTOR) == VIRTIO_MSIX_NO_VECTOR) {
            (void)printf("[virtio-net] msix queue vector refused q=%u\n", (unsigned)idx);
            return -1;
        }
    }
    q->ready = 1u;
    return (int)qsize;
}

/* Allocate the RX packet pool and post every buffer to the RX queue as a
 * device-writable descriptor, then kick so the device can start filling them.
 * Returns the number of buffers posted, or -1 on failure. */
static int rx_arm(void) {
    int32_t pages =
        (int32_t)(((uint64_t)VIRTIO_NET_RX_BUF_COUNT * VIRTIO_NET_RX_BUF_SIZE + 0xFFFu) / 0x1000u);
    uint64_t phys = 0;
    int32_t off = wasmos_region_alloc(pages, WASMOS_REGION_CACHE_WB, &phys);
    if (off < 0) {
        return -1;
    }
    g_rx_pool = ptr_cast(uint8_t, (uint32_t)off);
    g_rx_pool_phys = phys;

    for (uint32_t i = 0; i < VIRTIO_NET_RX_BUF_COUNT; ++i) {
        int32_t d = vring_alloc_desc(&g_rxq.vq, phys + (uint64_t)i * VIRTIO_NET_RX_BUF_SIZE,
                                     VIRTIO_NET_RX_BUF_SIZE, VRING_DESC_F_WRITE);
        if (d < 0) {
            return -1;
        }
        vring_publish(&g_rxq.vq, (uint16_t)d);
    }
    vring_kick(&g_rxq.vq);
    return (int)VIRTIO_NET_RX_BUF_COUNT;
}

static void rx_subs_init(void) {
    if (g_rx_subs_initialised) {
        return;
    }
    for (uint32_t i = 0; i < VIRTIO_NET_RX_SUBS_MAX; ++i) {
        g_rx_subs[i].endpoint = -1;
        g_rx_subs[i].head = 0;
        g_rx_subs[i].tail = 0;
        g_rx_subs[i].count = 0;
    }
    g_rx_subs_initialised = 1u;
}

/* The subscriber record for `endpoint`, claiming a free slot on first use.
 * Returns 0 when every slot is taken by a different consumer; the caller still
 * gets its RX_POLL answered, it just receives no pushes. */
static net_rx_sub_t* rx_sub_for(int32_t endpoint) {
    rx_subs_init();
    for (uint32_t i = 0; i < VIRTIO_NET_RX_SUBS_MAX; ++i) {
        if (g_rx_subs[i].endpoint == endpoint) {
            return &g_rx_subs[i];
        }
    }
    for (uint32_t i = 0; i < VIRTIO_NET_RX_SUBS_MAX; ++i) {
        if (g_rx_subs[i].endpoint < 0) {
            g_rx_subs[i].endpoint = endpoint;
            g_rx_subs[i].head = 0;
            g_rx_subs[i].tail = 0;
            g_rx_subs[i].count = 0;
            return &g_rx_subs[i];
        }
    }
    return 0;
}

static int rx_subs_any(void) {
    rx_subs_init();
    for (uint32_t i = 0; i < VIRTIO_NET_RX_SUBS_MAX; ++i) {
        if (g_rx_subs[i].endpoint >= 0) {
            return 1;
        }
    }
    return 0;
}

/* Copy a received frame into every subscriber's queue. A full queue drops for
 * that consumer alone, so a stalled reader cannot blind the others. */
static void rx_queue_push(const uint8_t* frame, uint16_t len) {
    rx_subs_init();
    if (len > VIRTIO_NET_MAX_FRAME) {
        len = VIRTIO_NET_MAX_FRAME;
    }
    for (uint32_t i = 0; i < VIRTIO_NET_RX_SUBS_MAX; ++i) {
        net_rx_sub_t* sub = &g_rx_subs[i];
        if (sub->endpoint < 0) {
            continue;
        }
        if (sub->count >= VIRTIO_NET_RXQ_DEPTH) {
            g_stats.rx_drops++;
            continue;
        }
        net_rx_slot_t* slot = &sub->slots[sub->tail];
        slot->len = len;
        __builtin_memcpy(slot->data, frame, len);
        sub->tail = (sub->tail + 1u) % VIRTIO_NET_RXQ_DEPTH;
        sub->count++;
    }
}

/* Pop the oldest frame queued for one subscriber into out (up to max bytes).
 * Returns its length, or 0 if that subscriber has nothing queued. */
static uint16_t rx_queue_pop(net_rx_sub_t* sub, uint8_t* out, uint32_t max) {
    if (!sub || sub->count == 0u) {
        return 0;
    }
    net_rx_slot_t* slot = &sub->slots[sub->head];
    uint16_t len = slot->len;
    if (len > max) {
        len = (uint16_t)max;
    }
    __builtin_memcpy(out, slot->data, len);
    sub->head = (sub->head + 1u) % VIRTIO_NET_RXQ_DEPTH;
    sub->count--;
    return len;
}

/* Allocate the TX packet pool and initialise the free-buffer stack. */
static int tx_arm(void) {
    int32_t pages =
        (int32_t)(((uint64_t)VIRTIO_NET_TX_BUF_COUNT * VIRTIO_NET_TX_BUF_SIZE + 0xFFFu) / 0x1000u);
    uint64_t phys = 0;
    int32_t off = wasmos_region_alloc(pages, WASMOS_REGION_CACHE_WB, &phys);
    if (off < 0) {
        return -1;
    }
    g_tx_pool = ptr_cast(uint8_t, (uint32_t)off);
    g_tx_pool_phys = phys;
    for (uint32_t i = 0; i < VIRTIO_NET_TX_BUF_COUNT; ++i) {
        g_tx_buf_free[i] = (uint16_t)i;
    }
    g_tx_buf_top = VIRTIO_NET_TX_BUF_COUNT;
    return 0;
}

/* Reclaim completed transmits: free each used descriptor, return its buffer to
 * the free stack, and count the completion. */
static void tx_reap(void) {
    uint32_t used_len = 0;
    int32_t id;
    while ((id = vring_get_used(&g_txq.vq, &used_len)) >= 0) {
        uint16_t b = g_tx_desc_buf[id];
        if (b < VIRTIO_NET_TX_BUF_COUNT && g_tx_buf_top < VIRTIO_NET_TX_BUF_COUNT) {
            g_tx_buf_free[g_tx_buf_top++] = b;
        }
        vring_free_desc(&g_txq.vq, (uint16_t)id);
        g_tx_completed++;
    }
}

/* Take a free TX buffer; returns its index or -1 if the pool is exhausted. */
static int32_t tx_take_buf(void) {
    tx_reap();
    if (g_tx_buf_top == 0u) {
        return -1;
    }
    return (int32_t)g_tx_buf_free[--g_tx_buf_top];
}

/* Post TX buffer b (already holding hdr+payload of total_len) as a device-
 * readable descriptor and kick. On ring-full, returns the buffer to the free
 * stack and returns -1. */
static int tx_post(uint16_t b, uint32_t total_len) {
    int32_t d = vring_alloc_desc(&g_txq.vq, g_tx_pool_phys + (uint64_t)b * VIRTIO_NET_TX_BUF_SIZE,
                                 total_len, 0 /* device-readable */);
    if (d < 0) {
        g_tx_buf_free[g_tx_buf_top++] = b;
        return -1;
    }
    g_tx_desc_buf[d] = (uint16_t)b;
    vring_publish(&g_txq.vq, (uint16_t)d);
    vring_kick(&g_txq.vq);
    g_stats.tx_packets++;
    return 0;
}

/* Transmit one Ethernet frame from the client's granted buffer. WASMOS_ERR_NET_*. */
static int tx_send(int32_t buffer_id, int32_t frame_len) {
    if (frame_len <= 0 || frame_len > (int32_t)VIRTIO_NET_MAX_FRAME) {
        return WASMOS_ERR_NET_INVALID;
    }
    int32_t b = tx_take_buf();
    if (b < 0) {
        return WASMOS_ERR_NET_QUEUE_FULL;
    }
    uint8_t* buf = g_tx_pool + (uint64_t)b * VIRTIO_NET_TX_BUF_SIZE;
    for (uint32_t i = 0; i < VIRTIO_NET_HDR_LEN; ++i) {
        buf[i] = 0;
    }
    if (wasmos_sys_buffer_read(buffer_id, buf + VIRTIO_NET_HDR_LEN, frame_len, 0) != 0) {
        g_tx_buf_free[g_tx_buf_top++] = (uint16_t)b; /* return the buffer */
        return WASMOS_ERR_NET_IO_ERROR;
    }
    if (tx_post((uint16_t)b, VIRTIO_NET_HDR_LEN + (uint32_t)frame_len) != 0) {
        return WASMOS_ERR_NET_QUEUE_FULL;
    }
    return WASMOS_ERR_NONE;
}

/* Transmit a driver-local frame (used by the ARP self-probe). Returns 0 or -1. */
static int tx_post_local(const uint8_t* frame, uint32_t frame_len) {
    int32_t b = tx_take_buf();
    if (b < 0) {
        return -1;
    }
    uint8_t* buf = g_tx_pool + (uint64_t)b * VIRTIO_NET_TX_BUF_SIZE;
    for (uint32_t i = 0; i < VIRTIO_NET_HDR_LEN; ++i) {
        buf[i] = 0;
    }
    __builtin_memcpy(buf + VIRTIO_NET_HDR_LEN, frame, frame_len);
    return tx_post((uint16_t)b, VIRTIO_NET_HDR_LEN + frame_len);
}

/* Drain one completed RX frame into a local buffer (self-probe path). Returns
 * the frame length (> 0) or 0 if none pending; recycles the descriptor. */
static int rx_poll_local(uint8_t* out, uint32_t max) {
    uint32_t used_len = 0;
    int32_t id = vring_get_used(&g_rxq.vq, &used_len);
    if (id < 0) {
        return 0;
    }
    int frame_len = 0;
    uint64_t addr = g_rxq.vq.desc[id].addr;
    uint32_t bidx = (uint32_t)((addr - g_rx_pool_phys) / VIRTIO_NET_RX_BUF_SIZE);
    if (bidx < VIRTIO_NET_RX_BUF_COUNT && used_len > VIRTIO_NET_HDR_LEN &&
        used_len <= VIRTIO_NET_RX_BUF_SIZE) {
        uint32_t flen = used_len - VIRTIO_NET_HDR_LEN;
        if (flen > max) {
            flen = max;
        }
        uint8_t* buf = g_rx_pool + (uint64_t)bidx * VIRTIO_NET_RX_BUF_SIZE;
        __builtin_memcpy(out, buf + VIRTIO_NET_HDR_LEN, flen);
        g_stats.rx_packets++;
        frame_len = (int)flen;
    }
    vring_publish(&g_rxq.vq, (uint16_t)id);
    vring_kick(&g_rxq.vq);
    return frame_len;
}

/* Connectivity probe: broadcast an ARP request for the SLIRP gateway (10.0.2.2)
 * from the device MAC. The reply is delivered asynchronously via the device IRQ (see
 * net_handle_irq). Exercises the whole path — region_alloc'd rings, vring
 * publish/kick, the device doorbell, and TX DMA read. */
static void net_probe_send(void) {
    static const uint8_t sender_ip[4] = {10, 0, 2, 15}; /* SLIRP default guest IP */
    static const uint8_t target_ip[4] = {10, 0, 2, 2};  /* SLIRP gateway */
    uint8_t frame[42];
    uint32_t p = 0;
    int i;

    /* Ethernet header: dst broadcast, src the device MAC, ethertype ARP. */
    for (i = 0; i < 6; ++i)
        frame[p++] = 0xFFu;
    for (i = 0; i < 6; ++i)
        frame[p++] = g_dev.mac[i];
    frame[p++] = 0x08u;
    frame[p++] = 0x06u;
    /* ARP request: Ethernet/IPv4, oper=1. */
    frame[p++] = 0x00u;
    frame[p++] = 0x01u; /* htype */
    frame[p++] = 0x08u;
    frame[p++] = 0x00u; /* ptype */
    frame[p++] = 0x06u;
    frame[p++] = 0x04u; /* hlen, plen */
    frame[p++] = 0x00u;
    frame[p++] = 0x01u; /* oper=request */
    for (i = 0; i < 6; ++i)
        frame[p++] = g_dev.mac[i]; /* sender MAC */
    for (i = 0; i < 4; ++i)
        frame[p++] = sender_ip[i]; /* sender IP */
    for (i = 0; i < 6; ++i)
        frame[p++] = 0x00u; /* target MAC (unknown) */
    for (i = 0; i < 4; ++i)
        frame[p++] = target_ip[i]; /* target IP */

    if (tx_post_local(frame, p) != 0) {
        (void)printf("[virtio-net] arp probe tx failed\n");
        return;
    }
    (void)printf("[virtio-net] arp request sent len=%u\n", (unsigned)p);
}

static uint8_t g_irq_rx_logged; /* one-shot: log the first pre-subscriber frame */

/* Drain completed RX frames out of the vring: enqueue for a subscriber, or (pre-
 * subscription) log the first one and drop. Returns the number enqueued. Shared
 * by the IRQ handler (push) and RX_POLL (pull). */
static int net_drain_rx(void) {
    uint8_t frame[VIRTIO_NET_MAX_FRAME];
    int enqueued = 0;
    int n;
    while ((n = rx_poll_local(frame, sizeof frame)) > 0) {
        if (rx_subs_any()) {
            rx_queue_push(frame, (uint16_t)n);
            enqueued++;
        } else if (!g_irq_rx_logged) {
            g_irq_rx_logged = 1u;
            unsigned et = ((unsigned)frame[12] << 8) | (unsigned)frame[13];
            (void)printf("[virtio-net] irq rx=%d ethertype=0x%04X "
                         "gw_mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                         n, et, frame[6], frame[7], frame[8], frame[9], frame[10], frame[11]);
        }
    }
    return enqueued;
}

/* Tell the RX subscriber, if there is one, how many frames are queued for it.
 * Fire-and-forget: one notify per drained batch, not per frame. */
static void net_notify_subscriber(void) {
    rx_subs_init();
    for (uint32_t i = 0; i < VIRTIO_NET_RX_SUBS_MAX; ++i) {
        const net_rx_sub_t* sub = &g_rx_subs[i];
        if (sub->endpoint < 0 || sub->count == 0u) {
            continue;
        }
        (void)wasmos_ipc_send(sub->endpoint, g_endpoint, NETDRV_IPC_RX_FRAME_NOTIFY, 0,
                              (int32_t)sub->count, 0, 0, 0);
    }
}

/* Reap completed TX, drain the RX used-ring, and notify a subscriber if any
 * frames were queued. Shared by the device IRQ handler and the timer-driven RX
 * poll (see the main loop). */
static void net_service_rx(void) {
    tx_reap();
    if (net_drain_rx()) {
        net_notify_subscriber();
    }
}

/* Device interrupt handler (IPC_IRQ_EVENT_TYPE for this device's line). Reading
 * the ISR status register de-asserts the (level-triggered) device interrupt;
 * the queues are then serviced and irq_ack unmasks.
 *
 * NOTE: on QEMU's legacy PCI-INTx path this IRQ reliably fires only for the
 * FIRST assertion — QEMU's emulated I/O APIC does not re-deliver a reasserted
 * level line on a steadily-unmasked RTE (confirmed via -d int: vector is
 * injected once, never again). Continuous RX therefore relies on the
 * timer-driven poll in the main loop; this handler covers the first frame and
 * any that happen to coincide with the mask->unmask window.
 *
 * This path is the FALLBACK, taken only when the device has no usable MSI-X
 * (see net_setup_msix): message-signalled vectors bypass the I/O APIC pin,
 * Remote-IRR and level re-sample entirely, so they re-deliver per notification. */
static void net_handle_irq(void) {
    /* Ack the device: reading ISR clears its interrupt-asserted bit. The value
     * itself is not wanted -- performing the read is the ack. */
    uint8_t isr = 0u;
    (void)wasmos_io_in8((int32_t)(g_dev.io_base + VIRTIO_PCI_ISR_STATUS), &isr);
    net_service_rx();
    /* Unmask the line now the device register has been read. */
    (void)wasmos_irq_ack((int32_t)g_dev.irq);
}

static void net_publish_link_change(void);

/* MSI event: arg0 names the source directly, so there is no ISR register to read
 * and nothing to ack — the vector is edge-triggered and owned by this driver
 * alone. */
static void net_handle_msi(int32_t entry) {
    if ((uint32_t)entry == VIRTIO_NET_MSIX_ENTRY_CONFIG) {
        net_publish_link_change();
        return;
    }
    /* RX and TX completions both come down to draining the used rings; the
     * distinct vectors keep the door open for handling them separately. */
    net_service_rx();
}

/* Ask pci-bus what interrupt style this function supports and, if it is MSI-X,
 * take one kernel vector per source and have pci-bus write them into the
 * device's table.
 *
 * The three-way split is deliberate. The kernel owns the vector namespace and
 * will only bind a vector to an endpoint the caller owns, so it must be this
 * driver that calls wasmos_msi_alloc. Writing the table is a config-space and
 * BAR access this driver has no capability for, so pci-bus does that half. And
 * telling the device which vector serves which queue is a virtio register write
 * in this driver's own I/O window, so that half is back here.
 *
 * Must run after feature negotiation and BEFORE any device-config or queue
 * access: enabling MSI-X moves the config region (cfg_base) and adds the queue
 * vector register the queue setup writes. Returns 0 when MSI-X is live. */
static int net_setup_msix(void) {
    if (g_pci_endpoint < 0) {
        return -1;
    }
    uint32_t bdf =
        ((uint32_t)g_dev.bus << 8) | ((uint32_t)g_dev.slot << 3) | (uint32_t)g_dev.function;
    wasmos_ipc_message_t reply;

    if (wasmos_ipc_call(g_pci_endpoint, g_endpoint, PCI_IPC_MSI_QUERY, 1, (int32_t)bdf, 0, 0, 0,
                        &reply) != 0 ||
        reply.type != PCI_IPC_RESP) {
        return -1;
    }
    if (reply.arg0 != WASMOS_PCI_MSI_KIND_MSIX || (uint32_t)reply.arg1 < VIRTIO_NET_MSIX_ENTRIES) {
        /* Plain MSI would work for a single source, but virtio needs a distinct
         * vector per queue to be worth the switch — fall back to INTx. */
        return -1;
    }

    uint32_t bound = 0;
    for (uint32_t entry = 0; entry < VIRTIO_NET_MSIX_ENTRIES; ++entry) {
        wasmos_msi_desc_t desc;
        g_dev.msix_vectors[entry] = 0;
        if (wasmos_msi_alloc(g_endpoint, &desc) != 0) {
            break;
        }
        g_dev.msix_vectors[entry] = desc.vector;
        int32_t arg0 = (int32_t)((bdf << 8) | entry);
        if (wasmos_ipc_call(g_pci_endpoint, g_endpoint, PCI_IPC_MSI_BIND, (int32_t)(2u + entry),
                            arg0, (int32_t)desc.address_lo, (int32_t)desc.address_hi,
                            (int32_t)desc.data, &reply) != 0 ||
            reply.type != PCI_IPC_RESP) {
            (void)wasmos_msi_free((int32_t)desc.vector);
            g_dev.msix_vectors[entry] = 0;
            break;
        }
        bound++;
    }
    if (bound < VIRTIO_NET_MSIX_ENTRIES) {
        /* All or nothing: a partially programmed table would leave some sources
         * silently undelivered, which is worse than plain INTx. */
        for (uint32_t entry = 0; entry < bound; ++entry) {
            int32_t arg0 = (int32_t)((bdf << 8) | entry);
            (void)wasmos_ipc_call(g_pci_endpoint, g_endpoint, PCI_IPC_MSI_UNBIND, 100, arg0, 0, 0,
                                  0, &reply);
            (void)wasmos_msi_free((int32_t)g_dev.msix_vectors[entry]);
            g_dev.msix_vectors[entry] = 0;
        }
        return -1;
    }

    g_dev.msix_enabled = 1u;
    /* Tell the device which entry serves config changes. Virtio requires the
     * readback: the device reports refusal by returning NO_VECTOR. */
    io_write16(g_dev.io_base + VIRTIO_PCI_MSIX_CONFIG_VECTOR, VIRTIO_NET_MSIX_ENTRY_CONFIG);
    if (io_read16(g_dev.io_base + VIRTIO_PCI_MSIX_CONFIG_VECTOR) == VIRTIO_MSIX_NO_VECTOR) {
        (void)printf("[virtio-net] msix config vector refused\n");
    }
    return 0;
}

static int initialize_device(void) {
    uint8_t status = 0u;
    if (!g_dev.present || g_dev.io_base == 0u) {
        return -1;
    }

    io_write8(g_dev.io_base + VIRTIO_PCI_DEVICE_STATUS, 0u);
    status = VIRTIO_STATUS_ACK;
    io_write8(g_dev.io_base + VIRTIO_PCI_DEVICE_STATUS, status);
    status |= VIRTIO_STATUS_DRIVER;
    io_write8(g_dev.io_base + VIRTIO_PCI_DEVICE_STATUS, status);

    g_dev.device_features = io_read32(g_dev.io_base + VIRTIO_PCI_DEVICE_FEATURES);
    g_dev.driver_features = g_dev.device_features & VIRTIO_NET_FEATURES_DRIVER;
    io_write32(g_dev.io_base + VIRTIO_PCI_DRIVER_FEATURES, g_dev.driver_features);

    if ((g_dev.driver_features & VIRTIO_NET_F_MAC) == 0u) {
        io_write8(g_dev.io_base + VIRTIO_PCI_DEVICE_STATUS,
                  (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }

    /* Before the first config or queue register access: enabling MSI-X changes
     * where both of those live. */
    if (net_setup_msix() == 0) {
        (void)printf("[virtio-net] msix enabled vectors rx=%u tx=%u cfg=%u\n",
                     (unsigned)g_dev.msix_vectors[VIRTIO_NET_MSIX_ENTRY_RX],
                     (unsigned)g_dev.msix_vectors[VIRTIO_NET_MSIX_ENTRY_TX],
                     (unsigned)g_dev.msix_vectors[VIRTIO_NET_MSIX_ENTRY_CONFIG]);
    } else {
        (void)printf("[virtio-net] msix unavailable; falling back to intx line=%u\n",
                     (unsigned)g_dev.irq);
    }

    int mac_rc = read_mac();
    if (mac_rc != 0) {
        (void)printf("[virtio-net] mac read refused rc=%d\n", mac_rc);
        io_write8(g_dev.io_base + VIRTIO_PCI_DEVICE_STATUS,
                  (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return mac_rc;
    }
    if ((g_dev.driver_features & VIRTIO_NET_F_STATUS) != 0u) {
        g_dev.status_word = read_status_word();
    } else {
        g_dev.status_word = 0u;
    }
    /* Set up the RX and TX virtqueues over driver-owned pinned DMA regions
     * before signalling DRIVER_OK, so the device sees valid rings once live.
     * This only lays out the rings and programs their physical addresses; the
     * buffers are posted by rx_arm/tx_arm below. */
    int rx_size = setup_queue(&g_rxq, (uint16_t)VIRTIO_NET_RX_QUEUE);
    int tx_size = setup_queue(&g_txq, (uint16_t)VIRTIO_NET_TX_QUEUE);
    if (rx_size < 0 || tx_size < 0) {
        io_write8(g_dev.io_base + VIRTIO_PCI_DEVICE_STATUS,
                  (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    io_write8(g_dev.io_base + VIRTIO_PCI_DEVICE_STATUS, status);
    g_dev.ready = 1u;
    (void)printf("[virtio-net] vq ready rx=%d tx=%d rx_phys=0x%08X tx_phys=0x%08X\n", rx_size,
                 tx_size, (unsigned)(g_rxq.vq.region_phys & 0xFFFFFFFFu),
                 (unsigned)(g_txq.vq.region_phys & 0xFFFFFFFFu));

    /* Post the receive buffers now that the device is live (RX buffers may be
     * added after DRIVER_OK). */
    int rx_bufs = rx_arm();
    if (rx_bufs < 0) {
        (void)printf("[virtio-net] rx arm failed\n");
        return -1;
    }
    (void)printf("[virtio-net] rx armed bufs=%d rx_pool=0x%08X\n", rx_bufs,
                 (unsigned)(g_rx_pool_phys & 0xFFFFFFFFu));

    if (tx_arm() != 0) {
        (void)printf("[virtio-net] tx arm failed\n");
        return -1;
    }
    (void)printf("[virtio-net] tx armed bufs=%u tx_pool=0x%08X\n",
                 (unsigned)VIRTIO_NET_TX_BUF_COUNT, (unsigned)(g_tx_pool_phys & 0xFFFFFFFFu));
    return 0;
}

static void send_error(int32_t dest, int32_t request_id, int32_t code) {
    (void)wasmos_ipc_send(dest, g_endpoint, NETDRV_IPC_ERROR, request_id, code, 0, 0, 0);
}

static void handle_link_get(int32_t source, int32_t request_id, int32_t buffer_id) {
    int32_t link_up;
    if (!g_dev.present || !g_dev.ready) {
        send_error(source, request_id, WASMOS_ERR_NET_NOT_READY);
        return;
    }
    if (wasmos_sys_buffer_write(buffer_id, g_dev.mac, 6, 0) != 0) {
        send_error(source, request_id, WASMOS_ERR_NET_IO_ERROR);
        return;
    }
    link_up = ((g_dev.status_word & VIRTIO_NET_S_LINK_UP) != 0u) ? 1 : 0;
    g_link_sub_endpoint = source;
    (void)wasmos_ipc_send(source, g_endpoint, NETDRV_IPC_RESP, request_id, link_up,
                          (int32_t)g_dev.status_word, (int32_t)VIRTIO_NET_MTU_BASELINE, 0);
}

static void net_publish_link_change(void) {
    uint16_t status_word;
    if (!g_dev.present || !g_dev.ready || (g_dev.driver_features & VIRTIO_NET_F_STATUS) == 0u) {
        return;
    }
    status_word = read_status_word();
    if (status_word == g_dev.status_word) {
        return;
    }
    g_dev.status_word = status_word;
    if (g_link_sub_endpoint >= 0) {
        (void)wasmos_ipc_send(g_link_sub_endpoint, g_endpoint, NETDRV_IPC_LINK_NOTIFY, 0,
                              (status_word & VIRTIO_NET_S_LINK_UP) != 0u ? 1 : 0,
                              (int32_t)status_word, (int32_t)VIRTIO_NET_MTU_BASELINE, 0);
    }
}

static void handle_stats_get(int32_t source, int32_t request_id, int32_t buffer_id) {
    if (!g_dev.present || !g_dev.ready) {
        send_error(source, request_id, WASMOS_ERR_NET_NOT_READY);
        return;
    }
    if (wasmos_sys_buffer_write(buffer_id, &g_stats, (int32_t)sizeof(g_stats), 0) != 0) {
        send_error(source, request_id, WASMOS_ERR_NET_IO_ERROR);
        return;
    }
    (void)wasmos_ipc_send(source, g_endpoint, NETDRV_IPC_RESP, request_id, WASMOS_ERR_NONE, 0, 0,
                          0);
}

/* NETDRV_IPC_RX_POLL: register the caller as the RX_FRAME_NOTIFY subscriber and
 * deliver the next queued frame (if any) into its borrowed buffer. Replies RESP
 * with arg0 = frame length (0 = queue empty) and arg1 = frames still queued. */
static void handle_rx_poll(int32_t source, int32_t request_id, int32_t buffer_id) {
    if (!g_dev.present || !g_dev.ready) {
        send_error(source, request_id, WASMOS_ERR_NET_NOT_READY);
        return;
    }
    net_rx_sub_t* sub = rx_sub_for(source); /* subscribe on first poll */
    (void)net_drain_rx();                   /* pull path: also collect anything in the vring */

    uint8_t frame[VIRTIO_NET_MAX_FRAME];
    uint16_t len = rx_queue_pop(sub, frame, sizeof frame);
    if (len > 0u) {
        if (wasmos_sys_buffer_write(buffer_id, frame, (int32_t)len, 0) != 0) {
            send_error(source, request_id, WASMOS_ERR_NET_IO_ERROR);
            return;
        }
    }
    (void)wasmos_ipc_send(source, g_endpoint, NETDRV_IPC_RESP, request_id, (int32_t)len,
                          (int32_t)(sub ? sub->count : 0u), 0, 0);
}

/* NETDRV_IPC_TX_FRAME: transmit the frame in the caller's borrowed buffer.
 * arg0 carries the frame length. Replies RESP(WASMOS_ERR_NONE) once queued. */
static void handle_tx_frame(int32_t source, int32_t request_id, int32_t frame_len,
                            int32_t buffer_id) {
    if (!g_dev.present || !g_dev.ready) {
        send_error(source, request_id, WASMOS_ERR_NET_NOT_READY);
        return;
    }
    int rc = tx_send(buffer_id, frame_len);
    if (rc != WASMOS_ERR_NONE) {
        send_error(source, request_id, rc);
        return;
    }
    (void)wasmos_ipc_send(source, g_endpoint, NETDRV_IPC_RESP, request_id, WASMOS_ERR_NONE, 0, 0,
                          0);
}

WASMOS_WASM_EXPORT int32_t initialize(void) {
    /* proc.endpoint comes from the spawn-info contract, not an entry arg. */
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
    if (proc_endpoint < 0) {
        return WASMOS_ERR_DRIVER_NO_PROC_ENDPOINT;
    }

    g_endpoint = wasmos_ipc_create_endpoint();
    if (g_endpoint < 0) {
        return WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    }

    g_dev.present = 0u;
    g_dev.ready = 0u;
    g_dev.status_word = 0u;
    if (probe_virtio_net_from_startup_args() != 0) {
        (void)printf("[virtio-net] no device identity in startup args\n");
        return WASMOS_ERR_DRIVER_NO_DEVICE_IDENTITY;
    }
    /* pci-bus owns config space and is the only party that can program this
     * device's MSI-X table. Its absence is not fatal — initialize_device falls back to
     * the shared INTx line. Looked up before device init because enabling MSI-X
     * changes the register layout the rest of init uses. */
    g_pci_endpoint = wasmos_sys_svc_lookup_retry(proc_endpoint, g_endpoint, "pci", 1, 1024);
    if (g_pci_endpoint < 0) {
        (void)printf("[virtio-net] pci service unavailable; msi-x disabled\n");
    }
    if (initialize_device() != 0) {
        (void)printf("[virtio-net] init failed io=0x%04X dev=0x%04X\n", (unsigned)g_dev.io_base,
                     (unsigned)g_dev.device_id);
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    (void)printf("[virtio-net] probe ok bus=%u slot=%u dev=0x%04X irq=%u\n", (unsigned)g_dev.bus,
                 (unsigned)g_dev.slot, (unsigned)g_dev.device_id, (unsigned)g_dev.irq);
    (void)printf("[virtio-net] mac %02X:%02X:%02X:%02X:%02X:%02X io=0x%04X\n",
                 (unsigned)g_dev.mac[0], (unsigned)g_dev.mac[1], (unsigned)g_dev.mac[2],
                 (unsigned)g_dev.mac[3], (unsigned)g_dev.mac[4], (unsigned)g_dev.mac[5],
                 (unsigned)g_dev.io_base);
    (void)printf("[virtio-net] features dev=0x%08X drv=0x%08X\n", (unsigned)g_dev.device_features,
                 (unsigned)g_dev.driver_features);
    (void)printf("[virtio-net] driver ok link=%s mtu=%u\n",
                 ((g_dev.status_word & VIRTIO_NET_S_LINK_UP) != 0u) ? "up" : "down",
                 (unsigned)VIRTIO_NET_MTU_BASELINE);

    /* With MSI-X the vectors were bound during device init and the device's
     * INTx is disabled, so routing the shared line would only subscribe this
     * driver to other devices' interrupts. Route it only on the fallback path. */
    if (!g_dev.msix_enabled) {
        if (wasmos_irq_route_ipc((int32_t)g_dev.irq, g_endpoint) == 0) {
            (void)printf("[virtio-net] irq routed line=%u\n", (unsigned)g_dev.irq);
        } else {
            (void)printf("[virtio-net] irq route failed line=%u\n", (unsigned)g_dev.irq);
        }
    }
    net_probe_send();

    /* Finish all local setup before publishing the endpoint. Clients may send
     * LINK_GET as soon as registration completes, so no startup operation may
     * run between publication and the handler loop that drains it. */
    if (wasmos_svc_register_class(proc_endpoint, g_endpoint, "virtio.net", "net.ifc", 0u, 1) < 0) {
        (void)printf("[virtio-net] register failed\n");
        return WASMOS_ERR_DRIVER_REGISTER;
    }
    wasmos_sys_notify_ready(proc_endpoint, g_endpoint);

    /* Wait strategy follows the interrupt style. Under MSI-X the vector
     * re-delivers per notification, so the loop is a plain blocking wait. Under
     * INTx it cannot be: QEMU's legacy path re-fires reliably only for the first
     * assertion (see net_handle_irq), so continuous RX falls back to draining
     * the ring on a bounded timeout — a timer, not a spin. */
    int32_t sel = wasmos_ipc_select_create();
    if (sel < 0 || wasmos_ipc_select_add(sel, g_endpoint) != 0) {
        (void)printf("[virtio-net] select setup failed\n");
        return WASMOS_ERR_DRIVER_SELECT_SETUP;
    }

    for (;;) {
        wasmos_ipc_message_t msg;
        /* Handler loop: drain and dispatch EVERY pending message each iteration,
         * unconditionally. wasmos_ipc_select_wait_timeout only reports
         * edge-signalled readiness (a single latched ready_ep), so a request
         * that arrived without an edge — or while RX was being serviced — must be
         * picked up here rather than stranded until some future signal. Draining
         * only on the "ready" branch deadlocks: a client blocked on a LINK_GET/TX
         * reply never gets it, because its request sits undrained on the timeout
         * branch. */
        while (wasmos_ipc_drain(g_endpoint) > 0) {
            wasmos_ipc_message_read_last(&msg);
            /* Hardware IRQ arrives as IPC_IRQ_EVENT_TYPE with source=NONE (< 0),
             * so handle it before the source check that guards request traffic. */
            if (msg.type == IPC_IRQ_EVENT_TYPE) {
                net_handle_irq();
                continue;
            }
            if (msg.type == WASMOS_IPC_MSI_EVENT_TYPE) {
                net_handle_msi(msg.arg0);
                continue;
            }
            if (msg.source < 0) {
                continue;
            }
            /* FIXME(owner-push): net protocol must carry the client buffer_id/grant;
             * threading msg.arg0 (msg.arg1 for TX, whose arg0 is frame_len) as a
             * placeholder buffer_id until the wire protocol is defined. */
            if (msg.type == NETDRV_IPC_LINK_GET) {
                handle_link_get(msg.source, msg.request_id, msg.arg0);
            } else if (msg.type == NETDRV_IPC_STATS_GET) {
                handle_stats_get(msg.source, msg.request_id, msg.arg0);
            } else if (msg.type == NETDRV_IPC_RX_POLL) {
                handle_rx_poll(msg.source, msg.request_id, msg.arg0);
            } else if (msg.type == NETDRV_IPC_TX_FRAME) {
                handle_tx_frame(msg.source, msg.request_id, msg.arg0, msg.arg1);
            } else {
                send_error(msg.source, msg.request_id, WASMOS_ERR_NET_INVALID);
            }
        }
        net_publish_link_change();
        if (g_dev.msix_enabled) {
            /* The vector fires for every completion, so block outright. */
            (void)wasmos_ipc_select_wait(sel);
        } else {
            /* Idle: poll the RX ring + reap TX (INTx re-delivery workaround),
             * then block until the next message or the poll deadline. */
            net_service_rx();
            if (!wasmos_sys_wait_parked(
                    wasmos_ipc_select_wait_timeout(sel, NET_RX_POLL_INTERVAL_MS))) {
                /* A failed wait returns immediately, so this poll loop stops
                 * being a poll. Yield explicitly rather than tightening into a
                 * hot loop -- and do NOT exit: losing the interface outright is
                 * worse than polling badly.
                 * FIXME: with a dead select set there is nothing left to park
                 * on, so this is still a yield loop. The real fix is not to
                 * lose the set; nothing currently reports that it happened. */
                (void)wasmos_sched_yield();
            }
        }
    }
    return 0;
}
