/* virtio_input_events.h - the device-independent half of the virtio-input
 * driver: the subscriber table, evdev-to-PS/2 translation and mouse report
 * accumulation. Nothing here touches the device or makes a host call;
 * notifications leave through `notify`, so the logic runs on the host under
 * tests/unit/test_virtio_input_events.c. */
#ifndef WASMOS_VIRTIO_INPUT_EVENTS_H
#define WASMOS_VIRTIO_INPUT_EVENTS_H

#include <stdint.h>

#define VIN_MAX_SUBSCRIBERS 4
/* MOUSE_IPC_MOVE_NOTIFY carries a signed 8-bit delta per axis. */
#define VIN_MOUSE_STEP_MAX 127

/* evdev event types and codes (Linux input-event-codes.h, which virtio-input
 * adopts verbatim). */
#define VIN_EV_SYN 0x00u
#define VIN_EV_KEY 0x01u
#define VIN_EV_REL 0x02u
#define VIN_EV_ABS 0x03u
#define VIN_SYN_REPORT 0u
#define VIN_REL_X 0x00u
#define VIN_REL_Y 0x01u
#define VIN_BTN_LEFT 0x110u
#define VIN_BTN_RIGHT 0x111u
#define VIN_BTN_MIDDLE 0x112u

/* Sends one notification to subscriber endpoint `ep`; returns the send's
 * status (0 delivered). */
typedef int32_t (*vin_notify_fn)(void* user, int32_t ep, int32_t type, int32_t a0, int32_t a1,
                                 int32_t a2);

typedef struct {
    int32_t subscribers[VIN_MAX_SUBSCRIBERS]; /* -1 = free */
    /* Mouse state accumulated since the last SYN_REPORT. `buttons` is the
     * state the device reports; `published_buttons` the state subscribers last
     * saw. */
    int32_t dx;
    int32_t dy;
    int32_t buttons;
    int32_t published_buttons;
    uint8_t dirty;
    vin_notify_fn notify;
    void* user;
} vin_events_t;

void vin_events_init(vin_events_t* ev, vin_notify_fn notify, void* user);

/* Adds `ep` (a repeat is a no-op). Returns 0, or
 * WASMOS_ERR_DRIVER_NO_SUBSCRIBER_SLOT when every slot is taken: a subscribe
 * beyond VIN_MAX_SUBSCRIBERS is refused, not queued. The value is the
 * subscribe reply's arg0. */
int32_t vin_events_subscribe(vin_events_t* ev, int32_t ep);

/* A message no loop handler claimed, of `type` with first argument `arg0`. */
void vin_events_unclaimed(vin_events_t* ev, int32_t type, int32_t arg0);

/* The PS/2 Set 1 make code of evdev key `code`, bit 8 set for an
 * 0xE0-prefixed key, or 0 when Set 1 cannot express it. Layout-neutral: both
 * codes name a key POSITION, and the vt's keymap turns a position into a
 * character. */
uint32_t vin_events_set1(uint32_t code);

/* One EV_KEY event from a keyboard: value 0 release, 1 press, 2 autorepeat
 * (another make code, as a PS/2 keyboard's typematic repeat is). */
void vin_events_key(vin_events_t* ev, uint32_t code, uint32_t value);

/* One event from a mouse. EV_REL and button EV_KEY accumulate; SYN_REPORT
 * publishes the report as MOUSE_IPC_MOVE_NOTIFYs of at most
 * VIN_MOUSE_STEP_MAX per axis. */
void vin_events_mouse(vin_events_t* ev, uint32_t type, uint32_t code, uint32_t value);

#endif
