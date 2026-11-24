/* virtio_input.c - virtio-input driver (virtio 1.2 §5.8): keyboard and mouse.
 *
 * One driver instance serves one virtio-input function and takes the role the
 * device's event bitmaps describe:
 *
 *   - a device that reports relative axes (EV_REL) is a MOUSE. The instance
 *     registers "mouse" and speaks the PS/2 mouse driver's protocol: each
 *     SYN_REPORT becomes MOUSE_IPC_MOVE_NOTIFY (dx, dy, buttons), split into
 *     steps of at most 127 per axis because the notify carries signed 8-bit
 *     deltas. evdev's REL_Y grows downwards, which is the protocol's direction.
 *   - a device that reports keys (EV_KEY) and no relative axes is a KEYBOARD.
 *     The instance registers "kbd" and speaks the PS/2 keyboard driver's
 *     protocol: each key event becomes KBD_IPC_KEY_NOTIFY carrying the PS/2
 *     Set 1 scancode of the evdev key code (k_set1 below). Clients -- the vt,
 *     and through it the compositor -- decode Set 1, so a virtio keyboard and
 *     a PS/2 one are indistinguishable to them.
 *   - a device with absolute axes and no relative ones (virtio-tablet) is
 *     refused with WASMOS_ERR_DRIVER_DEVICE_UNSUPPORTED.
 *     TODO: a tablet needs absolute pointer positions, which the mouse
 *     protocol cannot carry; until it can, QEMU's virtio-tablet-pci gives no
 *     pointer.
 *
 * Transport: virtio 1.x PCI (virtio_modern.h), registers in BAR 4, granted as
 * region 0. Only the eventq is driven; the statusq (LED state, driver to
 * device) is left unconfigured, which the device permits.
 *
 * Every exchange with another process is a loop intent, never a synchronous
 * call (AGENTS.md): pci-bus is found through a service watch, MSI-X is queried
 * and bound through intents, and the device goes DRIVER_OK, the service
 * registers and the process manager is told READY only once that chain has
 * settled. Device interrupts arrive on the same endpoint: an MSI-X vector as
 * WASMOS_IPC_MSI_EVENT_TYPE (no ack owed), a routed INTx line as
 * IPC_IRQ_EVENT_TYPE (ISR read plus irq_ack owed). */
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

/* PCI identity: 0x1AF4 is the virtio vendor id, 0x1052 the modern-only
 * virtio-input function (0x1040 + device type 18). There is no transitional
 * virtio-input device. */
#define VIRTIO_PCI_VENDOR_ID 0x1AF4u
#define VIRTIO_INPUT_DEV_MODERN 0x1052u

#define VIRTIO_INPUT_BAR 4u
#define VIRTIO_INPUT_REGION 0

/* eventq: device-to-driver input events; statusq (1) is not driven. */
#define VIRTIO_INPUT_EVENT_QUEUE 0u
#define VIRTIO_INPUT_MAX_QUEUE 64u
#define VIRTIO_INPUT_MSIX_ENTRY_QUEUE 0u

/* virtio_input_config (virtio 1.2 §5.8.4): the driver writes select/subsel and
 * reads size and the union. */
#define VIRTIO_INPUT_CFG_SELECT 0u
#define VIRTIO_INPUT_CFG_SUBSEL 1u
#define VIRTIO_INPUT_CFG_SIZE 2u
#define VIRTIO_INPUT_CFG_UNION 8u
#define VIRTIO_INPUT_CFG_EV_BITS 0x11u

/* evdev event types and codes (Linux input-event-codes.h, which virtio-input
 * adopts verbatim). */
#define EV_SYN 0x00u
#define EV_KEY 0x01u
#define EV_REL 0x02u
#define EV_ABS 0x03u
#define SYN_REPORT 0u
#define REL_X 0x00u
#define REL_Y 0x01u
#define BTN_LEFT 0x110u
#define BTN_RIGHT 0x111u
#define BTN_MIDDLE 0x112u

/* Interrupt delivery on the service endpoint. A routed INTx line arrives as
 * IPC_IRQ_EVENT_TYPE and owes an ack; MSI-X as WASMOS_IPC_MSI_EVENT_TYPE. Both
 * carry the interrupt id as their request_id, so the loop's own request ids
 * start far above any interrupt id (VIN_LOOP_REQ_BASE). */
#define VIN_IRQ_EVENT_TYPE 0xFF00
#define VIN_LOOP_REQ_BASE 0x7A0000

/* How long bring-up waits for pci-bus before settling for INTx without asking
 * it to arm the function. */
#define VIN_PCI_WAIT_MS 2000
#define VIN_LOOP_TICK_MS 250

#define MAX_SUBSCRIBERS 4
/* The mouse notify carries a signed 8-bit delta per axis. */
#define MOUSE_STEP_MAX 127

/* One evdev event as the device writes it into an eventq buffer. */
typedef struct {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} virtio_input_event_t;
_Static_assert(sizeof(virtio_input_event_t) == 8, "virtio_input_event is 8 bytes");

typedef enum { VIN_ROLE_KEYBOARD = 1, VIN_ROLE_MOUSE = 2 } vin_role_t;

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t irq;
    uint16_t device_id;
    uint8_t msix_enabled;
    uint8_t irq_routed;
    uint32_t msix_vector;
    virtio_modern_t vm;
    uint32_t notify;
} vin_device_t;

static int32_t g_proc_ep = -1;
static int32_t g_ep = -1;
static int32_t g_pci_ep = -1;
static wasmos_sys_event_loop_t g_loop;
static wasmos_sys_svc_watch_set_t g_watches;
static wasmos_sys_svc_watch_t g_pci_watch;
static int32_t g_register_bid = -1;
static uint8_t g_bringup_done;
static vin_device_t g_dev;
static vin_role_t g_role;
static int32_t g_subscribers[MAX_SUBSCRIBERS];

static vring_t g_eventq;
static uint16_t g_eventq_size;
static virtio_input_event_t* g_events; /* g_eventq_size buffers, DMA-visible */
static uint64_t g_events_phys;
static uint16_t g_desc_buf[VIRTIO_INPUT_MAX_QUEUE]; /* descriptor -> buffer */

/* Mouse state accumulated between SYN_REPORTs. */
static int32_t g_mouse_dx;
static int32_t g_mouse_dy;
static int32_t g_mouse_buttons;
static uint8_t g_mouse_dirty;

/* evdev key code -> PS/2 Set 1 make code, bit 8 set for an 0xE0-prefixed
 * (extended) key, 0 for a key Set 1 cannot express. Codes 1..88 ARE their
 * Set 1 codes (evdev's numbering started from the AT keyboard); the extended
 * keys and the gaps below are not. Release is reported separately, so no
 * entry carries the 0x80 break bit. */
#define E0(code) (0x100u | (code))
static const uint16_t k_set1[128] = {
    [1] = 0x01,       [2] = 0x02,  [3] = 0x03,      [4] = 0x04,  [5] = 0x05,  [6] = 0x06,
    [7] = 0x07,       [8] = 0x08,  [9] = 0x09,      [10] = 0x0A, [11] = 0x0B, [12] = 0x0C,
    [13] = 0x0D,      [14] = 0x0E, [15] = 0x0F,     [16] = 0x10, [17] = 0x11, [18] = 0x12,
    [19] = 0x13,      [20] = 0x14, [21] = 0x15,     [22] = 0x16, [23] = 0x17, [24] = 0x18,
    [25] = 0x19,      [26] = 0x1A, [27] = 0x1B,     [28] = 0x1C, [29] = 0x1D, [30] = 0x1E,
    [31] = 0x1F,      [32] = 0x20, [33] = 0x21,     [34] = 0x22, [35] = 0x23, [36] = 0x24,
    [37] = 0x25,      [38] = 0x26, [39] = 0x27,     [40] = 0x28, [41] = 0x29, [42] = 0x2A,
    [43] = 0x2B,      [44] = 0x2C, [45] = 0x2D,     [46] = 0x2E, [47] = 0x2F, [48] = 0x30,
    [49] = 0x31,      [50] = 0x32, [51] = 0x33,     [52] = 0x34, [53] = 0x35, [54] = 0x36,
    [55] = 0x37,      [56] = 0x38, [57] = 0x39,     [58] = 0x3A, [59] = 0x3B, [60] = 0x3C,
    [61] = 0x3D,      [62] = 0x3E, [63] = 0x3F,     [64] = 0x40, [65] = 0x41, [66] = 0x42,
    [67] = 0x43,      [68] = 0x44, [69] = 0x45,     [70] = 0x46, [71] = 0x47, [72] = 0x48,
    [73] = 0x49,      [74] = 0x4A, [75] = 0x4B,     [76] = 0x4C, [77] = 0x4D, [78] = 0x4E,
    [79] = 0x4F,      [80] = 0x50, [81] = 0x51,     [82] = 0x52, [83] = 0x53, [86] = 0x56,
    [87] = 0x57,      [88] = 0x58, [96] = E0(0x1C), /* KEY_KPENTER */
    [97] = E0(0x1D),                                /* KEY_RIGHTCTRL */
    [98] = E0(0x35),                                /* KEY_KPSLASH */
    [99] = E0(0x37),                                /* KEY_SYSRQ */
    [100] = E0(0x38),                               /* KEY_RIGHTALT (AltGr) */
    [102] = E0(0x47),                               /* KEY_HOME */
    [103] = E0(0x48),                               /* KEY_UP */
    [104] = E0(0x49),                               /* KEY_PAGEUP */
    [105] = E0(0x4B),                               /* KEY_LEFT */
    [106] = E0(0x4D),                               /* KEY_RIGHT */
    [107] = E0(0x4F),                               /* KEY_END */
    [108] = E0(0x50),                               /* KEY_DOWN */
    [109] = E0(0x51),                               /* KEY_PAGEDOWN */
    [110] = E0(0x52),                               /* KEY_INSERT */
    [111] = E0(0x53),                               /* KEY_DELETE */
    [125] = E0(0x5B),                               /* KEY_LEFTMETA */
    [126] = E0(0x5C),                               /* KEY_RIGHTMETA */
    [127] = E0(0x5D),                               /* KEY_COMPOSE (menu) */
};
#undef E0

static void vin_log(const char* msg) {
    (void)printf("%s", msg);
}

static uint32_t vin_bdf(void) {
    return ((uint32_t)g_dev.bus << 8) | ((uint32_t)g_dev.slot << 3) | (uint32_t)g_dev.function;
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
    if (vendor_id != VIRTIO_PCI_VENDOR_ID || device_id != VIRTIO_INPUT_DEV_MODERN) {
        return -1;
    }
    g_dev.bus = (uint8_t)bus;
    g_dev.slot = (uint8_t)slot;
    g_dev.function = (uint8_t)function;
    g_dev.irq = (uint8_t)irq;
    g_dev.device_id = (uint16_t)device_id;
    return 0;
}

/* Whether the device reports any event of `ev_type`: the EV_BITS selection's
 * size is the length of the code bitmap, zero when the type is absent. */
static int vin_has_event_type(uint8_t ev_type) {
    if (virtio_modern_device_write8(&g_dev.vm, VIRTIO_INPUT_CFG_SELECT, VIRTIO_INPUT_CFG_EV_BITS) !=
            0 ||
        virtio_modern_device_write8(&g_dev.vm, VIRTIO_INPUT_CFG_SUBSEL, ev_type) != 0) {
        return 0;
    }
    uint8_t size = 0;
    if (virtio_modern_device_read(&g_dev.vm, VIRTIO_INPUT_CFG_SIZE, &size, 1u) != 0) {
        return 0;
    }
    return size != 0u;
}

/* Locates the structures, negotiates VERSION_1 only, and decides the role.
 * Leaves the device at FEATURES_OK: queues and DRIVER_OK wait for the MSI-X
 * decision (vin_finish_bringup), since a queue is bound to its vector when it
 * is enabled. Returns 0 or a packed WASMOS_ERR_DRIVER_* code. */
static int32_t vin_probe_device(void) {
    if (virtio_modern_probe(&g_dev.vm, vin_bdf(), VIRTIO_INPUT_BAR, VIRTIO_INPUT_REGION) != 0) {
        vin_log("[virtio-input] no virtio-modern structures in BAR 4\n");
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    if (virtio_modern_negotiate(&g_dev.vm, 0u, 0, 0) != 0) {
        vin_log("[virtio-input] feature negotiation failed\n");
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    if (vin_has_event_type(EV_REL)) {
        g_role = VIN_ROLE_MOUSE;
    } else if (vin_has_event_type(EV_KEY) && !vin_has_event_type(EV_ABS)) {
        g_role = VIN_ROLE_KEYBOARD;
    } else {
        vin_log("[virtio-input] device reports neither relative axes nor keys only; "
                "not driven\n");
        virtio_modern_set_status(&g_dev.vm,
                                 (uint8_t)(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                           VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_FAILED));
        return WASMOS_ERR_DRIVER_DEVICE_UNSUPPORTED;
    }
    return 0;
}

static void vin_eventq_notify(void* user) {
    (void)user;
    virtio_modern_notify(&g_dev.vm, g_dev.notify, (uint16_t)VIRTIO_INPUT_EVENT_QUEUE);
}

/* Hands buffer `b` to the device. The descriptor table has one slot per
 * buffer, so a descriptor is always free for a buffer coming back. */
static void vin_post_buffer(uint16_t b) {
    int32_t d = vring_alloc_desc(&g_eventq,
                                 g_events_phys + (uint64_t)b * sizeof(virtio_input_event_t),
                                 (uint32_t)sizeof(virtio_input_event_t),
                                 VRING_DESC_F_WRITE);
    if (d < 0) {
        return;
    }
    g_desc_buf[d] = b;
    vring_publish(&g_eventq, (uint16_t)d);
}

/* Configures the eventq (bound to the MSI-X entry when MSI-X is live), posts
 * every buffer, and sets DRIVER_OK. Returns 0, or -1 with the device FAILED. */
static int vin_setup_eventq(void) {
    uint8_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
    uint16_t qsize = virtio_modern_queue_size(&g_dev.vm, (uint16_t)VIRTIO_INPUT_EVENT_QUEUE);
    if (qsize == 0u) {
        goto failed;
    }
    if (qsize > VIRTIO_INPUT_MAX_QUEUE) {
        qsize = VIRTIO_INPUT_MAX_QUEUE;
    }
    uint64_t ring_bytes = vring_size(qsize, VIRTIO_MODERN_VRING_ALIGN);
    int32_t ring_pages = (int32_t)((ring_bytes + 0xFFFu) / 0x1000u);
    uint64_t ring_phys = 0;
    int32_t ring_off = wasmos_region_alloc(ring_pages, WASMOS_REGION_CACHE_WB, &ring_phys);
    int32_t pool_off = wasmos_region_alloc(1, WASMOS_REGION_CACHE_WB, &g_events_phys);
    if (ring_off < 0 || pool_off < 0 ||
        vring_layout(&g_eventq,
                     ptr_cast(uint8_t, (uint32_t)ring_off),
                     ring_phys,
                     (uint64_t)ring_pages * 0x1000u,
                     qsize,
                     VIRTIO_MODERN_VRING_ALIGN) != 0) {
        goto failed;
    }
    vring_set_notify(&g_eventq, vin_eventq_notify, 0);
    if (virtio_modern_queue_enable(&g_dev.vm,
                                   (uint16_t)VIRTIO_INPUT_EVENT_QUEUE,
                                   &g_eventq,
                                   g_dev.msix_enabled ? (uint16_t)VIRTIO_INPUT_MSIX_ENTRY_QUEUE
                                                      : (uint16_t)VIRTIO_MSI_NO_VECTOR,
                                   &g_dev.notify) != 0) {
        goto failed;
    }
    g_events = ptr_cast(virtio_input_event_t, (uint32_t)pool_off);
    g_eventq_size = qsize;
    for (uint16_t b = 0; b < qsize; ++b) {
        vin_post_buffer(b);
    }
    virtio_modern_set_status(&g_dev.vm, (uint8_t)(status | VIRTIO_STATUS_DRIVER_OK));
    vring_kick(&g_eventq);
    return 0;
failed:
    virtio_modern_set_status(&g_dev.vm, (uint8_t)(status | VIRTIO_STATUS_FAILED));
    return -1;
}

// ------------------------------------------------------------------ clients

static int32_t vin_add_subscriber(int32_t ep) {
    for (int i = 0; i < MAX_SUBSCRIBERS; ++i) {
        if (g_subscribers[i] == ep) {
            return 0;
        }
    }
    for (int i = 0; i < MAX_SUBSCRIBERS; ++i) {
        if (g_subscribers[i] < 0) {
            g_subscribers[i] = ep;
            return 0;
        }
    }
    return -1; /* full: refused, as the PS/2 drivers refuse it */
}

static void vin_notify_all(int32_t type, int32_t a0, int32_t a1, int32_t a2) {
    for (int i = 0; i < MAX_SUBSCRIBERS; ++i) {
        if (g_subscribers[i] >= 0) {
            (void)wasmos_ipc_send(g_subscribers[i], g_ep, type, 0, a0, a1, a2, 0);
        }
    }
}

/* KBD_IPC_SUBSCRIBE_REQ / MOUSE_IPC_SUBSCRIBE_REQ: arg0 of the reply is 0 when
 * the subscriber was added (or already was), non-zero when every slot is
 * taken. Only the request matching this instance's role is served. */
static void vin_on_subscribe(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->source < 0) {
        return;
    }
    int32_t resp =
        (g_role == VIN_ROLE_KEYBOARD) ? KBD_IPC_SUBSCRIBE_RESP : MOUSE_IPC_SUBSCRIBE_RESP;
    int32_t want = (g_role == VIN_ROLE_KEYBOARD) ? KBD_IPC_SUBSCRIBE_REQ : MOUSE_IPC_SUBSCRIBE_REQ;
    if (msg->type != want) {
        return;
    }
    (void)wasmos_ipc_send(
        msg->source, g_ep, resp, msg->request_id, vin_add_subscriber(msg->source), 0, 0, 0);
}

// ------------------------------------------------------------------- events

static void vin_key_event(uint16_t code, uint32_t value) {
    if (code >= sizeof(k_set1) / sizeof(k_set1[0]) || k_set1[code] == 0u) {
        return;
    }
    /* value: 0 release, 1 press, 2 autorepeat. A repeat is another make code,
     * as a PS/2 keyboard's typematic repeat is. */
    int32_t keyup = (value == 0u) ? 1 : 0;
    int32_t extended = (k_set1[code] & 0x100u) ? 1 : 0;
    vin_notify_all(KBD_IPC_KEY_NOTIFY, (int32_t)(k_set1[code] & 0x7Fu), keyup, extended);
}

static int32_t vin_clamp_step(int32_t v) {
    if (v > MOUSE_STEP_MAX) {
        return MOUSE_STEP_MAX;
    }
    if (v < -MOUSE_STEP_MAX) {
        return -MOUSE_STEP_MAX;
    }
    return v;
}

/* Sends the motion and button state accumulated since the last SYN_REPORT, in
 * as many notifies as the 8-bit deltas need; the last carries the buttons
 * alone when there was no motion. */
static void vin_mouse_flush(void) {
    if (!g_mouse_dirty) {
        return;
    }
    do {
        int32_t sx = vin_clamp_step(g_mouse_dx);
        int32_t sy = vin_clamp_step(g_mouse_dy);
        g_mouse_dx -= sx;
        g_mouse_dy -= sy;
        vin_notify_all(MOUSE_IPC_MOVE_NOTIFY, sx & 0xFF, sy & 0xFF, g_mouse_buttons);
    } while (g_mouse_dx != 0 || g_mouse_dy != 0);
    g_mouse_dirty = 0;
}

static void vin_mouse_event(const virtio_input_event_t* ev) {
    if (ev->type == EV_REL) {
        if (ev->code == REL_X) {
            g_mouse_dx += (int32_t)ev->value;
            g_mouse_dirty = 1;
        } else if (ev->code == REL_Y) {
            g_mouse_dy += (int32_t)ev->value;
            g_mouse_dirty = 1;
        }
    } else if (ev->type == EV_KEY) {
        int32_t bit = (ev->code == BTN_LEFT)     ? 0x1
                      : (ev->code == BTN_RIGHT)  ? 0x2
                      : (ev->code == BTN_MIDDLE) ? 0x4
                                                 : 0;
        if (bit != 0) {
            g_mouse_buttons = ev->value ? (g_mouse_buttons | bit) : (g_mouse_buttons & ~bit);
            g_mouse_dirty = 1;
        }
    } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        vin_mouse_flush();
    }
}

/* Consumes every event the device has written and gives each buffer back. */
static void vin_drain_eventq(void) {
    uint32_t len = 0;
    int32_t id;
    int reposted = 0;
    if (g_eventq_size == 0u) {
        return;
    }
    while ((id = vring_get_used(&g_eventq, &len)) >= 0) {
        uint16_t b = ((uint32_t)id < VIRTIO_INPUT_MAX_QUEUE) ? g_desc_buf[id] : 0xFFFFu;
        vring_free_desc(&g_eventq, (uint16_t)id);
        if (b >= g_eventq_size) {
            continue;
        }
        if (len >= sizeof(virtio_input_event_t)) {
            virtio_input_event_t ev = g_events[b];
            if (g_role == VIN_ROLE_KEYBOARD) {
                if (ev.type == EV_KEY) {
                    vin_key_event(ev.code, ev.value);
                }
            } else {
                vin_mouse_event(&ev);
            }
        }
        vin_post_buffer(b);
        reposted = 1;
    }
    if (reposted) {
        vring_kick(&g_eventq);
    }
}

static void vin_on_msi(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
    vin_drain_eventq();
}

/* A routed INTx line: reading the ISR de-asserts it, and the kernel keeps the
 * line masked until irq_ack, so both are owed on every event -- including one
 * that arrives before the queue exists. */
static void vin_on_intx(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
    if (!g_dev.irq_routed) {
        return;
    }
    (void)virtio_modern_isr(&g_dev.vm);
    (void)wasmos_irq_ack((int32_t)g_dev.irq);
    vin_drain_eventq();
}

// ----------------------------------------------------------------- bring-up

/* Messages no intent or handler claims are dropped, never answered: an error
 * reply to an unsolicited message can ping-pong with a peer that does the
 * same. */
static void vin_on_unclaimed(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

static void vin_on_ignored_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

static void vin_fatal(const char* msg, int32_t status) {
    vin_log(msg);
    (void)virtio_modern_reset(&g_dev.vm);
    (void)wasmos_proc_exit(status);
    __builtin_trap();
}

static void vin_on_registered(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (g_register_bid >= 0) {
        (void)wasmos_xfer_buffer_release(g_register_bid);
        g_register_bid = -1;
    }
    if (msg->type != SVC_IPC_REGISTER_RESP) {
        vin_fatal("[virtio-input] service registration refused\n", WASMOS_ERR_DRIVER_REGISTER);
    }
}

/* Registers "kbd" or "mouse", then reports READY. The process manager serves
 * one sender in order, so the name is bound by the time anyone sees READY. */
static void vin_register(void) {
    svc_register_desc_t desc;
    const char* name = (g_role == VIN_ROLE_KEYBOARD) ? "kbd" : "mouse";
    memset(&desc, 0, sizeof(desc));
    desc.version = WASMOS_SVC_REGISTER_DESC_VERSION;
    desc.service_endpoint = (uint32_t)g_ep;
    memcpy(desc.name, name, strlen(name) + 1u);
    g_register_bid = wasmos_xfer_stage(&desc, (int32_t)sizeof(desc));
    if (g_register_bid < 0 || wasmos_sys_intent_send(&g_loop,
                                                     g_proc_ep,
                                                     g_ep,
                                                     SVC_IPC_REGISTER_DESC_REQ,
                                                     0,
                                                     (int32_t)sizeof(desc),
                                                     g_register_bid,
                                                     0,
                                                     vin_on_registered,
                                                     0,
                                                     0) != 0) {
        vin_fatal("[virtio-input] cannot register the service\n", WASMOS_ERR_DRIVER_REGISTER);
    }
    (void)wasmos_sys_intent_send(
        &g_loop, g_proc_ep, g_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, vin_on_ignored_reply, 0, 0);
}

/* The interrupt is settled (MSI-X bound, or INTx armed and routed, or neither
 * and events are picked up on the loop's tick): bring the queue up and go
 * live. Runs once. */
static void vin_finish_bringup(void) {
    if (g_bringup_done) {
        return;
    }
    g_bringup_done = 1;
    if (!g_dev.msix_enabled && g_dev.irq < 16u) {
        if (wasmos_irq_route_ipc((int32_t)g_dev.irq, g_ep) == 0) {
            g_dev.irq_routed = 1;
        } else {
            (void)printf("[virtio-input] irq route failed line=%u; polling on the loop tick\n",
                         (unsigned)g_dev.irq);
        }
    }
    if (vin_setup_eventq() != 0) {
        vin_fatal("[virtio-input] eventq setup failed\n", WASMOS_ERR_DRIVER_DEVICE_INIT);
    }
    (void)printf("[virtio-input] %s on bus=%u slot=%u (%s)\n",
                 g_role == VIN_ROLE_KEYBOARD ? "keyboard" : "mouse",
                 (unsigned)g_dev.bus,
                 (unsigned)g_dev.slot,
                 g_dev.msix_enabled ? "msi-x" : (g_dev.irq_routed ? "intx" : "polled"));
    vin_register();
}

static void vin_on_intx_armed(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type != PCI_IPC_RESP) {
        vin_fatal("[virtio-input] pci-bus refused intx enable; ending\n",
                  WASMOS_ERR_DRIVER_DEVICE_INIT);
    }
    vin_finish_bringup();
}

/* No MSI-X: ask pci-bus to arm the function for INTx. The kernel quiesces a
 * function when its driver ends, and only an MSI bind or this request undoes
 * that, so the queue waits for the answer. Without pci-bus there is nobody to
 * ask and the function keeps the firmware's state. */
static void vin_fall_back_to_intx(void) {
    if (g_pci_ep < 0) {
        vin_finish_bringup();
        return;
    }
    if (wasmos_sys_intent_send(&g_loop,
                               g_pci_ep,
                               g_ep,
                               PCI_IPC_INTX_ENABLE_REQ,
                               (int32_t)vin_bdf(),
                               0,
                               0,
                               0,
                               vin_on_intx_armed,
                               0,
                               0) != 0) {
        vin_fatal("[virtio-input] intx enable request could not be sent\n",
                  WASMOS_ERR_DRIVER_DEVICE_INIT);
    }
}

static void vin_on_msi_bound(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type != PCI_IPC_RESP) {
        (void)wasmos_msi_free((int32_t)g_dev.msix_vector);
        g_dev.msix_vector = 0;
        vin_fall_back_to_intx();
        return;
    }
    g_dev.msix_enabled = 1u;
    vin_finish_bringup();
}

/* pci-bus answered PCI_IPC_MSI_QUERY: with MSI-X on offer, take a kernel vector
 * bound to the service endpoint and have pci-bus program it into table entry
 * 0. The kernel owns the vector namespace, pci-bus owns config space; this
 * driver only carries the address/data pair between them. */
static void vin_on_msi_query(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    wasmos_msi_desc_t desc;
    if (msg->type != PCI_IPC_RESP || msg->arg0 != WASMOS_PCI_MSI_KIND_MSIX || msg->arg1 < 1 ||
        wasmos_msi_alloc(g_ep, &desc) != 0) {
        vin_fall_back_to_intx();
        return;
    }
    g_dev.msix_vector = desc.vector;
    int32_t entry = (int32_t)((vin_bdf() << 8) | VIRTIO_INPUT_MSIX_ENTRY_QUEUE);
    if (wasmos_sys_intent_send(&g_loop,
                               g_pci_ep,
                               g_ep,
                               PCI_IPC_MSI_BIND,
                               entry,
                               (int32_t)desc.address_lo,
                               (int32_t)desc.address_hi,
                               (int32_t)desc.data,
                               vin_on_msi_bound,
                               0,
                               0) != 0) {
        (void)wasmos_msi_free((int32_t)desc.vector);
        g_dev.msix_vector = 0;
        vin_fall_back_to_intx();
    }
}

static void vin_on_pci_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)user;
    (void)watch;
    /* Only the first bind matters: the interrupt is decided once, at bring-up.
     * A later pci-bus restart rebinds the name, and is ignored. */
    if (g_bringup_done || g_pci_ep >= 0) {
        return;
    }
    g_pci_ep = endpoint;
    if (wasmos_sys_intent_send(&g_loop,
                               g_pci_ep,
                               g_ep,
                               PCI_IPC_MSI_QUERY,
                               (int32_t)vin_bdf(),
                               0,
                               0,
                               0,
                               vin_on_msi_query,
                               0,
                               0) != 0) {
        vin_fall_back_to_intx();
    }
}

static void vin_on_pci_failed(void* user, wasmos_sys_svc_watch_t* watch, int32_t status) {
    (void)user;
    (void)watch;
    (void)printf("[virtio-input] pci service unavailable (status=%d); msi-x disabled\n",
                 (int)status);
    vin_fall_back_to_intx();
}

/* The loop's tick: expires the pci-bus watch's deadline, and picks up events
 * when no interrupt reaches this driver. */
static void vin_on_tick(void* user) {
    (void)user;
    wasmos_sys_svc_watch_set_expire(&g_watches);
    if (g_bringup_done && !g_dev.msix_enabled && !g_dev.irq_routed) {
        vin_drain_eventq();
    }
}

/* Driver entry point. Returns only on a bring-up failure, with a packed
 * WASMOS_ERR_DRIVER_* code; once the loop runs, a fatal failure exits from
 * vin_fatal with the same kind of code. */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    g_proc_ep = wasmos_startup_proc_endpoint();
    if (g_proc_ep < 0) {
        return WASMOS_ERR_DRIVER_NO_PROC_ENDPOINT;
    }
    for (int i = 0; i < MAX_SUBSCRIBERS; ++i) {
        g_subscribers[i] = -1;
    }
    if (probe_from_startup_args() != 0) {
        vin_log("[virtio-input] startup args name no virtio-input function\n");
        return WASMOS_ERR_DRIVER_NO_DEVICE_IDENTITY;
    }
    int32_t rc = vin_probe_device();
    if (rc != 0) {
        return rc;
    }
    g_ep = wasmos_ipc_create_endpoint();
    if (g_ep < 0) {
        return WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    }
    wasmos_sys_event_loop_init(&g_loop, g_ep, VIN_LOOP_REQ_BASE);
    if (g_loop.select_id < 0) {
        return WASMOS_ERR_DRIVER_SELECT_SETUP;
    }
    g_loop.poll_timeout_ms = VIN_LOOP_TICK_MS;
    g_loop.on_timeout = vin_on_tick;
    (void)wasmos_sys_event_set_default(&g_loop, vin_on_unclaimed, 0);
    if (wasmos_sys_event_register(&g_loop, KBD_IPC_SUBSCRIBE_REQ, vin_on_subscribe, 0) != 0 ||
        wasmos_sys_event_register(&g_loop, MOUSE_IPC_SUBSCRIBE_REQ, vin_on_subscribe, 0) != 0 ||
        wasmos_sys_event_register(&g_loop, WASMOS_IPC_MSI_EVENT_TYPE, vin_on_msi, 0) != 0 ||
        wasmos_sys_event_register(&g_loop, VIN_IRQ_EVENT_TYPE, vin_on_intx, 0) != 0 ||
        wasmos_sys_svc_watch_set_init(&g_watches, &g_loop, g_proc_ep) != 0) {
        return WASMOS_ERR_DRIVER_EVENT_LOOP;
    }
    memset(&g_pci_watch, 0, sizeof(g_pci_watch));
    if (wasmos_sys_svc_watch_start(&g_watches,
                                   &g_pci_watch,
                                   "pci",
                                   VIN_PCI_WAIT_MS,
                                   vin_on_pci_bound,
                                   vin_on_pci_failed,
                                   0) != 0) {
        vin_fall_back_to_intx();
    }

    for (;;) {
        (void)wasmos_sys_event_loop_poll(&g_loop, 16);
        wasmos_sys_svc_watch_set_expire(&g_watches);
    }
    return 0;
}
