/* virtio_rng.c - Minimal VirtIO entropy (hardware RNG) WASM driver.
 * virtio-rng is the simplest virtio device: a single device-writable virtqueue
 * (the requestq). The driver posts a buffer, kicks the doorbell, and the device
 * fills it with random bytes and returns it on the used ring — no headers, no
 * device-config negotiation. Exposes the backend-neutral "hrng" class service
 * (HRNG_IPC_GET_BYTES_REQ); see wasmos_driver_abi.h. */
#include <stdint.h>
#include <stdio.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos/vring.h"
#include "wasmos_driver_abi.h"

#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

#define VIRTIO_PCI_VENDOR_ID 0x1AF4u
#define VIRTIO_RNG_DEV_LEGACY 0x1005u        /* transitional/legacy entropy device */
#define VIRTIO_RNG_DEV_MODERN 0x1044u        /* 0x1040 + virtio device type 4 */

/* Legacy virtqueue registers (no MSI-X: mirrors virtio_net.c). */
#define VIRTIO_PCI_DEVICE_FEATURES 0x00u
#define VIRTIO_PCI_DRIVER_FEATURES 0x04u
#define VIRTIO_PCI_QUEUE_PFN 0x08u    /* u32: ring PFN (phys >> 12) */
#define VIRTIO_PCI_QUEUE_SIZE 0x0Cu   /* u16: selected queue size (0 = absent) */
#define VIRTIO_PCI_QUEUE_SELECT 0x0Eu /* u16: select the queue to configure */
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10u /* u16: doorbell — write the queue index */
#define VIRTIO_PCI_DEVICE_STATUS 0x12u
#define VIRTIO_PCI_ISR_STATUS 0x13u

#define VIRTIO_PCI_VRING_ALIGN 4096u
#define VIRTIO_RNG_REQUEST_QUEUE 0u
#define VIRTIO_RNG_MAX_QUEUE 256u /* max supported queue size */

#define VIRTIO_STATUS_ACK 1u
#define VIRTIO_STATUS_DRIVER 2u
#define VIRTIO_STATUS_DRIVER_OK 4u
#define VIRTIO_STATUS_FAILED 128u

/* One page of DMA-visible scratch the device fills with entropy per request. */
#define RNG_POOL_SIZE 4096u
/* Bounded wait for the device to complete a fill (it is effectively immediate on
 * QEMU); we poll the used ring, sleeping between checks rather than busy-spin. */
#define RNG_POLL_INTERVAL_MS 2
#define RNG_POLL_MAX_TRIES 500 /* ~1s ceiling before HRNG_STATUS_TIMEOUT */

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
} virtio_rng_device_t;

static int32_t g_endpoint = -1;
static int32_t g_select = -1;
static virtio_rng_device_t g_dev;

static vring_t g_rq; /* the single request virtqueue (device -> driver) */
static uint8_t* g_pool;
static uint64_t g_pool_phys;

static uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t reg) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                       ((uint32_t)function << 8) | ((uint32_t)reg & 0xFCu);
    (void)wasmos_io_out32(PCI_CFG_ADDR_PORT, (int32_t)address);
    return (uint32_t)wasmos_io_in32(PCI_CFG_DATA_PORT);
}

static uint16_t io_read16(uint16_t port) {
    return (uint16_t)((uint32_t)wasmos_io_in16((int32_t)port) & 0xFFFFu);
}

static uint32_t io_read32(uint16_t port) {
    return (uint32_t)wasmos_io_in32((int32_t)port);
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

static int is_virtio_rng_device(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id != VIRTIO_PCI_VENDOR_ID) {
        return 0;
    }
    return device_id == VIRTIO_RNG_DEV_LEGACY || device_id == VIRTIO_RNG_DEV_MODERN;
}

static int probe_virtio_rng(void) {
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t function = 0; function < 8; ++function) {
                uint32_t id = pci_config_read32((uint8_t)bus, slot, function, 0x00);
                uint16_t vendor_id = (uint16_t)(id & 0xFFFFu);
                uint16_t device_id = (uint16_t)((id >> 16) & 0xFFFFu);
                uint32_t bar0;
                if (vendor_id == 0xFFFFu) {
                    if (function == 0u) {
                        break;
                    }
                    continue;
                }
                if (!is_virtio_rng_device(vendor_id, device_id)) {
                    continue;
                }
                bar0 = pci_config_read32((uint8_t)bus, slot, function, 0x10);
                if ((bar0 & 0x1u) == 0u) {
                    continue; /* not an I/O BAR */
                }
                g_dev.present = 1u;
                g_dev.bus = (uint8_t)bus;
                g_dev.slot = slot;
                g_dev.function = function;
                g_dev.io_base = (uint16_t)(bar0 & 0xFFFCu);
                g_dev.irq = (uint8_t)(pci_config_read32((uint8_t)bus, slot, function, 0x3C) & 0xFFu);
                g_dev.vendor_id = vendor_id;
                g_dev.device_id = device_id;
                return 0;
            }
        }
    }
    return -1;
}

/* vring doorbell: tell the device the request queue has new available buffers. */
static void virtio_rng_notify(void* user) {
    (void)user;
    io_write16(g_dev.io_base + VIRTIO_PCI_QUEUE_NOTIFY, (uint16_t)VIRTIO_RNG_REQUEST_QUEUE);
}

/* Configure the request virtqueue over a pinned DMA region and program its PFN
 * into the device. Returns the queue size, or -1 on failure. Mirrors
 * virtio_net.c::setup_queue. */
static int setup_queue(void) {
    io_write16(g_dev.io_base + VIRTIO_PCI_QUEUE_SELECT, (uint16_t)VIRTIO_RNG_REQUEST_QUEUE);
    uint16_t qsize = io_read16(g_dev.io_base + VIRTIO_PCI_QUEUE_SIZE);
    if (qsize == 0u || qsize > VIRTIO_RNG_MAX_QUEUE) {
        return -1;
    }

    uint64_t ring_bytes = vring_size(qsize, VIRTIO_PCI_VRING_ALIGN);
    int32_t pages = (int32_t)((ring_bytes + 0xFFFu) / 0x1000u);
    uint64_t ring_phys = 0;
    int32_t off = wasmos_region_alloc(pages, WASMOS_REGION_CACHE_WB, &ring_phys);
    if (off < 0) {
        return -1;
    }
    uint8_t* ring = ptr_cast(uint8_t, (uint32_t)off);
    if (vring_layout(&g_rq, ring, ring_phys, (uint64_t)pages * 0x1000u, qsize,
                     VIRTIO_PCI_VRING_ALIGN) != 0) {
        return -1;
    }
    vring_set_notify(&g_rq, virtio_rng_notify, 0);
    io_write32(g_dev.io_base + VIRTIO_PCI_QUEUE_PFN, (uint32_t)(ring_phys >> 12));
    return (int)qsize;
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

    /* virtio-rng has no driver-relevant feature bits; accept none. */
    g_dev.device_features = io_read32(g_dev.io_base + VIRTIO_PCI_DEVICE_FEATURES);
    io_write32(g_dev.io_base + VIRTIO_PCI_DRIVER_FEATURES, 0u);

    int qsize = setup_queue();
    if (qsize < 0) {
        io_write8(g_dev.io_base + VIRTIO_PCI_DEVICE_STATUS, (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }

    /* One page of pinned DMA scratch that the device fills per request. */
    int32_t pages = (int32_t)((RNG_POOL_SIZE + 0xFFFu) / 0x1000u);
    uint64_t phys = 0;
    int32_t off = wasmos_region_alloc(pages, WASMOS_REGION_CACHE_WB, &phys);
    if (off < 0) {
        io_write8(g_dev.io_base + VIRTIO_PCI_DEVICE_STATUS, (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }
    g_pool = ptr_cast(uint8_t, (uint32_t)off);
    g_pool_phys = phys;

    status |= VIRTIO_STATUS_DRIVER_OK;
    io_write8(g_dev.io_base + VIRTIO_PCI_DEVICE_STATUS, status);
    g_dev.ready = 1u;
    (void)printf("[virtio-rng] vq ready qsize=%d ring_phys=0x%08X pool_phys=0x%08X\n", qsize,
                 (unsigned)(g_rq.region_phys & 0xFFFFFFFFu), (unsigned)(g_pool_phys & 0xFFFFFFFFu));
    return 0;
}

/* Fill up to `want` bytes of entropy into the DMA pool and return the count the
 * device produced, or -1 on ring/timeout failure. Posts the pool device-writable,
 * kicks, then polls the used ring (sleeping between checks). */
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
    for (int tries = 0; tries < RNG_POLL_MAX_TRIES; ++tries) {
        int32_t id = vring_get_used(&g_rq, &used_len);
        if (id >= 0) {
            vring_free_desc(&g_rq, (uint16_t)id);
            /* Read the ISR to de-assert any pending device interrupt line. */
            (void)wasmos_io_in8((int32_t)(g_dev.io_base + VIRTIO_PCI_ISR_STATUS));
            if (used_len > want) {
                used_len = want;
            }
            return (int)used_len;
        }
        (void)wasmos_ipc_select_wait_timeout(g_select, RNG_POLL_INTERVAL_MS);
    }
    vring_free_desc(&g_rq, (uint16_t)d);
    return -1; /* timed out */
}

static void send_error(int32_t dest, int32_t request_id, int32_t code) {
    (void)wasmos_ipc_send(dest, g_endpoint, HRNG_IPC_ERROR, request_id, code, 0, 0, 0);
}

/* HRNG_IPC_GET_BYTES_REQ: fill up to arg1 bytes of entropy into the client's
 * borrowed buffer (arg0). Replies HRNG_IPC_RESP with arg0 = bytes written. */
static void handle_get_bytes(int32_t source, int32_t request_id, int32_t buffer_id,
                             int32_t req_len) {
    if (!g_dev.present || !g_dev.ready) {
        send_error(source, request_id, HRNG_STATUS_NOT_READY);
        return;
    }
    if (req_len <= 0) {
        send_error(source, request_id, HRNG_STATUS_INVALID);
        return;
    }
    int n = rng_fill((uint32_t)req_len);
    if (n < 0) {
        send_error(source, request_id, HRNG_STATUS_TIMEOUT);
        return;
    }
    if (n > 0 && wasmos_sys_buffer_write(buffer_id, g_pool, n, 0) != 0) {
        send_error(source, request_id, HRNG_STATUS_IO_ERROR);
        return;
    }
    (void)wasmos_ipc_send(source, g_endpoint, HRNG_IPC_RESP, request_id, n, 0, 0, 0);
}

WASMOS_WASM_EXPORT int32_t initialize(int32_t proc_endpoint, int32_t ignored_arg1,
                                      int32_t ignored_arg2, int32_t ignored_arg3) {
    proc_endpoint = wasmos_startup_proc_endpoint();
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;
    if (proc_endpoint < 0) {
        return -1;
    }

    g_endpoint = wasmos_ipc_create_endpoint();
    if (g_endpoint < 0) {
        return -1;
    }
    g_select = wasmos_ipc_select_create();
    if (g_select < 0 || wasmos_ipc_select_add(g_select, g_endpoint) != 0) {
        (void)printf("[virtio-rng] select setup failed\n");
        return -1;
    }

    g_dev.present = 0u;
    g_dev.ready = 0u;
    if (probe_virtio_rng() == 0) {
        if (initialize_device() != 0) {
            (void)printf("[virtio-rng] init failed io=0x%04X dev=0x%04X\n", (unsigned)g_dev.io_base,
                         (unsigned)g_dev.device_id);
        } else {
            (void)printf("[virtio-rng] probe ok bus=%u slot=%u dev=0x%04X io=0x%04X\n",
                         (unsigned)g_dev.bus, (unsigned)g_dev.slot, (unsigned)g_dev.device_id,
                         (unsigned)g_dev.io_base);
        }
    } else {
        (void)printf("[virtio-rng] no device found\n");
    }

    /* Register under the generic "hrng" class so class-based lookup finds any
     * entropy provider uniformly; the concrete service name is "virtio-rng". */
    if (wasmos_svc_register_class(proc_endpoint, g_endpoint, "virtio-rng", "hrng", 0, 1) < 0) {
        (void)printf("[virtio-rng] register failed\n");
        return -1;
    }
    wasmos_sys_notify_ready(proc_endpoint, g_endpoint);

    for (;;) {
        while (wasmos_ipc_drain(g_endpoint) > 0) {
            wasmos_ipc_message_t msg;
            wasmos_ipc_message_read_last(&msg);
            if (msg.source < 0) {
                continue; /* stray notification / IRQ echo */
            }
            if (msg.type == HRNG_IPC_GET_BYTES_REQ) {
                handle_get_bytes(msg.source, msg.request_id, msg.arg0, msg.arg1);
            } else {
                send_error(msg.source, msg.request_id, HRNG_STATUS_INVALID);
            }
        }
        (void)wasmos_ipc_select_wait_timeout(g_select, 1000);
    }
    return 0;
}
