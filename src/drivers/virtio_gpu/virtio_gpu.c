/* virtio_gpu.c - virtio-gpu driver (virtio 1.2 §5.7): presents the scanout.
 *
 * The scanout is not this driver's memory. edk2's virtio-gpu GOP is blit-only,
 * so the boot loader allocates a linear BGRX framebuffer and hands it to the
 * kernel (docs/architecture/04-boot-and-handoff.md); the kernel, the panic
 * writer, the FBTEXT server and the compositor all draw into it. This driver
 * makes those pixels visible: it creates one 2D resource the size of the
 * framebuffer, attaches the framebuffer's physical pages as its backing, binds
 * it to scanout 0, and from then on copies damaged regions to the host
 * (TRANSFER_TO_HOST_2D) and presents them (RESOURCE_FLUSH).
 *
 * What to present is reported by the writers over DISPLAY_IPC_FLUSH_REQ, served
 * under the name "display" (abi/opcodes.yaml). A writer that reports nothing --
 * the kernel's own console -- is caught by a full-screen present on every loop
 * tick that finds the control queue idle (VGPU_TICK_MS).
 *
 * Transport: virtio 1.x PCI (virtio_modern.h), registers in BAR 4 as region 0.
 * Only the control queue is driven; the cursor queue is left unconfigured and
 * no feature beyond VERSION_1 is accepted (no VIRGL, no EDID).
 * TODO: 3D (VIRTIO_GPU_F_VIRGL contexts and command submission) and runtime
 * mode setting are not driven; the scanout keeps the boot mode.
 *
 * Every exchange with another process is a loop intent, never a synchronous
 * call (AGENTS.md); bring-up follows virtio_input.c: pci-bus through a service
 * watch, MSI-X query and bind as intents, INTx when MSI-X is unavailable, and
 * polling on the loop tick when neither is. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos/virtio_modern.h"
#include "wasmos/vring.h"
#include "wasmos_driver_abi.h"
#include "virtio_gpu_damage.h"

/* PCI identity: 0x1AF4 is the virtio vendor id, 0x1050 the modern virtio-gpu
 * function (0x1040 + device type 16); there is no transitional one. */
#define VIRTIO_PCI_VENDOR_ID 0x1AF4u
#define VIRTIO_GPU_DEV_MODERN 0x1050u

#define VIRTIO_GPU_BAR 4u
#define VIRTIO_GPU_REGION 0

#define VIRTIO_GPU_CONTROL_QUEUE 0u
#define VIRTIO_GPU_MAX_QUEUE 64u
#define VIRTIO_GPU_MSIX_ENTRY_QUEUE 0u

/* Control commands and responses (virtio 1.2 §5.7.6.7). */
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101u
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define VIRTIO_GPU_RESP_OK_NODATA 0x1100u

/* Memory byte order B, G, R, X: a little-endian 0x00RRGGBB word, which is what
 * every writer of the framebuffer stores. */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2u

/* The one resource and the one scanout this driver uses. Resource id 0 is
 * reserved by the protocol. */
#define VGPU_RESOURCE_ID 1u
#define VGPU_SCANOUT_ID 0u

/* Interrupt delivery on the service endpoint, as in virtio_input.c. Both kinds
 * carry the interrupt id as their request_id, so the loop's own request ids
 * start far above any interrupt id. */
#define VGPU_IRQ_EVENT_TYPE 0xFF00
#define VGPU_LOOP_REQ_BASE 0x7B0000

#define VGPU_PCI_WAIT_MS 2000
/* The loop tick, and with it the fallback present's period. */
#define VGPU_TICK_MS 250

/* Command slots: one request and its response per slot, in one DMA page. A
 * slot is a two-descriptor chain on the control queue. */
#define VGPU_SLOTS 8u
#define VGPU_SLOT_BYTES 512u
#define VGPU_RESP_OFFSET 256u
_Static_assert(VGPU_SLOTS* VGPU_SLOT_BYTES <= 4096u, "every slot fits the command page");

/* Flush requests that arrived while the control queue had no room. */
#define VGPU_PENDING_MAX 8u

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} vgpu_ctrl_hdr_t;
_Static_assert(sizeof(vgpu_ctrl_hdr_t) == 24u, "virtio_gpu_ctrl_hdr is 24 bytes");

typedef struct {
    vgpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} vgpu_resource_create_2d_t;

typedef struct {
    vgpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    uint64_t addr; /* the single virtio_gpu_mem_entry that follows */
    uint32_t length;
    uint32_t padding;
} vgpu_attach_backing_t;

typedef struct {
    vgpu_ctrl_hdr_t hdr;
    wasmos_display_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} vgpu_set_scanout_t;

typedef struct {
    vgpu_ctrl_hdr_t hdr;
    wasmos_display_rect_t r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} vgpu_transfer_to_host_2d_t;

typedef struct {
    vgpu_ctrl_hdr_t hdr;
    wasmos_display_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} vgpu_resource_flush_t;

/* One present -- TRANSFER_TO_HOST_2D then RESOURCE_FLUSH of one region -- and
 * whom to answer when both are done: a client's request, or nobody for the
 * fallback and the first present. */
typedef struct {
    uint8_t in_use;
    uint8_t remaining; /* commands not yet completed */
    int32_t status;    /* first failure, 0 while none */
    int32_t client;    /* endpoint to answer, -1 for none */
    int32_t request_id;
} vgpu_present_t;

typedef void (*vgpu_done_fn)(void* user, int32_t status);

typedef struct {
    uint8_t in_use;
    uint16_t head;
    vgpu_done_fn done;
    void* user;
} vgpu_slot_t;

typedef struct {
    int32_t client;
    int32_t request_id;
    int32_t buffer_id;
    int32_t offset;
    int32_t size;
} vgpu_pending_t;

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t irq;
    uint8_t msix_enabled;
    uint8_t irq_routed;
    uint32_t msix_vector;
    virtio_modern_t vm;
    uint32_t notify;
} vgpu_device_t;

static int32_t g_proc_ep = -1;
static int32_t g_ep = -1;
static int32_t g_pci_ep = -1;
static wasmos_sys_event_loop_t g_loop;
static wasmos_sys_svc_watch_set_t g_watches;
static wasmos_sys_svc_watch_t g_pci_watch;
/* The FBTEXT server ("fb"), which draws the text console into the scanout and
 * reports what it drew once told who presents it (FBTEXT_IPC_PRESENTER_REQ). */
static wasmos_sys_svc_watch_t g_fb_watch;
static int32_t g_fb_ep = -1;
static int32_t g_register_bid = -1;
static uint8_t g_bringup_done;
static uint8_t g_live; /* the scanout is bound; flushes are served */
static vgpu_device_t g_dev;

static wasmos_framebuffer_info_t g_fb;
static vring_t g_ctrlq;
static uint8_t* g_cmd;
static uint64_t g_cmd_phys;
static vgpu_slot_t g_slots[VGPU_SLOTS];
static vgpu_present_t g_presents[VGPU_SLOTS / 2u];
static vgpu_pending_t g_pending[VGPU_PENDING_MAX];
static uint32_t g_pending_head;
static uint32_t g_pending_count;
/* Clients that have flushed, each logged on its first flush: the boot log then
 * shows which writers report their damage (the compositor and the text
 * console), as opposed to being caught by the periodic present. */
#define VGPU_CLIENTS_LOGGED 8u
static int32_t g_clients_seen[VGPU_CLIENTS_LOGGED];
static uint32_t g_clients_seen_count;

static void vgpu_log(const char* msg) {
    (void)printf("%s", msg);
}

static uint32_t vgpu_bdf(void) {
    return ((uint32_t)g_dev.bus << 8) | ((uint32_t)g_dev.slot << 3) | (uint32_t)g_dev.function;
}

static void vgpu_fatal(const char* msg, int32_t status) {
    vgpu_log(msg);
    (void)virtio_modern_reset(&g_dev.vm);
    (void)wasmos_proc_exit(status);
    __builtin_trap();
}

static int parse_hex_n(const char* s, uint32_t digits, uint32_t* out) {
    uint32_t value = 0;
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

/* A PCI-rule spawn names the matched function and its INTx line in the startup
 * args; the instance drives exactly that function. */
static int probe_from_startup_args(void) {
    char args[128];
    uint32_t bus = 0, slot = 0, function = 0, vendor_id = 0, device_id = 0, irq = 0;
    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return -1;
    }
    const char* pci = find_token_value(args, "pci=");
    const char* vendor = find_token_value(args, "vendor=");
    const char* device = find_token_value(args, "device=");
    const char* irq_arg = find_token_value(args, "irq=");
    if (!pci || !vendor || !device || !irq_arg || parse_hex_n(pci, 2u, &bus) != 0 ||
        pci[2] != ':' || parse_hex_n(pci + 3, 2u, &slot) != 0 || pci[5] != '.' ||
        parse_hex_n(pci + 6, 2u, &function) != 0 || parse_hex_n(vendor, 4u, &vendor_id) != 0 ||
        parse_hex_n(device, 4u, &device_id) != 0 || parse_hex_n(irq_arg, 2u, &irq) != 0) {
        return -1;
    }
    if (vendor_id != VIRTIO_PCI_VENDOR_ID || device_id != VIRTIO_GPU_DEV_MODERN) {
        return -1;
    }
    g_dev.bus = (uint8_t)bus;
    g_dev.slot = (uint8_t)slot;
    g_dev.function = (uint8_t)function;
    g_dev.irq = (uint8_t)irq;
    return 0;
}

// ------------------------------------------------------------------ commands

static void vgpu_ctrlq_notify(void* user) {
    (void)user;
    virtio_modern_notify(&g_dev.vm, g_dev.notify, (uint16_t)VIRTIO_GPU_CONTROL_QUEUE);
}

static uint32_t vgpu_free_slots(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < VGPU_SLOTS; ++i) {
        n += g_slots[i].in_use ? 0u : 1u;
    }
    return n;
}

/* Posts one command: `len` bytes already written at the slot's request area,
 * answered into its response area. Returns 0, or -1 when no slot or
 * descriptor is free. Does not kick. */
static int vgpu_post(uint32_t s, uint32_t len, vgpu_done_fn done, void* user) {
    uint64_t base = g_cmd_phys + (uint64_t)s * VGPU_SLOT_BYTES;
    vring_buf_t bufs[2] = {
        {base, len, 0u},
        {base + VGPU_RESP_OFFSET, (uint32_t)sizeof(vgpu_ctrl_hdr_t), VRING_DESC_F_WRITE},
    };
    int32_t head = vring_alloc_chain(&g_ctrlq, bufs, 2u);
    if (head < 0) {
        return -1;
    }
    g_slots[s].in_use = 1u;
    g_slots[s].head = (uint16_t)head;
    g_slots[s].done = done;
    g_slots[s].user = user;
    vring_publish(&g_ctrlq, (uint16_t)head);
    return 0;
}

/* A free slot's request area, zeroed, or NULL with *out_slot untouched. */
static void* vgpu_slot_request(uint32_t* out_slot) {
    for (uint32_t i = 0; i < VGPU_SLOTS; ++i) {
        if (!g_slots[i].in_use) {
            void* req = g_cmd + i * VGPU_SLOT_BYTES;
            memset(req, 0, VGPU_SLOT_BYTES);
            *out_slot = i;
            return req;
        }
    }
    return 0;
}

/* Completes every command the device has answered. */
static void vgpu_drain_ctrlq(void) {
    uint32_t len = 0;
    int32_t id;
    while ((id = vring_get_used(&g_ctrlq, &len)) >= 0) {
        vring_free_chain(&g_ctrlq, (uint16_t)id);
        for (uint32_t i = 0; i < VGPU_SLOTS; ++i) {
            if (!g_slots[i].in_use || g_slots[i].head != (uint16_t)id) {
                continue;
            }
            const vgpu_ctrl_hdr_t* resp =
                (const vgpu_ctrl_hdr_t*)(g_cmd + i * VGPU_SLOT_BYTES + VGPU_RESP_OFFSET);
            int32_t status =
                resp->type == VIRTIO_GPU_RESP_OK_NODATA ? 0 : WASMOS_ERR_DISPLAY_DEVICE;
            vgpu_done_fn done = g_slots[i].done;
            void* user = g_slots[i].user;
            g_slots[i].in_use = 0u;
            if (status != 0) {
                (void)printf("[virtio-gpu] command answered 0x%x\n", (unsigned)resp->type);
            }
            if (done) {
                done(user, status);
            }
            break;
        }
    }
}

// ------------------------------------------------------------------ presents

static void vgpu_serve_pending(void);

/* Answers a client's flush could not be delivered (its queue was full) and are
 * retried after every loop poll: a writer keeps one flush in flight and sends no
 * other until it is answered, so a lost answer would stop it reporting damage
 * for good. A client's hangup drops its entries. When the store is full the
 * oldest entry is dropped -- that client then reports only once its own
 * periodic damage resumes after a rebind. */
#define VGPU_UNSENT_MAX 16u
typedef struct {
    int32_t client;
    int32_t request_id;
    int32_t status;
} vgpu_unsent_t;
static vgpu_unsent_t g_unsent[VGPU_UNSENT_MAX];
static uint32_t g_unsent_count;

static int vgpu_send_answer(int32_t client, int32_t request_id, int32_t status) {
    return status == 0
               ? wasmos_ipc_send(client, g_ep, DISPLAY_IPC_RESP, request_id, 0, 0, 0, 0)
               : wasmos_ipc_send(client, g_ep, DISPLAY_IPC_ERROR, request_id, status, 0, 0, 0);
}

/* Answers a flush: DISPLAY_IPC_RESP for status 0, else DISPLAY_IPC_ERROR. */
static void vgpu_answer(int32_t client, int32_t request_id, int32_t status) {
    if (client < 0 || vgpu_send_answer(client, request_id, status) == 0) {
        return;
    }
    if (g_unsent_count == VGPU_UNSENT_MAX) {
        vgpu_log("[virtio-gpu] answer store full; dropping the oldest answer\n");
        for (uint32_t i = 1; i < VGPU_UNSENT_MAX; ++i) {
            g_unsent[i - 1u] = g_unsent[i];
        }
        g_unsent_count--;
    }
    g_unsent[g_unsent_count++] = (vgpu_unsent_t){client, request_id, status};
}

/* Retries every stored answer, keeping those still refused, in order. */
static void vgpu_retry_answers(void) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_unsent_count; ++i) {
        if (vgpu_send_answer(g_unsent[i].client, g_unsent[i].request_id, g_unsent[i].status) != 0) {
            g_unsent[kept++] = g_unsent[i];
        }
    }
    g_unsent_count = kept;
}

/* A client ended: its stored answers and its queued requests are dropped. */
static void vgpu_forget_client(int32_t client) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_unsent_count; ++i) {
        if (g_unsent[i].client != client) {
            g_unsent[kept++] = g_unsent[i];
        }
    }
    g_unsent_count = kept;
    for (uint32_t i = 0; i < g_pending_count; ++i) {
        vgpu_pending_t* req = &g_pending[(g_pending_head + i) % VGPU_PENDING_MAX];
        if (req->client == client) {
            req->client = -1;
        }
    }
}

static void vgpu_present_done(void* user, int32_t status) {
    vgpu_present_t* p = (vgpu_present_t*)user;
    if (status != 0 && p->status == 0) {
        p->status = status;
    }
    if (--p->remaining != 0u) {
        return;
    }
    vgpu_answer(p->client, p->request_id, p->status);
    p->in_use = 0u;
    vgpu_serve_pending();
}

/* Copies `region` of the framebuffer to the host resource and presents it.
 * `client` (or -1) is answered once both commands have completed. Returns 0, or
 * -1 when two slots are not free (the caller queues the request). An empty
 * region presents nothing and is answered at once. */
static int vgpu_present(vgpu_region_t region, int32_t client, int32_t request_id) {
    if (region.width == 0u || region.height == 0u) {
        vgpu_answer(client, request_id, 0);
        return 0;
    }
    vgpu_present_t* p = 0;
    for (uint32_t i = 0; i < VGPU_SLOTS / 2u; ++i) {
        if (!g_presents[i].in_use) {
            p = &g_presents[i];
            break;
        }
    }
    if (!p || vgpu_free_slots() < 2u || g_ctrlq.num_free < 4u) {
        return -1;
    }
    p->in_use = 1u;
    p->remaining = 2u;
    p->status = 0;
    p->client = client;
    p->request_id = request_id;

    uint32_t s = 0;
    vgpu_transfer_to_host_2d_t* xfer = (vgpu_transfer_to_host_2d_t*)vgpu_slot_request(&s);
    xfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer->r = region;
    /* Byte offset of the region's first pixel in the backing, whose rows are
     * the framebuffer's stride. */
    xfer->offset = ((uint64_t)region.y * g_fb.framebuffer_stride + region.x) * 4u;
    xfer->resource_id = VGPU_RESOURCE_ID;
    (void)vgpu_post(s, (uint32_t)sizeof(*xfer), vgpu_present_done, p);

    vgpu_resource_flush_t* flush = (vgpu_resource_flush_t*)vgpu_slot_request(&s);
    flush->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush->r = region;
    flush->resource_id = VGPU_RESOURCE_ID;
    (void)vgpu_post(s, (uint32_t)sizeof(*flush), vgpu_present_done, p);
    /* The device executes the control queue in order, so the flush presents
     * what the transfer copied. */
    vring_kick(&g_ctrlq);
    return 0;
}

static vgpu_region_t vgpu_full_region(void) {
    vgpu_region_t r = {0, 0, g_fb.framebuffer_width, g_fb.framebuffer_height};
    return r;
}

/* Reads one queued request's descriptor and presents it. Returns 0 when the
 * request was taken (presented, refused or answered), -1 when the queue has no
 * room for it yet. */
static int vgpu_take_request(const vgpu_pending_t* req) {
    wasmos_display_flush_desc_t desc;
    int32_t size = req->size;
    if (req->client < 0) {
        return 0; /* the client ended while the request waited */
    }
    if (size > (int32_t)sizeof(desc)) {
        size = (int32_t)sizeof(desc);
    }
    memset(&desc, 0, sizeof(desc));
    int32_t status = 0;
    /* A negative offset or size is refused here, not left to the host call:
     * with a non-negative offset and a size bounded by the descriptor, the
     * range the kernel checks cannot wrap.
     * TODO: WARP's xfer_buffer_read adds offset + len in 32 bits, so the
     * kernel itself must refuse a wrapping range (docs/TASKS.md). */
    if (req->offset < 0 || size < (int32_t)DISPLAY_FLUSH_DESC_SIZE(0) ||
        wasmos_xfer_buffer_read(req->buffer_id, &desc, size, req->offset) != 0) {
        status = WASMOS_ERR_DISPLAY_BAD_DESC;
    } else {
        status = vgpu_damage_check(&desc, (uint32_t)size);
    }
    if (status != 0) {
        vgpu_answer(req->client, req->request_id, status);
        return 0;
    }
    vgpu_region_t region =
        vgpu_damage_bounds(desc.rects, desc.count, g_fb.framebuffer_width, g_fb.framebuffer_height);
    return vgpu_present(region, req->client, req->request_id);
}

/* Presents queued requests, oldest first, while the queue has room. */
static void vgpu_serve_pending(void) {
    while (g_pending_count > 0u) {
        if (vgpu_take_request(&g_pending[g_pending_head]) != 0) {
            return;
        }
        g_pending_head = (g_pending_head + 1u) % VGPU_PENDING_MAX;
        g_pending_count--;
    }
}

/* DISPLAY_IPC_FLUSH_REQ: arg0 = buffer_id, arg1 = offset, arg2 = size of a
 * wasmos_display_flush_desc_t the client owns and has lent READ. Requests are
 * presented in arrival order; one that finds the control queue full waits in
 * g_pending. */
static void vgpu_on_flush(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->source < 0) {
        return;
    }
    if (!g_live) {
        vgpu_answer(msg->source, msg->request_id, WASMOS_ERR_DISPLAY_NOT_READY);
        return;
    }
    if (g_pending_count == VGPU_PENDING_MAX) {
        vgpu_answer(msg->source, msg->request_id, WASMOS_ERR_DISPLAY_BUSY);
        return;
    }
    uint32_t seen = 0;
    while (seen < g_clients_seen_count && g_clients_seen[seen] != msg->source) {
        ++seen;
    }
    if (seen == g_clients_seen_count && g_clients_seen_count < VGPU_CLIENTS_LOGGED) {
        g_clients_seen[g_clients_seen_count++] = msg->source;
        (void)printf("[virtio-gpu] presenting for client endpoint=%d\n", (int)msg->source);
    }
    vgpu_pending_t* req = &g_pending[(g_pending_head + g_pending_count) % VGPU_PENDING_MAX];
    req->client = msg->source;
    req->request_id = msg->request_id;
    req->buffer_id = msg->arg0;
    req->offset = msg->arg1;
    req->size = msg->arg2;
    g_pending_count++;
    vgpu_serve_pending();
}

// ----------------------------------------------------------------- interrupt

static void vgpu_on_msi(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
    vgpu_drain_ctrlq();
}

/* A routed INTx line: reading the ISR de-asserts it, and the kernel keeps the
 * line masked until irq_ack, so both are owed on every event. */
static void vgpu_on_intx(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
    if (!g_dev.irq_routed) {
        return;
    }
    (void)virtio_modern_isr(&g_dev.vm);
    (void)wasmos_irq_ack((int32_t)g_dev.irq);
    vgpu_drain_ctrlq();
}

// ------------------------------------------------------------------ bring-up

/* Messages no intent or handler claims are dropped, never answered: an error
 * reply to an unsolicited message can ping-pong with a peer that does the
 * same. A hangup of the FBTEXT server makes its watch report the next bind
 * even on the same endpoint id, so a restarted server is told again. */
static void vgpu_on_unclaimed(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type != WASMOS_IPC_HANGUP || msg->arg0 < 0) {
        return;
    }
    vgpu_forget_client(msg->arg0);
    if (msg->arg0 == g_fb_ep) {
        g_fb_ep = -1;
        wasmos_sys_svc_watch_forget(&g_fb_watch);
    }
}

static void vgpu_on_presenter_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type != FBTEXT_IPC_RESP) {
        (void)printf("[virtio-gpu] the text console refused its presenter (status=%d); "
                     "text shows on the periodic present only\n",
                     (int)msg->arg0);
    }
}

/* Tells the FBTEXT server, on each bind, that this driver presents the scanout. */
static void vgpu_on_fb_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)user;
    (void)watch;
    g_fb_ep = endpoint;
    (void)wasmos_sys_intent_send(&g_loop,
                                 endpoint,
                                 g_ep,
                                 FBTEXT_IPC_PRESENTER_REQ,
                                 0,
                                 0,
                                 0,
                                 0,
                                 vgpu_on_presenter_reply,
                                 0,
                                 0);
}

static void vgpu_on_ignored_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

static void vgpu_on_registered(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (g_register_bid >= 0) {
        (void)wasmos_xfer_buffer_release(g_register_bid);
        g_register_bid = -1;
    }
    if (msg->type != SVC_IPC_REGISTER_RESP) {
        vgpu_fatal("[virtio-gpu] service registration refused\n", WASMOS_ERR_DRIVER_REGISTER);
    }
}

/* Registers "display", then reports READY. The process manager serves one
 * sender in order, so the name is bound by the time anyone sees READY. */
static void vgpu_register(void) {
    svc_register_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.version = WASMOS_SVC_REGISTER_DESC_VERSION;
    desc.service_endpoint = (uint32_t)g_ep;
    memcpy(desc.name, "display", sizeof("display"));
    g_register_bid = wasmos_xfer_stage(&desc, (int32_t)sizeof(desc));
    if (g_register_bid < 0 || wasmos_sys_intent_send(&g_loop,
                                                     g_proc_ep,
                                                     g_ep,
                                                     SVC_IPC_REGISTER_DESC_REQ,
                                                     0,
                                                     (int32_t)sizeof(desc),
                                                     g_register_bid,
                                                     0,
                                                     vgpu_on_registered,
                                                     0,
                                                     0) != 0) {
        vgpu_fatal("[virtio-gpu] cannot register the service\n", WASMOS_ERR_DRIVER_REGISTER);
    }
    (void)wasmos_sys_intent_send(
        &g_loop, g_proc_ep, g_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, vgpu_on_ignored_reply, 0, 0);
}

/* The bring-up commands, each issued from the previous one's completion. */
static void vgpu_scanout_set(void* user, int32_t status) {
    (void)user;
    if (status != 0) {
        vgpu_fatal("[virtio-gpu] SET_SCANOUT failed\n", WASMOS_ERR_DRIVER_DEVICE_INIT);
    }
    g_live = 1u;
    (void)printf("[virtio-gpu] scanout %ux%u on bus=%u slot=%u (%s)\n",
                 (unsigned)g_fb.framebuffer_width,
                 (unsigned)g_fb.framebuffer_height,
                 (unsigned)g_dev.bus,
                 (unsigned)g_dev.slot,
                 g_dev.msix_enabled ? "msi-x" : (g_dev.irq_routed ? "intx" : "polled"));
    /* Everything drawn before now -- the kernel's console, the early log -- is
     * presented once before anyone asks. */
    (void)vgpu_present(vgpu_full_region(), -1, 0);
    vgpu_register();
    memset(&g_fb_watch, 0, sizeof(g_fb_watch));
    if (wasmos_sys_svc_watch_start(&g_watches, &g_fb_watch, "fb", 0, vgpu_on_fb_bound, 0, 0) != 0) {
        vgpu_log("[virtio-gpu] cannot watch the text console; text shows on the periodic "
                 "present only\n");
    }
}

static void vgpu_backing_attached(void* user, int32_t status) {
    (void)user;
    uint32_t s = 0;
    if (status != 0) {
        vgpu_fatal("[virtio-gpu] RESOURCE_ATTACH_BACKING failed\n", WASMOS_ERR_DRIVER_DEVICE_INIT);
    }
    vgpu_set_scanout_t* req = (vgpu_set_scanout_t*)vgpu_slot_request(&s);
    req->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    req->r = vgpu_full_region();
    req->scanout_id = VGPU_SCANOUT_ID;
    req->resource_id = VGPU_RESOURCE_ID;
    (void)vgpu_post(s, (uint32_t)sizeof(*req), vgpu_scanout_set, 0);
    vring_kick(&g_ctrlq);
}

/* The backing is the framebuffer the loader allocated: physically contiguous,
 * so one entry covers it. */
static void vgpu_resource_created(void* user, int32_t status) {
    (void)user;
    uint32_t s = 0;
    if (status != 0) {
        vgpu_fatal("[virtio-gpu] RESOURCE_CREATE_2D failed\n", WASMOS_ERR_DRIVER_DEVICE_INIT);
    }
    vgpu_attach_backing_t* req = (vgpu_attach_backing_t*)vgpu_slot_request(&s);
    req->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    req->resource_id = VGPU_RESOURCE_ID;
    req->nr_entries = 1u;
    req->addr = g_fb.framebuffer_base;
    req->length = g_fb.framebuffer_stride * g_fb.framebuffer_height * 4u;
    (void)vgpu_post(s, (uint32_t)sizeof(*req), vgpu_backing_attached, 0);
    vring_kick(&g_ctrlq);
}

static void vgpu_create_resource(void) {
    uint32_t s = 0;
    vgpu_resource_create_2d_t* req = (vgpu_resource_create_2d_t*)vgpu_slot_request(&s);
    req->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    req->resource_id = VGPU_RESOURCE_ID;
    req->format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    req->width = g_fb.framebuffer_width;
    req->height = g_fb.framebuffer_height;
    (void)vgpu_post(s, (uint32_t)sizeof(*req), vgpu_resource_created, 0);
    vring_kick(&g_ctrlq);
}

/* Configures the control queue (bound to the MSI-X entry when MSI-X is live)
 * and the command page, and sets DRIVER_OK. Returns 0, or -1 with the device
 * FAILED. */
static int vgpu_setup_ctrlq(void) {
    uint8_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
    uint16_t qsize = virtio_modern_queue_size(&g_dev.vm, (uint16_t)VIRTIO_GPU_CONTROL_QUEUE);
    if (qsize == 0u) {
        goto failed;
    }
    if (qsize > VIRTIO_GPU_MAX_QUEUE) {
        qsize = VIRTIO_GPU_MAX_QUEUE;
    }
    uint64_t ring_bytes = vring_size(qsize, VIRTIO_MODERN_VRING_ALIGN);
    int32_t ring_pages = (int32_t)((ring_bytes + 0xFFFu) / 0x1000u);
    uint64_t ring_phys = 0;
    int32_t ring_off = wasmos_region_alloc(ring_pages, WASMOS_REGION_CACHE_WB, &ring_phys);
    int32_t cmd_off = wasmos_region_alloc(1, WASMOS_REGION_CACHE_WB, &g_cmd_phys);
    if (ring_off < 0 || cmd_off < 0 ||
        vring_layout(&g_ctrlq,
                     ptr_cast(uint8_t, (uint32_t)ring_off),
                     ring_phys,
                     (uint64_t)ring_pages * 0x1000u,
                     qsize,
                     VIRTIO_MODERN_VRING_ALIGN) != 0) {
        goto failed;
    }
    vring_set_notify(&g_ctrlq, vgpu_ctrlq_notify, 0);
    /* Polled: no interrupt is bound, so none may be raised; the tick also reads
     * the ISR, since the flag is only a hint (virtio_input.c, same reason). */
    if (!g_dev.msix_enabled && !g_dev.irq_routed) {
        vring_disable_interrupts(&g_ctrlq);
    }
    if (virtio_modern_queue_enable(&g_dev.vm,
                                   (uint16_t)VIRTIO_GPU_CONTROL_QUEUE,
                                   &g_ctrlq,
                                   g_dev.msix_enabled ? (uint16_t)VIRTIO_GPU_MSIX_ENTRY_QUEUE
                                                      : (uint16_t)VIRTIO_MSI_NO_VECTOR,
                                   &g_dev.notify) != 0) {
        goto failed;
    }
    g_cmd = ptr_cast(uint8_t, (uint32_t)cmd_off);
    virtio_modern_set_status(&g_dev.vm, (uint8_t)(status | VIRTIO_STATUS_DRIVER_OK));
    return 0;
failed:
    virtio_modern_set_status(&g_dev.vm, (uint8_t)(status | VIRTIO_STATUS_FAILED));
    return -1;
}

/* The interrupt is settled: bring the queue up and start the bring-up
 * commands. Runs once. */
static void vgpu_finish_bringup(void) {
    if (g_bringup_done) {
        return;
    }
    g_bringup_done = 1;
    if (!g_dev.msix_enabled && g_dev.irq < 16u) {
        if (wasmos_irq_route_ipc((int32_t)g_dev.irq, g_ep) == 0) {
            g_dev.irq_routed = 1;
        } else {
            (void)printf("[virtio-gpu] irq route failed line=%u; polling on the loop tick\n",
                         (unsigned)g_dev.irq);
        }
    }
    if (vgpu_setup_ctrlq() != 0) {
        vgpu_fatal("[virtio-gpu] control queue setup failed\n", WASMOS_ERR_DRIVER_DEVICE_INIT);
    }
    vgpu_create_resource();
}

static void vgpu_on_intx_armed(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type != PCI_IPC_RESP) {
        vgpu_fatal("[virtio-gpu] pci-bus refused intx enable; ending\n",
                   WASMOS_ERR_DRIVER_DEVICE_INIT);
    }
    vgpu_finish_bringup();
}

/* No MSI-X: ask pci-bus to arm the function for INTx (virtio_input.c). */
static void vgpu_fall_back_to_intx(void) {
    if (g_pci_ep < 0) {
        vgpu_finish_bringup();
        return;
    }
    if (wasmos_sys_intent_send(&g_loop,
                               g_pci_ep,
                               g_ep,
                               PCI_IPC_INTX_ENABLE_REQ,
                               (int32_t)vgpu_bdf(),
                               0,
                               0,
                               0,
                               vgpu_on_intx_armed,
                               0,
                               0) != 0) {
        vgpu_fatal("[virtio-gpu] intx enable request could not be sent\n",
                   WASMOS_ERR_DRIVER_DEVICE_INIT);
    }
}

static void vgpu_on_msi_bound(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type != PCI_IPC_RESP) {
        (void)wasmos_msi_free((int32_t)g_dev.msix_vector);
        g_dev.msix_vector = 0;
        vgpu_fall_back_to_intx();
        return;
    }
    g_dev.msix_enabled = 1u;
    vgpu_finish_bringup();
}

/* pci-bus answered PCI_IPC_MSI_QUERY: with MSI-X on offer, take a kernel vector
 * bound to the service endpoint and have pci-bus program it into entry 0. */
static void vgpu_on_msi_query(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    wasmos_msi_desc_t desc;
    if (msg->type != PCI_IPC_RESP || msg->arg0 != WASMOS_PCI_MSI_KIND_MSIX || msg->arg1 < 1 ||
        wasmos_msi_alloc(g_ep, &desc) != 0) {
        vgpu_fall_back_to_intx();
        return;
    }
    g_dev.msix_vector = desc.vector;
    int32_t entry = (int32_t)((vgpu_bdf() << 8) | VIRTIO_GPU_MSIX_ENTRY_QUEUE);
    if (wasmos_sys_intent_send(&g_loop,
                               g_pci_ep,
                               g_ep,
                               PCI_IPC_MSI_BIND,
                               entry,
                               (int32_t)desc.address_lo,
                               (int32_t)desc.address_hi,
                               (int32_t)desc.data,
                               vgpu_on_msi_bound,
                               0,
                               0) != 0) {
        (void)wasmos_msi_free((int32_t)desc.vector);
        g_dev.msix_vector = 0;
        vgpu_fall_back_to_intx();
    }
}

static void vgpu_on_pci_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)user;
    (void)watch;
    /* Only the first bind matters: the interrupt is decided once, at bring-up. */
    if (g_bringup_done || g_pci_ep >= 0) {
        return;
    }
    g_pci_ep = endpoint;
    if (wasmos_sys_intent_send(&g_loop,
                               g_pci_ep,
                               g_ep,
                               PCI_IPC_MSI_QUERY,
                               (int32_t)vgpu_bdf(),
                               0,
                               0,
                               0,
                               vgpu_on_msi_query,
                               0,
                               0) != 0) {
        vgpu_fall_back_to_intx();
    }
}

static void vgpu_on_pci_failed(void* user, wasmos_sys_svc_watch_t* watch, int32_t status) {
    (void)user;
    (void)watch;
    (void)printf("[virtio-gpu] pci service unavailable (status=%d); msi-x disabled\n", (int)status);
    vgpu_fall_back_to_intx();
}

/* The loop's tick: expires the pci-bus watch's deadline, completes commands
 * when no interrupt reaches this driver, and presents the whole scanout when
 * nothing is in flight, for what no writer reported. */
static void vgpu_on_tick(void* user) {
    (void)user;
    wasmos_sys_svc_watch_set_expire(&g_watches);
    if (!g_bringup_done) {
        return;
    }
    if (!g_dev.msix_enabled && !g_dev.irq_routed) {
        (void)virtio_modern_isr(&g_dev.vm);
        vgpu_drain_ctrlq();
    }
    if (g_live && g_pending_count == 0u && vgpu_free_slots() == VGPU_SLOTS) {
        (void)vgpu_present(vgpu_full_region(), -1, 0);
    }
}

/* Driver entry point. Returns only on a bring-up failure, with a packed
 * WASMOS_ERR_DRIVER_* code; once the loop runs, a fatal failure exits from
 * vgpu_fatal with the same kind of code. */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    g_proc_ep = wasmos_startup_proc_endpoint();
    if (g_proc_ep < 0) {
        return WASMOS_ERR_DRIVER_NO_PROC_ENDPOINT;
    }
    if (probe_from_startup_args() != 0) {
        vgpu_log("[virtio-gpu] startup args name no virtio-gpu function\n");
        return WASMOS_ERR_DRIVER_NO_DEVICE_IDENTITY;
    }
    /* The scanout must be the loader's linear BGRX framebuffer with rows of
     * exactly `width` pixels: the resource's rows are width x 4 bytes, and
     * TRANSFER_TO_HOST_2D reads the backing with that pitch. */
    if (wasmos_framebuffer_info(&g_fb, (int32_t)sizeof(g_fb)) != 0 || g_fb.framebuffer_base == 0u ||
        g_fb.framebuffer_width == 0u || g_fb.framebuffer_height == 0u ||
        g_fb.framebuffer_stride != g_fb.framebuffer_width) {
        vgpu_log("[virtio-gpu] no linear framebuffer to present\n");
        return WASMOS_ERR_DRIVER_DEVICE_UNSUPPORTED;
    }
    if (virtio_modern_probe(&g_dev.vm, vgpu_bdf(), VIRTIO_GPU_BAR, VIRTIO_GPU_REGION) != 0) {
        vgpu_log("[virtio-gpu] no virtio-modern structures in BAR 4\n");
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    if (virtio_modern_negotiate(&g_dev.vm, 0u, 0, 0) != 0) {
        vgpu_log("[virtio-gpu] feature negotiation failed\n");
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    g_ep = wasmos_ipc_create_endpoint();
    if (g_ep < 0) {
        return WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    }
    wasmos_sys_event_loop_init(&g_loop, g_ep, VGPU_LOOP_REQ_BASE);
    if (g_loop.select_id < 0) {
        return WASMOS_ERR_DRIVER_SELECT_SETUP;
    }
    g_loop.poll_timeout_ms = VGPU_TICK_MS;
    g_loop.on_timeout = vgpu_on_tick;
    (void)wasmos_sys_event_set_default(&g_loop, vgpu_on_unclaimed, 0);
    if (wasmos_sys_event_register(&g_loop, DISPLAY_IPC_FLUSH_REQ, vgpu_on_flush, 0) != 0 ||
        wasmos_sys_event_register(&g_loop, WASMOS_IPC_MSI_EVENT_TYPE, vgpu_on_msi, 0) != 0 ||
        wasmos_sys_event_register(&g_loop, VGPU_IRQ_EVENT_TYPE, vgpu_on_intx, 0) != 0 ||
        wasmos_sys_svc_watch_set_init(&g_watches, &g_loop, g_proc_ep) != 0) {
        return WASMOS_ERR_DRIVER_EVENT_LOOP;
    }
    memset(&g_pci_watch, 0, sizeof(g_pci_watch));
    if (wasmos_sys_svc_watch_start(&g_watches,
                                   &g_pci_watch,
                                   "pci",
                                   VGPU_PCI_WAIT_MS,
                                   vgpu_on_pci_bound,
                                   vgpu_on_pci_failed,
                                   0) != 0) {
        vgpu_fall_back_to_intx();
    }

    for (;;) {
        (void)wasmos_sys_event_loop_poll(&g_loop, 16);
        wasmos_sys_svc_watch_set_expire(&g_watches);
        /* Polled: completions are picked up here, after every poll, and not
         * only on the idle tick -- steady traffic would otherwise keep the
         * tick from ever running and hold every slot. */
        if (g_bringup_done && !g_dev.msix_enabled && !g_dev.irq_routed) {
            (void)virtio_modern_isr(&g_dev.vm);
            vgpu_drain_ctrlq();
        }
        vgpu_retry_answers();
    }
    return 0;
}
