/* virtio_rng.c - Minimal VirtIO entropy (hardware RNG) WASM driver.
 * virtio-rng is the simplest virtio device: a single device-writable virtqueue
 * (the requestq). The driver posts a buffer, kicks the doorbell, and the device
 * fills it with random bytes and returns it on the used ring — no headers, no
 * device-config negotiation. Exposes the backend-neutral "hrng" class service
 * (HRNG_IPC_GET_BYTES_REQ); see wasmos_driver_abi.h.
 *
 * The device is driven over the virtio 1.x PCI transport (virtio_modern.h): its
 * registers are device memory in BAR 4, granted as region 0 by the manifest's
 * [[regions]] entry, and located through the device's own capability list. */
#include <stdint.h>
#include <stdio.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos/virtio_modern.h"
#include "wasmos/vring.h"
#include "wasmos_driver_abi.h"

/* 0x1AF4 is the Red Hat / virtio vendor id; both device ids below identify an
 * entropy source and are accepted interchangeably (a transitional device also
 * offers the modern interface this driver uses). */
#define VIRTIO_PCI_VENDOR_ID 0x1AF4u
#define VIRTIO_RNG_DEV_LEGACY 0x1005u /* transitional entropy device */
#define VIRTIO_RNG_DEV_MODERN 0x1044u /* 0x1040 + virtio device type 4 */

/* The BAR holding the virtio structures, and the region the manifest grants it
 * as. QEMU places every modern structure in BAR 4; a device that does not is
 * refused by virtio_modern_probe rather than driven through a window that was
 * never granted. */
#define VIRTIO_RNG_BAR 4u
#define VIRTIO_RNG_REGION 0

/* One source, one entry: the request queue's completion. */
#define VIRTIO_RNG_MSIX_ENTRY_QUEUE 0u

/* virtio-rng defines exactly one queue, index 0, the requestq;
 * VIRTIO_RNG_MAX_QUEUE caps how large a queue this driver will accept from the
 * device, since the descriptor bookkeeping is statically sized. */
#define VIRTIO_RNG_REQUEST_QUEUE 0u
#define VIRTIO_RNG_MAX_QUEUE 256u /* max supported queue size */

/* One page of DMA-visible scratch the device fills with entropy per request. */
#define RNG_POOL_SIZE 4096u
/* Completion is interrupt-driven: the device raises its INTx line (or its MSI-X
 * vector) when it has used a buffer, and the driver blocks on the routed event.
 * The interval and try count below are only a safety net for a lost or
 * unroutable interrupt, not the mechanism — acking a device interrupt on a timer
 * leaves the shared line asserted between ticks, which livelocks a single-CPU
 * guest. */
#define RNG_IRQ_WAIT_MS 50
#define RNG_IRQ_MAX_WAITS 20 /* ~1s ceiling before WASMOS_ERR_HRNG_TIMEOUT */

typedef struct {
    uint8_t present;
    uint8_t ready;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t irq;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t device_features;
    uint8_t msix_enabled; /* MSI-X bound: the device no longer drives INTx */
    uint32_t msix_vector; /* kernel vector behind entry 0 */
    virtio_modern_t vm;   /* the transport: where the device's structures are */
    uint32_t notify;      /* the request queue's doorbell offset in region 0 */
} virtio_rng_device_t;

static int32_t g_endpoint = -1;
static int32_t g_select = -1;
/* The IRQ event has its own endpoint and select set so the completion wait can
 * drain it without consuming (and discarding) pending HRNG service requests. */
static int32_t g_irq_endpoint = -1;
static int32_t g_irq_select = -1;
static uint8_t g_irq_routed;
static int32_t g_pci_endpoint = -1; /* pci-bus: owns config space, programs MSI-X */
/* On the INTx path the function may still be quiesced from a previous driver's
 * end -- bus mastering off, INTx disabled -- until pci-bus answers
 * PCI_IPC_INTX_ENABLE_REQ. The request goes out from g_endpoint before the
 * service registers, and its answer is consumed by the request loop. A fill is
 * refused with WASMOS_ERR_HRNG_NOT_READY until then. A request that cannot be
 * sent, or that pci-bus refuses, ends the driver: the function cannot be armed,
 * and the supervisor's restart is the retry. */
#define RNG_INTX_ENABLE_REQ_ID 0x7B20
static uint8_t g_function_armed = 1u;
static virtio_rng_device_t g_dev;

static vring_t g_rq; /* the single request virtqueue (device -> driver) */
static uint8_t* g_pool;
static uint64_t g_pool_phys;

static int is_virtio_rng_device(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id != VIRTIO_PCI_VENDOR_ID) {
        return 0;
    }
    return device_id == VIRTIO_RNG_DEV_LEGACY || device_id == VIRTIO_RNG_DEV_MODERN;
}

static int parse_hex_n(const char* s, uint32_t digits, uint32_t* out) {
    uint32_t value = 0;
    if (!s || !out || digits == 0u) {
        return -1;
    }
    for (uint32_t i = 0; i < digits; ++i) {
        char ch = s[i];
        uint32_t nibble;
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

/* A PCI-rule spawn names the matched function and its IRQ line in the startup
 * args; the driver is spawned for exactly that device and has no way to look for
 * another (it may read only its own function's config space). */
static int probe_virtio_rng_from_startup_args(void) {
    char args[128];
    const char* pci;
    const char* vendor;
    const char* device;
    const char* irq;
    uint32_t bus = 0, slot = 0, function = 0;
    uint32_t vendor_id = 0, device_id = 0, irq_line = 0;

    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return -1;
    }
    pci = find_token_value(args, "pci=");
    vendor = find_token_value(args, "vendor=");
    device = find_token_value(args, "device=");
    irq = find_token_value(args, "irq=");
    if (!pci || !vendor || !device || !irq) {
        return -1;
    }
    if (parse_hex_n(pci, 2u, &bus) != 0 || pci[2] != ':' || parse_hex_n(pci + 3, 2u, &slot) != 0 ||
        pci[5] != '.' || parse_hex_n(pci + 6, 2u, &function) != 0 ||
        parse_hex_n(vendor, 4u, &vendor_id) != 0 || parse_hex_n(device, 4u, &device_id) != 0 ||
        parse_hex_n(irq, 2u, &irq_line) != 0) {
        return -1;
    }
    if (!is_virtio_rng_device((uint16_t)vendor_id, (uint16_t)device_id)) {
        return -1;
    }
    g_dev.present = 1u;
    g_dev.bus = (uint8_t)bus;
    g_dev.slot = (uint8_t)slot;
    g_dev.function = (uint8_t)function;
    g_dev.irq = (uint8_t)irq_line;
    g_dev.vendor_id = (uint16_t)vendor_id;
    g_dev.device_id = (uint16_t)device_id;
    return 0;
}

/* vring doorbell: tell the device the request queue has new available buffers. */
static void virtio_rng_notify(void* user) {
    (void)user;
    virtio_modern_notify(&g_dev.vm, g_dev.notify, (uint16_t)VIRTIO_RNG_REQUEST_QUEUE);
}

/* Configure the request virtqueue over a pinned DMA region and hand it to the
 * device, bound to its MSI-X entry when MSI-X is live. Returns the queue size,
 * or -1 on failure. */
static int setup_queue(void) {
    uint16_t qsize = virtio_modern_queue_size(&g_dev.vm, (uint16_t)VIRTIO_RNG_REQUEST_QUEUE);
    if (qsize == 0u) {
        return -1;
    }
    if (qsize > VIRTIO_RNG_MAX_QUEUE) {
        qsize = VIRTIO_RNG_MAX_QUEUE; /* a modern device accepts a smaller size */
    }

    uint64_t ring_bytes = vring_size(qsize, VIRTIO_MODERN_VRING_ALIGN);
    int32_t pages = (int32_t)((ring_bytes + 0xFFFu) / 0x1000u);
    uint64_t ring_phys = 0;
    int32_t off = wasmos_region_alloc(pages, WASMOS_REGION_CACHE_WB, &ring_phys);
    if (off < 0) {
        return -1;
    }
    uint8_t* ring = ptr_cast(uint8_t, (uint32_t)off);
    if (vring_layout(
            &g_rq, ring, ring_phys, (uint64_t)pages * 0x1000u, qsize, VIRTIO_MODERN_VRING_ALIGN) !=
        0) {
        return -1;
    }
    vring_set_notify(&g_rq, virtio_rng_notify, 0);
    if (virtio_modern_queue_enable(&g_dev.vm,
                                   (uint16_t)VIRTIO_RNG_REQUEST_QUEUE,
                                   &g_rq,
                                   g_dev.msix_enabled ? (uint16_t)VIRTIO_RNG_MSIX_ENTRY_QUEUE
                                                      : (uint16_t)VIRTIO_MSI_NO_VECTOR,
                                   &g_dev.notify) != 0) {
        (void)printf("[virtio-rng] msix queue vector refused\n");
        return -1;
    }
    return (int)qsize;
}

/* Take one kernel MSI vector for the completion interrupt and have pci-bus
 * program it into the device's MSI-X table. See net_setup_msix in virtio_net.c
 * for why the work is split across driver, kernel and pci-bus.
 *
 * Moving this device off INTx matters beyond its own latency: under QEMU it
 * shares IRQ 11 with virtio-net, and a shared wire is what forced the whole
 * sharer/ack/deadline machinery in the kernel. A message-signalled vector is
 * nobody else's. Returns 0 when MSI-X is live.
 *
 * TODO: both pci-bus requests below are synchronous wasmos_ipc_call round
 * trips, which AGENTS.md no longer allows for new code; they predate that rule
 * and move to a future together with the driver's bring-up (docs/TASKS.md,
 * synchronous call removal). */
static int rng_setup_msix(void) {
    if (g_pci_endpoint < 0) {
        return -1;
    }
    uint32_t bdf =
        ((uint32_t)g_dev.bus << 8) | ((uint32_t)g_dev.slot << 3) | (uint32_t)g_dev.function;
    wasmos_ipc_message_t reply;

    if (wasmos_ipc_call(
            g_pci_endpoint, g_endpoint, PCI_IPC_MSI_QUERY, 1, (int32_t)bdf, 0, 0, 0, &reply) != 0 ||
        reply.type != PCI_IPC_RESP || reply.arg0 != WASMOS_PCI_MSI_KIND_MSIX || reply.arg1 < 1) {
        return -1;
    }

    wasmos_msi_desc_t desc;
    if (g_irq_endpoint < 0 || wasmos_msi_alloc(g_irq_endpoint, &desc) != 0) {
        return -1;
    }
    int32_t arg0 = (int32_t)((bdf << 8) | VIRTIO_RNG_MSIX_ENTRY_QUEUE);
    if (wasmos_ipc_call(g_pci_endpoint,
                        g_endpoint,
                        PCI_IPC_MSI_BIND,
                        2,
                        arg0,
                        (int32_t)desc.address_lo,
                        (int32_t)desc.address_hi,
                        (int32_t)desc.data,
                        &reply) != 0 ||
        reply.type != PCI_IPC_RESP) {
        (void)wasmos_msi_free((int32_t)desc.vector);
        return -1;
    }
    g_dev.msix_vector = desc.vector;
    g_dev.msix_enabled = 1u;
    return 0;
}

/* Brings the device up over the modern transport: locate its structures,
 * negotiate (virtio-rng has no feature of its own, so only VERSION_1), bind
 * MSI-X, configure the request queue and the entropy pool, then DRIVER_OK. */
static int initialize_device(void) {
    uint8_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
    uint64_t accepted = 0;
    uint32_t bdf =
        ((uint32_t)g_dev.bus << 8) | ((uint32_t)g_dev.slot << 3) | (uint32_t)g_dev.function;
    if (!g_dev.present) {
        return -1;
    }
    if (virtio_modern_probe(&g_dev.vm, bdf, VIRTIO_RNG_BAR, VIRTIO_RNG_REGION) != 0) {
        (void)printf("[virtio-rng] no virtio-modern structures in BAR %u\n",
                     (unsigned)VIRTIO_RNG_BAR);
        return -1;
    }
    if (virtio_modern_negotiate(&g_dev.vm, 0u, 0, &accepted) != 0) {
        (void)printf("[virtio-rng] feature negotiation failed\n");
        return -1;
    }
    g_dev.device_features = (uint32_t)(accepted >> 32);

    /* Before queue setup, which binds the queue to its MSI-X entry. */
    if (rng_setup_msix() == 0) {
        (void)printf("[virtio-rng] msix enabled vector=%u\n", (unsigned)g_dev.msix_vector);
    } else {
        (void)printf("[virtio-rng] msix unavailable; falling back to intx line=%u\n",
                     (unsigned)g_dev.irq);
    }

    int qsize = setup_queue();
    if (qsize < 0) {
        virtio_modern_set_status(&g_dev.vm, (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }

    /* One page of pinned DMA scratch that the device fills per request. */
    int32_t pages = (int32_t)((RNG_POOL_SIZE + 0xFFFu) / 0x1000u);
    uint64_t phys = 0;
    int32_t off = wasmos_region_alloc(pages, WASMOS_REGION_CACHE_WB, &phys);
    if (off < 0) {
        virtio_modern_set_status(&g_dev.vm, (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }
    g_pool = ptr_cast(uint8_t, (uint32_t)off);
    g_pool_phys = phys;

    virtio_modern_set_status(&g_dev.vm, (uint8_t)(status | VIRTIO_STATUS_DRIVER_OK));
    g_dev.ready = 1u;
    (void)printf("[virtio-rng] vq ready qsize=%d ring_phys=0x%08X pool_phys=0x%08X\n",
                 qsize,
                 (unsigned)(g_rq.region_phys & 0xFFFFFFFFu),
                 (unsigned)(g_pool_phys & 0xFFFFFFFFu));
    return 0;
}

/* Service the device's interrupt: reading the virtio ISR de-asserts the
 * level-triggered INTx line, then the line is unmasked via irq_ack.  Both halves
 * matter — the kernel keeps a dispatched line masked until every sharer acks, and
 * the device keeps asserting until its ISR is read.  This device shares IRQ 11
 * with virtio-net under QEMU, so an unserviced assertion here re-fires for both
 * drivers on every unmask. */
static void rng_service_irq(void) {
    if (!g_irq_routed && !g_dev.msix_enabled) {
        return;
    }
    /* Drain the interrupt endpoint: several events may have been queued. */
    while (wasmos_ipc_drain(g_irq_endpoint) > 0) {
        /* The payload names only the source; the work is the same. */
    }
    if (g_dev.msix_enabled) {
        /* Nothing to de-assert and nothing to ack: the vector is edge-triggered
         * and exclusive to this device, so none of the shared-line ceremony
         * applies. */
        return;
    }
    /* Reading ISR is itself the ack; the value is not wanted. */
    (void)virtio_modern_isr(&g_dev.vm);
    (void)wasmos_irq_ack((int32_t)g_dev.irq);
}

/* Fill up to `want` bytes of entropy into the DMA pool and return the count the
 * device produced, or -1 on ring/timeout failure. Posts the pool device-writable,
 * kicks, then checks the used ring between blocking waits on the completion
 * interrupt. */
static int rng_fill(uint32_t want) {
    if (want > RNG_POOL_SIZE) {
        want = RNG_POOL_SIZE;
    }
    int32_t d = vring_alloc_desc(&g_rq, g_pool_phys, want, VRING_DESC_F_WRITE);
    if (d < 0) {
        return -1;
    }
    vring_publish(&g_rq, (uint16_t)d);
    vring_kick(&g_rq);

    uint32_t used_len = 0;
    for (int waits = 0; waits <= RNG_IRQ_MAX_WAITS; ++waits) {
        int32_t id = vring_get_used(&g_rq, &used_len);
        if (id >= 0) {
            vring_free_desc(&g_rq, (uint16_t)id);
            if (used_len > want) {
                used_len = want;
            }
            return (int)used_len;
        }
        if (waits == RNG_IRQ_MAX_WAITS) {
            break;
        }
        /* Block until the device's completion interrupt arrives (or the
         * safety-net interval elapses).  Only IRQ events land on this endpoint,
         * so draining it cannot swallow an HRNG request.  A wait that FAILS
         * returns immediately, so looping on it would spin rather than block;
         * give up on the completion and let the caller's timeout path run. */
        if (g_irq_routed || g_dev.msix_enabled) {
            if (!wasmos_sys_wait_parked(
                    wasmos_ipc_select_wait_timeout(g_irq_select, RNG_IRQ_WAIT_MS))) {
                break;
            }
            rng_service_irq();
        } else if (!wasmos_sys_wait_parked(
                       wasmos_ipc_select_wait_timeout(g_select, RNG_IRQ_WAIT_MS))) {
            break;
        }
    }
    vring_free_desc(&g_rq, (uint16_t)d);
    /* Time out only after servicing the line: leaving it asserted here turns a
     * lost completion into an unbounded interrupt storm. */
    rng_service_irq();
    return -1;
}

static void send_error(int32_t dest, int32_t request_id, int32_t code) {
    (void)wasmos_ipc_send(dest, g_endpoint, HRNG_IPC_ERROR, request_id, code, 0, 0, 0);
}

/* HRNG_IPC_GET_BYTES_REQ: fill up to arg1 bytes of entropy into the client's
 * borrowed buffer (arg0). Replies HRNG_IPC_RESP with arg0 = bytes written. */
static void handle_get_bytes(int32_t source, int32_t request_id, int32_t buffer_id,
                             int32_t req_len) {
    if (!g_dev.present || !g_dev.ready || !g_function_armed) {
        send_error(source, request_id, WASMOS_ERR_HRNG_NOT_READY);
        return;
    }
    if (req_len <= 0) {
        send_error(source, request_id, WASMOS_ERR_HRNG_INVALID);
        return;
    }
    int n = rng_fill((uint32_t)req_len);
    if (n < 0) {
        send_error(source, request_id, WASMOS_ERR_HRNG_TIMEOUT);
        return;
    }
    if (n > 0 && wasmos_sys_buffer_write(buffer_id, g_pool, n, 0) != 0) {
        send_error(source, request_id, WASMOS_ERR_HRNG_IO_ERROR);
        return;
    }
    (void)wasmos_ipc_send(source, g_endpoint, HRNG_IPC_RESP, request_id, n, 0, 0, 0);
}

/* Ask pci-bus to arm the function for INTx (PCI_IPC_INTX_ENABLE_REQ), without
 * waiting. The kernel quiesced the function when a previous driver of it ended,
 * and only an MSI bind or this request undoes that. initialize_device reset the
 * device first, so it names none of the previous driver's frames once it can
 * master again. Returns 0, or -1 when the request could not be sent. Without a
 * pci service nothing can be asked, and the function keeps the firmware's
 * state. */
static int rng_request_intx_enable(void) {
    uint32_t bdf =
        ((uint32_t)g_dev.bus << 8) | ((uint32_t)g_dev.slot << 3) | (uint32_t)g_dev.function;
    if (g_pci_endpoint < 0) {
        (void)printf("[virtio-rng] no pci service; intx left as the firmware set it\n");
        return 0;
    }
    if (wasmos_ipc_send(g_pci_endpoint,
                        g_endpoint,
                        PCI_IPC_INTX_ENABLE_REQ,
                        RNG_INTX_ENABLE_REQ_ID,
                        (int32_t)bdf,
                        0,
                        0,
                        0) != 0) {
        (void)printf("[virtio-rng] intx enable request could not be sent\n");
        return -1;
    }
    g_function_armed = 0u;
    return 0;
}

/* Consume pci-bus's answer to PCI_IPC_INTX_ENABLE_REQ. Returns 0 when `msg` is
 * not it, 1 when the function is armed, and -1 when pci-bus refused, after
 * resetting the device so it holds none of this driver's frames when the
 * driver ends. */
static int rng_intx_enable_event(const wasmos_ipc_message_t* msg) {
    if ((msg->type != PCI_IPC_RESP && msg->type != PCI_IPC_ERROR) ||
        msg->request_id != RNG_INTX_ENABLE_REQ_ID || msg->source != g_pci_endpoint) {
        return 0;
    }
    if (msg->type != PCI_IPC_RESP) {
        (void)printf("[virtio-rng] pci-bus refused intx enable status=%d; ending\n",
                     (int)msg->arg1);
        (void)virtio_modern_reset(&g_dev.vm);
        return -1;
    }
    (void)printf("[virtio-rng] function armed for intx\n");
    g_function_armed = 1u;
    return 1;
}

/* Driver entry point: bring up the virtio-rng device, register under the "hrng"
 * class, and serve HRNG_IPC_GET_BYTES_REQ forever.
 *
 * All four parameters are ignored; proc_endpoint is overwritten from the
 * spawn-info contract, because the entry arguments are passed as zero.
 *
 * On success this does not return. The service deliberately keeps running even
 * when its wait primitive fails -- an entropy provider that exits leaves every
 * client blocked, so it degrades to yielding instead of quitting. A bring-up
 * failure returns a packed WASMOS_ERR_DRIVER_* code. */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
    if (proc_endpoint < 0) {
        return WASMOS_ERR_DRIVER_NO_PROC_ENDPOINT;
    }

    g_endpoint = wasmos_ipc_create_endpoint();
    if (g_endpoint < 0) {
        return WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    }
    g_select = wasmos_ipc_select_create();
    if (g_select < 0 || wasmos_ipc_select_add(g_select, g_endpoint) != 0) {
        (void)printf("[virtio-rng] select setup failed\n");
        return WASMOS_ERR_DRIVER_SELECT_SETUP;
    }

    /* Interrupts get their own endpoint, whether they arrive as a routed INTx
     * line or as an MSI vector: the completion wait drains it, and draining the
     * service endpoint instead would discard pending HRNG requests. It must
     * exist before device init, because that is where the MSI vector is bound. */
    g_irq_endpoint = wasmos_ipc_create_endpoint();
    g_irq_select = (g_irq_endpoint >= 0) ? wasmos_ipc_select_create() : -1;
    if (g_irq_endpoint < 0 || g_irq_select < 0 ||
        wasmos_ipc_select_add(g_irq_select, g_irq_endpoint) != 0 ||
        wasmos_ipc_select_add(g_select, g_irq_endpoint) != 0) {
        (void)printf("[virtio-rng] irq endpoint setup failed; using timed waits\n");
        g_irq_endpoint = -1;
        g_irq_select = -1;
    }

    /* pci-bus owns config space and is the only party that can program this
     * device's MSI-X table; its absence just means the INTx fallback below. */
    g_pci_endpoint = wasmos_sys_svc_lookup_retry(proc_endpoint, g_endpoint, "pci", 1, 1024);
    if (g_pci_endpoint < 0) {
        (void)printf("[virtio-rng] pci service unavailable; msi-x disabled\n");
    }

    g_dev.present = 0u;
    g_dev.ready = 0u;
    if (probe_virtio_rng_from_startup_args() == 0) {
        if (initialize_device() != 0) {
            (void)printf("[virtio-rng] init failed dev=0x%04X\n", (unsigned)g_dev.device_id);
        } else {
            (void)printf("[virtio-rng] probe ok bus=%u slot=%u dev=0x%04X modern\n",
                         (unsigned)g_dev.bus,
                         (unsigned)g_dev.slot,
                         (unsigned)g_dev.device_id);
        }
    } else {
        (void)printf("[virtio-rng] no device found\n");
    }

    /* INTx fallback only. With MSI-X bound the device's INTx is disabled, so
     * routing the shared line would just subscribe this driver to other devices'
     * interrupts, which is exactly what MSI-X exists to avoid.
     * Without it, routing is what keeps the line from re-firing forever: the
     * device asserts on every completed fill and nothing else clears it.
     * Failure is not fatal: the fill falls back to the timed safety net. */
    if (!g_dev.msix_enabled && g_dev.ready && rng_request_intx_enable() != 0) {
        (void)virtio_modern_reset(&g_dev.vm);
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    if (!g_dev.msix_enabled && g_dev.ready && g_dev.irq < 16u && g_irq_endpoint >= 0) {
        if (wasmos_irq_route_ipc((int32_t)g_dev.irq, g_irq_endpoint) == 0) {
            g_irq_routed = 1;
            (void)printf("[virtio-rng] irq routed line=%u\n", (unsigned)g_dev.irq);
        } else {
            (void)printf("[virtio-rng] irq route failed line=%u; using timed waits\n",
                         (unsigned)g_dev.irq);
        }
    }

    /* Register under the generic "hrng" class so class-based lookup finds any
     * entropy provider uniformly; the concrete service name is "virtio-rng". */
    if (wasmos_svc_register_class(proc_endpoint, g_endpoint, "virtio-rng", "hrng", 0, 1) < 0) {
        (void)printf("[virtio-rng] register failed\n");
        return WASMOS_ERR_DRIVER_REGISTER;
    }
    wasmos_sys_notify_ready(proc_endpoint, g_endpoint);

    for (;;) {
        /* Ack the line whenever an event has arrived, not only while a fill is
         * waiting.  IRQ 11 is shared with virtio-net and the kernel keeps a
         * dispatched line masked until EVERY sharer acks, so deferring this ack
         * stalls the other driver's interrupts as well. */
        rng_service_irq();
        while (wasmos_ipc_drain(g_endpoint) > 0) {
            wasmos_ipc_message_t msg;
            wasmos_ipc_message_read_last(&msg);
            if (msg.source < 0) {
                continue; /* stray notification / IRQ echo */
            }
            int armed = rng_intx_enable_event(&msg);
            if (armed < 0) {
                return WASMOS_ERR_DRIVER_DEVICE_INIT;
            }
            if (armed > 0) {
                continue;
            }
            if (msg.type == HRNG_IPC_GET_BYTES_REQ) {
                handle_get_bytes(msg.source, msg.request_id, msg.arg0, msg.arg1);
            } else {
                send_error(msg.source, msg.request_id, WASMOS_ERR_HRNG_INVALID);
            }
        }
        if (!wasmos_sys_wait_parked(wasmos_ipc_select_wait_timeout(g_select, 1000))) {
            /* A failed wait returns immediately, so this loop stops blocking.
             * Yield explicitly rather than tightening into a hot loop -- and do
             * NOT exit: an entropy service that quits is worse than one that
             * polls.
             * FIXME: with a dead select set there is nothing left to park on.
             * The real fix is not to lose the set. */
            (void)wasmos_sched_yield();
        }
    }
    return 0;
}
