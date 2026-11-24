/* test_virtio_input_events.c -- the device-independent half of the virtio-input
 * driver (src/drivers/input/virtio_input/virtio_input_events.c): subscriber slots,
 * evdev-to-PS/2 Set 1 translation and mouse report publishing. Notifications
 * are recorded by a fake notify callback, so every case asserts what the
 * subscribers would have received.
 *
 * The cases report through assert(); main() prints "ok" only if every case ran
 * to completion. The suite is compiled without -DNDEBUG. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "virtio_input_events.h"
#include "wasmos_driver_abi.h"

typedef struct {
    int32_t ep, type, a0, a1, a2;
} sent_t;

static sent_t g_sent[64];
static int g_nsent;

static int32_t record(void* user, int32_t ep, int32_t type, int32_t a0, int32_t a1, int32_t a2) {
    (void)user;
    assert(g_nsent < (int)(sizeof(g_sent) / sizeof(g_sent[0])));
    g_sent[g_nsent++] = (sent_t){ep, type, a0, a1, a2};
    return 0;
}

static void reset(vin_events_t* ev) {
    g_nsent = 0;
    vin_events_init(ev, record, 0);
}

static int32_t s8(int32_t v) {
    return (int32_t)(int8_t)(uint8_t)v;
}

static void test_key_translation(void) {
    vin_events_t ev;
    reset(&ev);
    assert(vin_events_subscribe(&ev, 7) == 0);
    vin_events_key(&ev, 30, 1);  /* KEY_A press */
    vin_events_key(&ev, 30, 0);  /* release */
    vin_events_key(&ev, 103, 2); /* KEY_UP autorepeat */
    vin_events_key(&ev, 84, 1);  /* no Set 1 code: dropped */
    assert(g_nsent == 3);
    assert(g_sent[0].type == KBD_IPC_KEY_NOTIFY && g_sent[0].a0 == 0x1E && g_sent[0].a1 == 0 &&
           g_sent[0].a2 == 0);
    assert(g_sent[1].a0 == 0x1E && g_sent[1].a1 == 1);
    assert(g_sent[2].a0 == 0x48 && g_sent[2].a1 == 0 && g_sent[2].a2 == 1);
}

static void test_subscriber_slots_are_bounded(void) {
    vin_events_t ev;
    reset(&ev);
    for (int32_t ep = 10; ep < 10 + VIN_MAX_SUBSCRIBERS; ++ep) {
        assert(vin_events_subscribe(&ev, ep) == 0);
    }
    assert(vin_events_subscribe(&ev, 10) == 0); /* a repeat takes no slot */
    assert(vin_events_subscribe(&ev, 99) == WASMOS_ERR_DRIVER_NO_SUBSCRIBER_SLOT);
}

/* Regression: 2026-11-24-virtio-input-hangup-slots. A subscriber that exited
 * kept its slot: WASMOS_IPC_HANGUP reached the unclaimed-message path and was
 * dropped, so four restarts of the vt or compositor filled the table and every
 * later client was refused input until the driver itself restarted. */
static void test_hangup_frees_the_subscriber_slot(void) {
    vin_events_t ev;
    reset(&ev);
    for (int32_t ep = 10; ep < 10 + VIN_MAX_SUBSCRIBERS; ++ep) {
        assert(vin_events_subscribe(&ev, ep) == 0);
    }
    vin_events_unclaimed(&ev, WASMOS_IPC_HANGUP, 11);
    vin_events_unclaimed(&ev, WASMOS_IPC_HANGUP, 11); /* idempotent (doc 09) */
    assert(vin_events_subscribe(&ev, 99) == 0);
    vin_events_key(&ev, 30, 1);
    for (int i = 0; i < g_nsent; ++i) {
        assert(g_sent[i].ep != 11);
    }
    assert(g_nsent == VIN_MAX_SUBSCRIBERS);
}

/* Regression: 2026-11-24-virtio-input-split-click. A report moving more than
 * one 8-bit step AND pressing a button published the press on the FIRST step,
 * so the compositor hit-tested the click 127 pixels into a 300-pixel move,
 * where it could land on a different window. */
static void test_split_motion_publishes_buttons_on_the_last_step(void) {
    vin_events_t ev;
    reset(&ev);
    assert(vin_events_subscribe(&ev, 7) == 0);
    vin_events_mouse(&ev, VIN_EV_REL, VIN_REL_X, 300u);
    vin_events_mouse(&ev, VIN_EV_KEY, VIN_BTN_LEFT, 1u);
    vin_events_mouse(&ev, VIN_EV_SYN, VIN_SYN_REPORT, 0u);
    assert(g_nsent == 3);
    assert(s8(g_sent[0].a0) == 127 && g_sent[0].a2 == 0);
    assert(s8(g_sent[1].a0) == 127 && g_sent[1].a2 == 0);
    assert(s8(g_sent[2].a0) == 46 && g_sent[2].a2 == 1);

    /* The release in the next report, again split, keeps the button held
     * until the last step. */
    g_nsent = 0;
    vin_events_mouse(&ev, VIN_EV_REL, VIN_REL_Y, (uint32_t)-200);
    vin_events_mouse(&ev, VIN_EV_KEY, VIN_BTN_LEFT, 0u);
    vin_events_mouse(&ev, VIN_EV_SYN, VIN_SYN_REPORT, 0u);
    assert(g_nsent == 2);
    assert(s8(g_sent[0].a1) == -127 && g_sent[0].a2 == 1);
    assert(s8(g_sent[1].a1) == -73 && g_sent[1].a2 == 0);
}

static void test_button_only_report(void) {
    vin_events_t ev;
    reset(&ev);
    assert(vin_events_subscribe(&ev, 7) == 0);
    vin_events_mouse(&ev, VIN_EV_KEY, VIN_BTN_RIGHT, 1u);
    vin_events_mouse(&ev, VIN_EV_SYN, VIN_SYN_REPORT, 0u);
    vin_events_mouse(&ev, VIN_EV_SYN, VIN_SYN_REPORT, 0u); /* nothing new */
    assert(g_nsent == 1);
    assert(g_sent[0].type == MOUSE_IPC_MOVE_NOTIFY && g_sent[0].a0 == 0 && g_sent[0].a1 == 0 &&
           g_sent[0].a2 == 2);
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_key_translation),
        WASMOS_TEST_CASE(test_subscriber_slots_are_bounded),
        WASMOS_TEST_CASE(test_hangup_frees_the_subscriber_slot),
        WASMOS_TEST_CASE(test_split_motion_publishes_buttons_on_the_last_step),
        WASMOS_TEST_CASE(test_button_only_report),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("ok\n");
    return 0;
}
