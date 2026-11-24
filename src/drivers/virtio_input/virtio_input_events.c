/* virtio_input_events.c - see virtio_input_events.h. */
#include "virtio_input_events.h"

#include "wasmos_driver_abi.h"

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

void vin_events_init(vin_events_t* ev, vin_notify_fn notify, void* user) {
    for (int i = 0; i < VIN_MAX_SUBSCRIBERS; ++i) {
        ev->subscribers[i] = -1;
    }
    ev->dx = 0;
    ev->dy = 0;
    ev->buttons = 0;
    ev->published_buttons = 0;
    ev->dirty = 0;
    ev->notify = notify;
    ev->user = user;
}

int32_t vin_events_subscribe(vin_events_t* ev, int32_t ep) {
    for (int i = 0; i < VIN_MAX_SUBSCRIBERS; ++i) {
        if (ev->subscribers[i] == ep) {
            return 0;
        }
    }
    for (int i = 0; i < VIN_MAX_SUBSCRIBERS; ++i) {
        if (ev->subscribers[i] < 0) {
            ev->subscribers[i] = ep;
            return 0;
        }
    }
    return WASMOS_ERR_DRIVER_NO_SUBSCRIBER_SLOT;
}

/* WASMOS_IPC_HANGUP names in arg0 a peer endpoint that ended
 * (docs/architecture/09-process-and-ipc.md, "Peer Hangup"): a subscriber that
 * exited gives its slot back, so a restarted client finds one free. A context
 * can be reported more than once, and releasing an unknown endpoint is a
 * no-op. Anything else unclaimed is dropped, never answered: an error reply to
 * an unsolicited message can ping-pong with a peer that does the same. */
void vin_events_unclaimed(vin_events_t* ev, int32_t type, int32_t arg0) {
    if (type != WASMOS_IPC_HANGUP || arg0 < 0) {
        return;
    }
    for (int i = 0; i < VIN_MAX_SUBSCRIBERS; ++i) {
        if (ev->subscribers[i] == arg0) {
            ev->subscribers[i] = -1;
        }
    }
}

/* TODO: a notify a full subscriber queue refuses is lost, so a missed key or
 * button release leaves that client seeing the key held. The PS/2 drivers
 * share the gap; closing it means a bounded per-subscriber backlog retried
 * from the loop (docs/TASKS.md, input drivers). */
static void vin_notify_all(vin_events_t* ev, int32_t type, int32_t a0, int32_t a1, int32_t a2) {
    for (int i = 0; i < VIN_MAX_SUBSCRIBERS; ++i) {
        if (ev->subscribers[i] >= 0) {
            (void)ev->notify(ev->user, ev->subscribers[i], type, a0, a1, a2);
        }
    }
}

uint32_t vin_events_set1(uint32_t code) {
    return code < sizeof(k_set1) / sizeof(k_set1[0]) ? k_set1[code] : 0u;
}

void vin_events_key(vin_events_t* ev, uint32_t code, uint32_t value) {
    uint32_t sc = vin_events_set1(code);
    if (sc == 0u) {
        return;
    }
    int32_t keyup = (value == 0u) ? 1 : 0;
    int32_t extended = (sc & 0x100u) ? 1 : 0;
    vin_notify_all(ev, KBD_IPC_KEY_NOTIFY, (int32_t)(sc & 0x7Fu), keyup, extended);
}

static int32_t vin_clamp_step(int32_t v) {
    if (v > VIN_MOUSE_STEP_MAX) {
        return VIN_MOUSE_STEP_MAX;
    }
    if (v < -VIN_MOUSE_STEP_MAX) {
        return -VIN_MOUSE_STEP_MAX;
    }
    return v;
}

/* Publishes the report accumulated since the last SYN_REPORT. A move larger
 * than one step is split, and the report's button change belongs to where the
 * move ENDS: the compositor hit-tests a press at the pointer position of the
 * notify that carries it, so every step but the last repeats the buttons
 * already published. */
static void vin_mouse_flush(vin_events_t* ev) {
    if (!ev->dirty) {
        return;
    }
    do {
        int32_t sx = vin_clamp_step(ev->dx);
        int32_t sy = vin_clamp_step(ev->dy);
        ev->dx -= sx;
        ev->dy -= sy;
        int32_t last = (ev->dx == 0 && ev->dy == 0);
        vin_notify_all(ev,
                       MOUSE_IPC_MOVE_NOTIFY,
                       sx & 0xFF,
                       sy & 0xFF,
                       last ? ev->buttons : ev->published_buttons);
    } while (ev->dx != 0 || ev->dy != 0);
    ev->published_buttons = ev->buttons;
    ev->dirty = 0;
}

void vin_events_mouse(vin_events_t* ev, uint32_t type, uint32_t code, uint32_t value) {
    if (type == VIN_EV_REL) {
        if (code == VIN_REL_X) {
            ev->dx += (int32_t)value;
            ev->dirty = 1;
        } else if (code == VIN_REL_Y) {
            ev->dy += (int32_t)value;
            ev->dirty = 1;
        }
    } else if (type == VIN_EV_KEY) {
        int32_t bit = (code == VIN_BTN_LEFT)     ? 0x1
                      : (code == VIN_BTN_RIGHT)  ? 0x2
                      : (code == VIN_BTN_MIDDLE) ? 0x4
                                                 : 0;
        if (bit != 0) {
            ev->buttons = value ? (ev->buttons | bit) : (ev->buttons & ~bit);
            ev->dirty = 1;
        }
    } else if (type == VIN_EV_SYN && code == VIN_SYN_REPORT) {
        vin_mouse_flush(ev);
    }
}
