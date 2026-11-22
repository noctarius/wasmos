/* virtio_serial.c - virtio-console driver: the kernel-log port.
 *
 * Streams the kernel log to port 0 of a virtio console (virtio 1.2 §5.3),
 * which the QEMU configuration connects to a host file. It is the kernel-log
 * port that replaces the second UART (log.serial=com2, still available on
 * x86_64): the kernel itself drives no device for it. This driver subscribes to
 * the kmsg ring (kmsg_subscribe / WASMOS_IPC_KMSG_NOTIFY), replays it from the
 * oldest retained byte when it starts, and then writes every new record, so the
 * port carries the complete, in-order kernel log from boot on as long as the
 * ring has not overwritten anything before the driver read it. Bytes it did
 * overwrite are reported in place by a "[virtio-serial] N bytes ..." line
 * (logger_stage.c, shared with the logger service).
 *
 * What the port cannot carry: output after the driver is gone -- a panic, the
 * end of a shutdown -- and anything older than the 64 KiB ring when the driver
 * starts. Panic output still reaches COM1, and log.serial=com2 is the port for
 * a boot that never reaches user space.
 *
 * Transport: virtio 1.x PCI (virtio_modern.h), registers in BAR 4, granted as
 * region 0. Only port 0's transmit queue is used: MULTIPORT is not accepted, so
 * the device has exactly one port and no control queues, and nothing is read
 * from the host. Transmit completions are reclaimed when a buffer is needed
 * rather than signalled, so the driver binds no interrupt and tells the device
 * to raise none (vring_disable_interrupts). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "logger_stage.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos/virtio_modern.h"
#include "wasmos/vring.h"
#include "wasmos_driver_abi.h"

/* PCI identity of a virtio console: 0x1AF4 is the Red Hat / virtio vendor id,
 * 0x1003 the transitional device and 0x1043 the modern-only one (0x1040 +
 * virtio device type 3). Both offer the modern interface this driver uses. */
#define VIRTIO_PCI_VENDOR_ID 0x1AF4u
#define VIRTIO_CONSOLE_DEV_TRANSITIONAL 0x1003u
#define VIRTIO_CONSOLE_DEV_MODERN 0x1043u

/* The BAR holding the virtio structures, and the region the manifest grants it
 * as. QEMU places every modern structure in BAR 4; a device that does not is
 * refused by virtio_modern_probe. */
#define VIRTIO_CONSOLE_BAR 4u
#define VIRTIO_CONSOLE_REGION 0

/* transmitq0, port 0's output (virtio 1.2 §5.3.2); receiveq0 is queue 0 and is
 * left unconfigured, since nothing is read from the host. */
#define VIRTIO_CONSOLE_TX_QUEUE 1u
#define VIRTIO_CONSOLE_MAX_QUEUE 128u

/* Transmit buffers: one staged chunk of kernel log each (a loss marker plus up
 * to LOGGER_CHUNK bytes), handed to the device whole. */
#define TX_BUF_SIZE 4096u
#define TX_BUF_COUNT 16u
_Static_assert(TX_BUF_SIZE >= LOGGER_MARKER_MAX + LOGGER_CHUNK, "a staged chunk fits one buffer");

/* How long to park when every transmit buffer is still the device's. The device
 * completes a write as soon as the host has it, so this only paces a full
 * queue; it is a bounded park on the service endpoint, not a spin. */
#define TX_FULL_WAIT_MS 10

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t device_id;
} virtio_console_device_t;

static int32_t g_endpoint = -1;
static int32_t g_select = -1;
static virtio_console_device_t g_dev;
static virtio_modern_t g_vm;
static vring_t g_txq;
static uint32_t g_tx_notify;
static uint8_t* g_tx_pool;
static uint64_t g_tx_pool_phys;
static uint16_t g_tx_free[TX_BUF_COUNT];
static uint32_t g_tx_free_top;
static uint16_t g_tx_desc_buf[VIRTIO_CONSOLE_MAX_QUEUE];
static logger_stage_t g_stage;

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
    uint32_t key_len = (uint32_t)strlen(key);
    uint32_t i = 0;
    for (;;) {
        while (args[i] == ' ') {
            i++;
        }
        if (args[i] == '\0') {
            return 0;
        }
        if (strncmp(&args[i], key, key_len) == 0) {
            return &args[i + key_len];
        }
        while (args[i] != '\0' && args[i] != ' ') {
            i++;
        }
    }
}

/* A PCI-rule spawn names the matched function in the startup args; the driver
 * is spawned for exactly that device. */
static int probe_from_startup_args(void) {
    char args[128];
    uint32_t bus = 0, slot = 0, function = 0, vendor_id = 0, device_id = 0;
    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return -1;
    }
    const char* pci = find_token_value(args, "pci=");
    const char* vendor = find_token_value(args, "vendor=");
    const char* device = find_token_value(args, "device=");
    if (!pci || !vendor || !device || parse_hex_n(pci, 2u, &bus) != 0 || pci[2] != ':' ||
        parse_hex_n(pci + 3, 2u, &slot) != 0 || pci[5] != '.' ||
        parse_hex_n(pci + 6, 2u, &function) != 0 || parse_hex_n(vendor, 4u, &vendor_id) != 0 ||
        parse_hex_n(device, 4u, &device_id) != 0) {
        return -1;
    }
    if (vendor_id != VIRTIO_PCI_VENDOR_ID ||
        (device_id != VIRTIO_CONSOLE_DEV_TRANSITIONAL && device_id != VIRTIO_CONSOLE_DEV_MODERN)) {
        return -1;
    }
    g_dev.bus = (uint8_t)bus;
    g_dev.slot = (uint8_t)slot;
    g_dev.function = (uint8_t)function;
    g_dev.device_id = (uint16_t)device_id;
    return 0;
}

static void tx_notify(void* user) {
    (void)user;
    virtio_modern_notify(&g_vm, g_tx_notify, (uint16_t)VIRTIO_CONSOLE_TX_QUEUE);
}

/* Brings the device up with port 0's transmit queue only: no feature beyond
 * VERSION_1 (no MULTIPORT, so one port and no control queues), no interrupt.
 * Returns 0, or -1 with the device marked FAILED. */
static int initialize_device(void) {
    uint8_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
    uint32_t bdf =
        ((uint32_t)g_dev.bus << 8) | ((uint32_t)g_dev.slot << 3) | (uint32_t)g_dev.function;
    if (virtio_modern_probe(&g_vm, bdf, VIRTIO_CONSOLE_BAR, VIRTIO_CONSOLE_REGION) != 0) {
        (void)printf("[virtio-serial] no virtio-modern structures in BAR %u\n",
                     (unsigned)VIRTIO_CONSOLE_BAR);
        return -1;
    }
    if (virtio_modern_negotiate(&g_vm, 0u, 0, 0) != 0) {
        (void)printf("[virtio-serial] feature negotiation failed\n");
        return -1;
    }
    uint16_t qsize = virtio_modern_queue_size(&g_vm, (uint16_t)VIRTIO_CONSOLE_TX_QUEUE);
    if (qsize == 0u) {
        virtio_modern_set_status(&g_vm, (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }
    if (qsize > VIRTIO_CONSOLE_MAX_QUEUE) {
        qsize = VIRTIO_CONSOLE_MAX_QUEUE;
    }
    uint64_t ring_bytes = vring_size(qsize, VIRTIO_MODERN_VRING_ALIGN);
    int32_t ring_pages = (int32_t)((ring_bytes + 0xFFFu) / 0x1000u);
    uint64_t ring_phys = 0;
    int32_t ring_off = wasmos_region_alloc(ring_pages, WASMOS_REGION_CACHE_WB, &ring_phys);
    int32_t pool_pages = (int32_t)((TX_BUF_COUNT * TX_BUF_SIZE) / 0x1000u);
    int32_t pool_off = wasmos_region_alloc(pool_pages, WASMOS_REGION_CACHE_WB, &g_tx_pool_phys);
    if (ring_off < 0 || pool_off < 0 ||
        vring_layout(&g_txq,
                     ptr_cast(uint8_t, (uint32_t)ring_off),
                     ring_phys,
                     (uint64_t)ring_pages * 0x1000u,
                     qsize,
                     VIRTIO_MODERN_VRING_ALIGN) != 0) {
        virtio_modern_set_status(&g_vm, (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }
    vring_set_notify(&g_txq, tx_notify, 0);
    /* No interrupt is bound, so none may be raised: the console's INTx line
     * can be shared (under QEMU with a virtio-blk disk on its INTx fallback),
     * and an interrupt this driver never acknowledges would re-fire on that
     * line for the driver that routes it. */
    vring_disable_interrupts(&g_txq);
    if (virtio_modern_queue_enable(&g_vm,
                                   (uint16_t)VIRTIO_CONSOLE_TX_QUEUE,
                                   &g_txq,
                                   (uint16_t)VIRTIO_MSI_NO_VECTOR,
                                   &g_tx_notify) != 0) {
        virtio_modern_set_status(&g_vm, (uint8_t)(status | VIRTIO_STATUS_FAILED));
        return -1;
    }
    g_tx_pool = ptr_cast(uint8_t, (uint32_t)pool_off);
    for (uint32_t i = 0; i < TX_BUF_COUNT; ++i) {
        g_tx_free[i] = (uint16_t)i;
    }
    g_tx_free_top = TX_BUF_COUNT;
    virtio_modern_set_status(&g_vm, (uint8_t)(status | VIRTIO_STATUS_DRIVER_OK));
    return 0;
}

/* Returns every buffer the device has finished with to the free stack. */
static void tx_reclaim(void) {
    uint32_t len = 0;
    int32_t id;
    while ((id = vring_get_used(&g_txq, &len)) >= 0) {
        if ((uint32_t)id < VIRTIO_CONSOLE_MAX_QUEUE && g_tx_free_top < TX_BUF_COUNT) {
            g_tx_free[g_tx_free_top++] = g_tx_desc_buf[id];
        }
        vring_free_desc(&g_txq, (uint16_t)id);
    }
}

/* Hands `len` bytes to the device as one device-readable buffer. When every
 * buffer is still the device's, parks briefly on the service endpoint and
 * reclaims again; the device frees a buffer as soon as the host has its bytes. */
static void tx_write(const char* data, uint32_t len) {
    for (;;) {
        tx_reclaim();
        if (g_tx_free_top > 0u) {
            uint16_t b = g_tx_free[--g_tx_free_top];
            int32_t d;
            memcpy(g_tx_pool + (uint32_t)b * TX_BUF_SIZE, data, len);
            d = vring_alloc_desc(&g_txq, g_tx_pool_phys + (uint64_t)b * TX_BUF_SIZE, len, 0u);
            if (d < 0) {
                g_tx_free[g_tx_free_top++] = b;
            } else {
                g_tx_desc_buf[d] = b;
                vring_publish(&g_txq, (uint16_t)d);
                vring_kick(&g_txq);
                return;
            }
        }
        if (!wasmos_sys_wait_parked(wasmos_ipc_select_wait_timeout(g_select, TX_FULL_WAIT_MS))) {
            (void)wasmos_sched_yield();
        }
    }
}

static int32_t stage_read(void* user, uint8_t* dst, int32_t len, uint64_t* seq) {
    (void)user;
    return wasmos_kmsg_read(dst, len, seq);
}

/* Writes everything the ring holds past the last byte written. */
static void drain_kernel_log(void) {
    for (;;) {
        int32_t n = logger_stage_next(&g_stage, stage_read, 0);
        if (n <= 0) {
            return;
        }
        tx_write(g_stage.out, (uint32_t)n);
    }
}

/* Driver entry point: bring the console up, follow the kernel log forever.
 * Returns only on a bring-up failure, with a packed WASMOS_ERR_DRIVER_* code. */
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
        return WASMOS_ERR_DRIVER_SELECT_SETUP;
    }
    if (probe_from_startup_args() != 0) {
        (void)printf("[virtio-serial] startup args name no virtio console\n");
        return WASMOS_ERR_DRIVER_NO_DEVICE_IDENTITY;
    }
    if (initialize_device() != 0) {
        (void)printf("[virtio-serial] init failed dev=0x%04X\n", (unsigned)g_dev.device_id);
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    memset(&g_stage, 0, sizeof(g_stage));
    g_stage.tag = "[virtio-serial]";
    /* Subscribe before the first drain, so nothing logged between the replay
     * and the subscription goes unannounced. */
    int32_t rc = wasmos_kmsg_subscribe(g_endpoint);
    if (rc != 0) {
        (void)printf("[virtio-serial] kmsg_subscribe failed rc=%d\n", (int)rc);
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    (void)printf("[virtio-serial] kernel log on virtio console bus=%u slot=%u dev=0x%04X\n",
                 (unsigned)g_dev.bus,
                 (unsigned)g_dev.slot,
                 (unsigned)g_dev.device_id);
    wasmos_sys_notify_ready(proc_endpoint, g_endpoint);
    drain_kernel_log();

    for (;;) {
        /* Every message is either a KMSG_NOTIFY or something nobody should send
         * here; either way the answer is to drain. No request is served, so
         * nothing is replied to. */
        while (wasmos_ipc_drain(g_endpoint) > 0) {
        }
        drain_kernel_log();
        if (!wasmos_sys_wait_parked(wasmos_ipc_select_wait(g_select))) {
            /* A failed wait returns at once; yield rather than spin.
             * FIXME: with a dead select set there is nothing left to park on. */
            (void)wasmos_sched_yield();
        }
    }
    return 0;
}
