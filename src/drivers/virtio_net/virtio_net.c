/* virtio_net.c - Minimal VirtIO network WASM driver.
 * Provides adapter-neutral netdrv IPC for link/status queries while keeping
 * transport details private to this backend. */
#include <stdint.h>
#include <stdio.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/vring.h"
#include "wasmos_driver_abi.h"

#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

#define VIRTIO_PCI_VENDOR_ID 0x1AF4u
#define VIRTIO_NET_DEV_LEGACY 0x1000u
#define VIRTIO_NET_DEV_TRANSITIONAL 0x1041u

#define VIRTIO_PCI_DEVICE_FEATURES 0x00u
#define VIRTIO_PCI_DRIVER_FEATURES 0x04u
/* Legacy virtqueue registers (no MSI-X: device config starts at 0x14). */
#define VIRTIO_PCI_QUEUE_PFN       0x08u  /* u32: ring PFN (phys >> 12) */
#define VIRTIO_PCI_QUEUE_SIZE      0x0Cu  /* u16: selected queue size (0 = absent) */
#define VIRTIO_PCI_QUEUE_SELECT    0x0Eu  /* u16: select the queue to configure */
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10u  /* u16: doorbell — write the queue index */
#define VIRTIO_PCI_DEVICE_STATUS   0x12u
#define VIRTIO_PCI_ISR_STATUS      0x13u
#define VIRTIO_NET_CFG_MAC         0x14u
#define VIRTIO_NET_CFG_STATUS      0x1Au

#define VIRTIO_PCI_VRING_ALIGN     4096u
#define VIRTIO_NET_RX_QUEUE        0u
#define VIRTIO_NET_TX_QUEUE        1u

/* Legacy virtio-net header (no VIRTIO_NET_F_MRG_RXBUF) prepended to every
 * RX/TX buffer. The device writes it on RX and reads it on TX. */
#define VIRTIO_NET_HDR_LEN         10u
#define VIRTIO_NET_RX_BUF_SIZE     2048u  /* hdr + up to 1514-byte Ethernet frame */
#define VIRTIO_NET_RX_BUF_COUNT    64u    /* pre-posted receive buffers */

#define VIRTIO_STATUS_ACK       1u
#define VIRTIO_STATUS_DRIVER    2u
#define VIRTIO_STATUS_DRIVER_OK 4u
#define VIRTIO_STATUS_FAILED    128u

#define VIRTIO_NET_F_MAC    (1u << 5)
#define VIRTIO_NET_F_STATUS (1u << 16)
#define VIRTIO_NET_FEATURES_DRIVER (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS)

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
} virtio_net_device_t;

static int32_t g_endpoint = -1;
static virtio_net_device_t g_dev;
static netdrv_stats_t g_stats;

/* One configured virtqueue: the vring core state plus the device queue index
 * (needed by the doorbell notify callback). */
typedef struct {
    vring_t vq;
    uint16_t queue_idx;
    uint8_t ready;
} virtio_net_queue_t;

static virtio_net_queue_t g_rxq;  /* queue 0: device -> driver */
static virtio_net_queue_t g_txq;  /* queue 1: driver -> device */

/* RX packet pool: a pinned DMA region carved into VIRTIO_NET_RX_BUF_COUNT
 * fixed-size buffers, each posted to the RX queue as a device-writable
 * descriptor. g_rx_pool is the driver's linmem view; g_rx_pool_phys the device
 * address programmed into descriptors. */
static uint8_t *g_rx_pool;
static uint64_t g_rx_pool_phys;

static uint32_t
pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t reg)
{
    uint32_t address = 0x80000000u |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)function << 8) |
                       ((uint32_t)reg & 0xFCu);
    (void)wasmos_io_out32(PCI_CFG_ADDR_PORT, (int32_t)address);
    return (uint32_t)wasmos_io_in32(PCI_CFG_DATA_PORT);
}

static uint16_t
io_read16(uint16_t port)
{
    return (uint16_t)((uint32_t)wasmos_io_in16((int32_t)port) & 0xFFFFu);
}

static uint32_t
io_read32(uint16_t port)
{
    return (uint32_t)wasmos_io_in32((int32_t)port);
}

static void
io_write8(uint16_t port, uint8_t value)
{
    (void)wasmos_io_out8((int32_t)port, (int32_t)value);
}

static void
io_write16(uint16_t port, uint16_t value)
{
    (void)wasmos_io_out16((int32_t)port, (int32_t)value);
}

static void
io_write32(uint16_t port, uint32_t value)
{
    (void)wasmos_io_out32((int32_t)port, (int32_t)value);
}

static int
is_virtio_net_device(uint16_t vendor_id, uint16_t device_id)
{
    if (vendor_id != VIRTIO_PCI_VENDOR_ID) {
        return 0;
    }
    return device_id == VIRTIO_NET_DEV_LEGACY || device_id == VIRTIO_NET_DEV_TRANSITIONAL;
}

static int
probe_virtio_net(void)
{
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t function = 0; function < 8; ++function) {
                uint32_t id = pci_config_read32((uint8_t)bus, slot, function, 0x00);
                uint16_t vendor_id = (uint16_t)(id & 0xFFFFu);
                uint16_t device_id = (uint16_t)((id >> 16) & 0xFFFFu);
                uint32_t class_reg;
                uint32_t bar0;
                if (vendor_id == 0xFFFFu) {
                    if (function == 0u) {
                        break;
                    }
                    continue;
                }
                if (!is_virtio_net_device(vendor_id, device_id)) {
                    continue;
                }
                class_reg = pci_config_read32((uint8_t)bus, slot, function, 0x08);
                if (((class_reg >> 24) & 0xFFu) != 0x02u || ((class_reg >> 16) & 0xFFu) != 0x00u) {
                    continue;
                }
                bar0 = pci_config_read32((uint8_t)bus, slot, function, 0x10);
                if ((bar0 & 0x1u) == 0u) {
                    continue;
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

static void
read_mac(void)
{
    for (uint32_t i = 0; i < 6u; ++i) {
        g_dev.mac[i] = (uint8_t)(wasmos_io_in8((int32_t)(g_dev.io_base + VIRTIO_NET_CFG_MAC + i)) & 0xFF);
    }
}

/* vring doorbell: tell the device which queue has new available buffers. */
static void
virtio_net_notify(void *user)
{
    virtio_net_queue_t *q = (virtio_net_queue_t *)user;
    io_write16(g_dev.io_base + VIRTIO_PCI_QUEUE_NOTIFY, q->queue_idx);
}

/* Configure one virtqueue: select it, read its size, allocate a pinned,
 * contiguous DMA region for the ring, lay the vring out over it, and program
 * the device's QUEUE_PFN with the ring's physical page-frame number. Returns
 * the queue size on success, or -1 if the queue is absent or setup fails. */
static int
setup_queue(virtio_net_queue_t *q, uint16_t idx)
{
    io_write16(g_dev.io_base + VIRTIO_PCI_QUEUE_SELECT, idx);
    uint16_t qsize = io_read16(g_dev.io_base + VIRTIO_PCI_QUEUE_SIZE);
    if (qsize == 0u) {
        return -1;  /* queue not provided by the device */
    }

    uint64_t ring_bytes = vring_size(qsize, VIRTIO_PCI_VRING_ALIGN);
    int32_t pages = (int32_t)((ring_bytes + 0xFFFu) / 0x1000u);

    uint64_t ring_phys = 0;
    int32_t off = wasmos_region_alloc(pages, WASMOS_REGION_CACHE_WB, &ring_phys);
    if (off < 0) {
        return -1;  /* region_alloc failed (cap/window/no-linmem-window) */
    }
    /* In wasm32 a pointer is the linear-memory byte offset region_alloc returned. */
    uint8_t *ring = (uint8_t *)(uintptr_t)(uint32_t)off;
    if (vring_layout(&q->vq, ring, ring_phys, (uint64_t)pages * 0x1000u,
                     qsize, VIRTIO_PCI_VRING_ALIGN) != 0) {
        return -1;
    }
    q->queue_idx = idx;
    vring_set_notify(&q->vq, virtio_net_notify, q);

    /* Legacy transport: hand the device the ring's physical PFN. */
    io_write32(g_dev.io_base + VIRTIO_PCI_QUEUE_PFN, (uint32_t)(ring_phys >> 12));
    q->ready = 1u;
    return (int)qsize;
}

/* Allocate the RX packet pool and post every buffer to the RX queue as a
 * device-writable descriptor, then kick so the device can start filling them.
 * Returns the number of buffers posted, or -1 on failure. */
static int
rx_arm(void)
{
    int32_t pages = (int32_t)(((uint64_t)VIRTIO_NET_RX_BUF_COUNT * VIRTIO_NET_RX_BUF_SIZE
                               + 0xFFFu) / 0x1000u);
    uint64_t phys = 0;
    int32_t off = wasmos_region_alloc(pages, WASMOS_REGION_CACHE_WB, &phys);
    if (off < 0) {
        return -1;
    }
    g_rx_pool = (uint8_t *)(uintptr_t)(uint32_t)off;
    g_rx_pool_phys = phys;

    for (uint32_t i = 0; i < VIRTIO_NET_RX_BUF_COUNT; ++i) {
        int32_t d = vring_alloc_desc(&g_rxq.vq,
                                     phys + (uint64_t)i * VIRTIO_NET_RX_BUF_SIZE,
                                     VIRTIO_NET_RX_BUF_SIZE, VRING_DESC_F_WRITE);
        if (d < 0) {
            return -1;
        }
        vring_publish(&g_rxq.vq, (uint16_t)d);
    }
    vring_kick(&g_rxq.vq);
    return (int)VIRTIO_NET_RX_BUF_COUNT;
}

/* Consume one completed RX buffer: strip the virtio-net header, write the
 * Ethernet frame into `dest`'s borrowed buffer, recycle the descriptor, and
 * re-kick. Returns the frame length (>= 0), 0 if nothing is pending, -1 on a
 * delivery error. */
static int
rx_poll_one(int32_t dest)
{
    uint32_t used_len = 0;
    int32_t id = vring_get_used(&g_rxq.vq, &used_len);
    if (id < 0) {
        return 0;  /* nothing completed (or a rejected corrupt used element) */
    }

    int32_t frame_len = 0;
    /* Locate the buffer from the descriptor's device address; ignore a frame
     * that doesn't fit our fixed buffers or a bogus length from the device. */
    uint64_t addr = g_rxq.vq.desc[id].addr;
    uint32_t bidx = (uint32_t)((addr - g_rx_pool_phys) / VIRTIO_NET_RX_BUF_SIZE);
    if (bidx < VIRTIO_NET_RX_BUF_COUNT
        && used_len > VIRTIO_NET_HDR_LEN
        && used_len <= VIRTIO_NET_RX_BUF_SIZE) {
        uint8_t *buf = g_rx_pool + (uint64_t)bidx * VIRTIO_NET_RX_BUF_SIZE;
        frame_len = (int32_t)(used_len - VIRTIO_NET_HDR_LEN);
        if (wasmos_sys_buffer_write_to(WASMOS_BUFFER_KIND_FS, dest,
                                       WASMOS_BUFFER_GRANT_WRITE,
                                       buf + VIRTIO_NET_HDR_LEN,
                                       frame_len, 0) != 0) {
            frame_len = -1;
        } else {
            g_stats.rx_packets++;
        }
    }
    /* Recycle the descriptor (addr/len/flags survive device use) and re-post. */
    vring_publish(&g_rxq.vq, (uint16_t)id);
    vring_kick(&g_rxq.vq);
    return frame_len;
}

static int
initialize_device(void)
{
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
        io_write8(g_dev.io_base + VIRTIO_PCI_DEVICE_STATUS, (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }

    read_mac();
    if ((g_dev.driver_features & VIRTIO_NET_F_STATUS) != 0u) {
        g_dev.status_word = io_read16(g_dev.io_base + VIRTIO_NET_CFG_STATUS);
    } else {
        g_dev.status_word = 0u;
    }
    /* Set up the RX and TX virtqueues over driver-owned pinned DMA regions
     * before signalling DRIVER_OK, so the device sees valid rings once live.
     * RX/TX descriptor population is the next step; this establishes the rings
     * and programs their physical addresses into the device. */
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
    (void)printf("[virtio-net] vq ready rx=%d tx=%d rx_phys=0x%08X tx_phys=0x%08X\n",
                 rx_size, tx_size,
                 (unsigned)(g_rxq.vq.region_phys & 0xFFFFFFFFu),
                 (unsigned)(g_txq.vq.region_phys & 0xFFFFFFFFu));

    /* Post the receive buffers now that the device is live (RX buffers may be
     * added after DRIVER_OK). */
    int rx_bufs = rx_arm();
    if (rx_bufs < 0) {
        (void)printf("[virtio-net] rx arm failed\n");
        return -1;
    }
    (void)printf("[virtio-net] rx armed bufs=%d rx_pool=0x%08X\n",
                 rx_bufs, (unsigned)(g_rx_pool_phys & 0xFFFFFFFFu));
    return 0;
}

static void
send_error(int32_t dest, int32_t request_id, int32_t code)
{
    (void)wasmos_ipc_send(dest, g_endpoint, NETDRV_IPC_ERROR, request_id, code, 0, 0, 0);
}

static void
handle_link_get(int32_t source, int32_t request_id)
{
    int32_t link_up;
    if (!g_dev.present || !g_dev.ready) {
        send_error(source, request_id, NET_STATUS_NOT_READY);
        return;
    }
    if (wasmos_sys_buffer_write_to(WASMOS_BUFFER_KIND_FS,
                                   source,
                                   WASMOS_BUFFER_GRANT_WRITE,
                                   g_dev.mac,
                                   6,
                                   0) != 0) {
        send_error(source, request_id, NET_STATUS_IO_ERROR);
        return;
    }
    link_up = ((g_dev.status_word & VIRTIO_NET_S_LINK_UP) != 0u) ? 1 : 0;
    (void)wasmos_ipc_send(source,
                          g_endpoint,
                          NETDRV_IPC_RESP,
                          request_id,
                          link_up,
                          (int32_t)g_dev.status_word,
                          (int32_t)VIRTIO_NET_MTU_BASELINE,
                          0);
}

static void
handle_stats_get(int32_t source, int32_t request_id)
{
    if (!g_dev.present || !g_dev.ready) {
        send_error(source, request_id, NET_STATUS_NOT_READY);
        return;
    }
    if (wasmos_sys_buffer_write_to(WASMOS_BUFFER_KIND_FS,
                                   source,
                                   WASMOS_BUFFER_GRANT_WRITE,
                                   &g_stats,
                                   (int32_t)sizeof(g_stats),
                                   0) != 0) {
        send_error(source, request_id, NET_STATUS_IO_ERROR);
        return;
    }
    (void)wasmos_ipc_send(source, g_endpoint, NETDRV_IPC_RESP, request_id, NET_STATUS_OK, 0, 0, 0);
}

/* NETDRV_IPC_RX_POLL: deliver the next received frame (if any) into the caller's
 * borrowed buffer. Replies RESP with arg0 = frame length; 0 means no frame is
 * currently pending. */
static void
handle_rx_poll(int32_t source, int32_t request_id)
{
    if (!g_dev.present || !g_dev.ready) {
        send_error(source, request_id, NET_STATUS_NOT_READY);
        return;
    }
    int frame_len = rx_poll_one(source);
    if (frame_len < 0) {
        send_error(source, request_id, NET_STATUS_IO_ERROR);
        return;
    }
    (void)wasmos_ipc_send(source, g_endpoint, NETDRV_IPC_RESP, request_id,
                          frame_len, 0, 0, 0);
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint, int32_t ignored_arg1, int32_t ignored_arg2, int32_t ignored_arg3)
{
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

    g_dev.present = 0u;
    g_dev.ready = 0u;
    g_dev.status_word = 0u;
    (void)probe_virtio_net();
    if (g_dev.present) {
        if (initialize_device() != 0) {
            (void)printf("[virtio-net] init failed io=0x%04X dev=0x%04X\n",
                         (unsigned)g_dev.io_base,
                         (unsigned)g_dev.device_id);
        } else {
            (void)printf("[virtio-net] probe ok bus=%u slot=%u dev=0x%04X irq=%u\n",
                         (unsigned)g_dev.bus,
                         (unsigned)g_dev.slot,
                         (unsigned)g_dev.device_id,
                         (unsigned)g_dev.irq);
            (void)printf("[virtio-net] mac %02X:%02X:%02X:%02X:%02X:%02X io=0x%04X\n",
                         (unsigned)g_dev.mac[0],
                         (unsigned)g_dev.mac[1],
                         (unsigned)g_dev.mac[2],
                         (unsigned)g_dev.mac[3],
                         (unsigned)g_dev.mac[4],
                         (unsigned)g_dev.mac[5],
                         (unsigned)g_dev.io_base);
            (void)printf("[virtio-net] features dev=0x%08X drv=0x%08X\n",
                         (unsigned)g_dev.device_features,
                         (unsigned)g_dev.driver_features);
            (void)printf("[virtio-net] driver ok link=%s mtu=%u\n",
                         ((g_dev.status_word & VIRTIO_NET_S_LINK_UP) != 0u) ? "up" : "down",
                         (unsigned)VIRTIO_NET_MTU_BASELINE);
        }
    } else {
        (void)printf("[virtio-net] no device found\n");
    }

    if (wasmos_svc_register(proc_endpoint, g_endpoint, "virtio.net", 1) != 0) {
        (void)printf("[virtio-net] register failed\n");
        return -1;
    }
    wasmos_sys_notify_ready(proc_endpoint, g_endpoint);

    for (;;) {
        wasmos_ipc_message_t msg;
        if (wasmos_ipc_select_one(g_endpoint) != 1) {
            continue;
        }
        wasmos_ipc_message_read_last(&msg);
        if (msg.source < 0) {
            continue;
        }
        if (msg.type == NETDRV_IPC_LINK_GET) {
            handle_link_get(msg.source, msg.request_id);
        } else if (msg.type == NETDRV_IPC_STATS_GET) {
            handle_stats_get(msg.source, msg.request_id);
        } else if (msg.type == NETDRV_IPC_RX_POLL) {
            handle_rx_poll(msg.source, msg.request_id);
        } else if (msg.type == NETDRV_IPC_TX_FRAME) {
            send_error(msg.source, msg.request_id, NET_STATUS_NOT_READY);
        } else {
            send_error(msg.source, msg.request_id, NET_STATUS_INVALID);
        }
    }
    return 0;
}
