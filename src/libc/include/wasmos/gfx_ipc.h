/* gfx_ipc.h - IPC message types and pixel-format constants for the GFX compositor.
 * Shared between the compositor service, native framebuffer driver, and any
 * WASM app that wants to send raw draw commands to the compositor. */
#ifndef WASMOS_LIBC_WASMOS_GFX_IPC_H
#define WASMOS_LIBC_WASMOS_GFX_IPC_H

#include <stdint.h>

/* Protocol identity words, sent in a request argument so a peer can reject
 * traffic meant for a different protocol or ABI generation. The magics are the
 * ASCII tags below; the versions are bumped whenever an argument contract in
 * this header changes incompatibly. A CREATE_WINDOW carries the gfx pair as
 * arg2 = GFX_IPC_ABI_MAGIC and the packed version/opcode as arg3. */
#define FB_IPC_ABI_MAGIC 0x46424950u /* FBIP */
#define FB_IPC_ABI_VERSION 1u

#define GFX_IPC_ABI_MAGIC 0x47465850u /* GFXP */
#define GFX_IPC_ABI_VERSION 1u

/* Framebuffer-driver opcode block (0x0100..), reserved for the display-device
 * protocol between the compositor and a framebuffer/scanout driver. Nothing in
 * the tree sends or serves them: the compositor talks to the "fb" service with
 * the generated FBTEXT_IPC_* opcodes and maps the scanout through the
 * framebuffer host calls. */
enum {
    FB_IPC_GET_INFO = 0x0100,
    FB_IPC_SET_MODE = 0x0101,
    FB_IPC_MAP_SCANOUT = 0x0102,
    FB_IPC_ALLOC_BUFFER = 0x0103,
    FB_IPC_PRESENT = 0x0104,
    FB_IPC_WAIT_VBLANK = 0x0105,
    FB_IPC_QUERY_CAPS = 0x0106,
    FB_IPC_QUERY_MODES = 0x0107
};

/* GFX_IPC_* opcodes come from the generated IPC opcode ABI (abi/opcodes.yaml). */
#include "../../../../abi/generated/c/wasmos_opcodes.h"
#include "../../../../abi/generated/c/wasmos_status.h"

/* Provisional v1 message argument contracts:
 * - GFX_IPC_CREATE_WINDOW:  arg0=content_width arg1=content_height arg2=GFX_IPC_ABI_MAGIC
 *                           arg3=gfx_ipc_header_pack(version, opcode)
 * - GFX_IPC_DESTROY_WINDOW: arg0=window_id arg1..arg3 reserved
 * - GFX_IPC_RESIZE_WINDOW:  arg0=window_id arg1=content_width arg2=content_height
 * - GFX_IPC_ALLOC_SHARED_BUFFER:
 *                           arg0=window_id(0=unbound) arg1=width arg2=height
 *                           reply: arg1=buffer_id arg2=shmem_id arg3=stride
 * - GFX_IPC_PRESENT_WINDOW: arg0=window_id arg1=buffer_id
 *                           arg2=damage_count arg3=damage_shmem_id
 * - GFX_IPC_PUSH_EVENT:     server->client; arg1=event_type arg2=window_id arg3=payload
 *                           reply: arg1=event_type arg2=event_arg1 arg3=event_arg2
 * - GFX_IPC_RELEASE_SHARED_BUFFER:
 *                           arg0=buffer_id arg1..arg3 reserved
 * - GFX_IPC_SET_DISPLAY_MODE:
 *                           arg0=width arg1=height arg2/arg3 reserved
 *                           reply: arg1=width arg2=height
 * - GFX_IPC_LIST_WINDOWS:   arg0=index (0-based)
 *                           reply: arg1=window_id(0=end), arg2=owner_endpoint,
 *                                  arg3=total_active_windows
 * - GFX_IPC_FOCUS_WINDOW:   arg0=window_id
 * - GFX_IPC_SET_WINDOW_FLAGS: arg0=window_id arg1=flags (GFX_WINDOW_FLAG_*)
 * - GFX_IPC_GET_DISPLAY_INFO: arg0..arg3 reserved
 *                           reply: arg1=width arg2=height
 * - GFX_IPC_MOVE_WINDOW:    arg0=window_id arg1=x arg2=y arg3 reserved
 * - GFX_IPC_SET_WINDOW_TITLE: arg0=window_id arg1=shmem_id arg2=title_len(1..47) arg3=0
 *                           Caller writes title bytes to shmem before sending.
 *                           Only the window owner may set its title.
 * - GFX_IPC_GET_WINDOW_TITLE: arg0=window_id arg1=shmem_id(0=query-only) arg2=max_len arg3=0
 *                           reply arg1=actual_len (0 if no title set).
 *                           If shmem_id==0 only the length is returned.
 * Window create/resize dimensions are logical content dimensions. Compositor
 * chrome, when enabled, is added outside that content rect.
 */

/* Window flags for GFX_IPC_SET_WINDOW_FLAGS. These bits compose.
 * SET_WINDOW_FLAGS REPLACES the whole word rather than ORing into it, so a
 * caller changing one bit must resend the others, and only the window's owner
 * may set them.
 *   TOPMOST           pins the window to the system z-band, above ordinary
 *                     windows, and stops clicks from re-raising it.
 *   NO_CHROME         no title bar or border: the content rect is the whole
 *                     window rect and there are no move/resize/close targets.
 *   INVISIBLE         skipped when compositing and when hit-testing the
 *                     pointer; setting it drops focus if the window had it.
 *   PASSTHROUGH_ZERO  pixels whose value is 0 composite as transparent instead
 *                     of opaque black (for no-chrome overlays).
 *   NO_ACTIVATE       never takes focus, neither on click nor on first present;
 *                     setting it drops focus if the window had it.
 *   NO_CONTENT        the content rect is empty (chrome only) and any presented
 *                     buffer is dropped.
 *   NO_TASK_LIST      excluded from GFX_IPC_LIST_WINDOWS (and its count). */
#define GFX_WINDOW_FLAG_TOPMOST (1u << 0)
#define GFX_WINDOW_FLAG_NO_CHROME (1u << 1)
#define GFX_WINDOW_FLAG_INVISIBLE (1u << 2)
#define GFX_WINDOW_FLAG_PASSTHROUGH_ZERO (1u << 3)
#define GFX_WINDOW_FLAG_NO_ACTIVATE (1u << 4)
#define GFX_WINDOW_FLAG_NO_CONTENT (1u << 5)
#define GFX_WINDOW_FLAG_NO_TASK_LIST (1u << 6) /* exclude from GFX_IPC_LIST_WINDOWS */

/* Axis-aligned rectangle in whole pixels: (x, y) is the top-left corner and
 * w/h are extents, so the covered range is [x, x+w) x [y, y+h). Signed, because
 * a window rect may be partly off-screen; an empty rect has w or h <= 0. */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} gfx_rect_t;

/* Request statuses are the packed gfx domain in abi/errors.yaml:
 * WASMOS_ERR_NONE (0) on success, else a negative WASMOS_ERR_GFX_*. */

enum {
    GFX_EVENT_NONE = 0,
    GFX_EVENT_FOCUS_GAINED = 1,
    GFX_EVENT_FOCUS_LOST = 2,
    /* arg2=packed key code: low byte = the character the vt decoded with the
     * active keymap (0 when the key has none, e.g. arrows/function keys),
     * high byte = raw set-1 scancode.  Consumers must mask; the packed value
     * is not a codepoint.  arg3=flags: bit0=down, bit1=extended, bit2=shift,
     * bit3=ctrl, bit4=altgr. */
    GFX_EVENT_KEY = 3,
    /* arg2=window_id, arg3=gfx_pointer_event_pack(x, y, buttons)
     * where x/y are content-local coordinates and buttons is the low 8-bit
     * button mask. */
    GFX_EVENT_POINTER = 4,
    /* arg2=window_id, arg3 reserved */
    GFX_EVENT_CLOSE_REQUEST = 5,
    /* arg2=window_id, arg3 packs width/height as u16: low16=width high16=height */
    GFX_EVENT_RESIZE = 6,
    /* arg2=window_id, arg3=gfx_pointer_gesture_pack(x, y, button, gesture)
     * where x/y are content-local coordinates, button is one of
     * GFX_POINTER_BUTTON_*, and gesture is one of GFX_POINTER_GESTURE_*.
     * This is the higher-level app-facing pointer contract for click/down/up
     * and drag lifecycle events. */
    GFX_EVENT_POINTER_GESTURE = 7
};

/* Which button a GFX_EVENT_POINTER_GESTURE is about; 0 is reserved for "no
 * button". These are identifiers, not mask bits — the bit mask in
 * GFX_EVENT_POINTER is a separate encoding. */
enum { GFX_POINTER_BUTTON_LEFT = 1, GFX_POINTER_BUTTON_RIGHT = 2, GFX_POINTER_BUTTON_MIDDLE = 3 };

/* Gesture kinds the compositor derives from raw button transitions, delivered to
 * the window under the press. A press emits DOWN; a release emits UP, preceded
 * by DRAG_END if a drag was running. A press/release pair that stays within a
 * few pixels and a short tick window also emits CLICK after the UP; a second
 * left click on the same window soon after emits DOUBLE_CLICK in addition to
 * that CLICK, not instead of it. Moving beyond the slop while a button is held
 * emits DRAG_START and then a DRAG_MOVE per motion. */
enum {
    GFX_POINTER_GESTURE_DOWN = 1,
    GFX_POINTER_GESTURE_UP = 2,
    GFX_POINTER_GESTURE_CLICK = 3,
    GFX_POINTER_GESTURE_DOUBLE_CLICK = 4,
    GFX_POINTER_GESTURE_DRAG_START = 5,
    GFX_POINTER_GESTURE_DRAG_MOVE = 6,
    GFX_POINTER_GESTURE_DRAG_END = 7
};

/* Unpacked form of the protocol header the argument words carry: `magic` is
 * GFX_IPC_ABI_MAGIC, `version`/`opcode` are the two halves of a
 * gfx_ipc_header_pack word, `request_id` correlates a reply with its request,
 * and `status` is WASMOS_ERR_NONE or a negative WASMOS_ERR_GFX_* on a reply. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t request_id;
    int32_t status;
} gfx_ipc_hdr_t;

/* Pack version (high 16 bits) and opcode (low 16) into the single argument word
 * a request carries alongside GFX_IPC_ABI_MAGIC. */
static inline uint32_t gfx_ipc_header_pack(uint16_t version, uint16_t opcode) {
    return ((uint32_t)version << 16) | (uint32_t)opcode;
}

/* Returns 1 when `magic` is GFX_IPC_ABI_MAGIC and the version half of
 * `ver_opcode` is GFX_IPC_ABI_VERSION, 0 otherwise. The opcode half is not
 * checked — the receiver dispatches on the message type. */
static inline int gfx_ipc_header_valid(uint32_t magic, uint32_t ver_opcode) {
    uint16_t version = (uint16_t)(ver_opcode >> 16);
    return magic == GFX_IPC_ABI_MAGIC && version == GFX_IPC_ABI_VERSION;
}

/* Pack/unpack the GFX_EVENT_POINTER arg3 word: x in bits 0..11, y in bits
 * 12..23, the button mask in bits 24..31. Coordinates are content-local pixels
 * and each field is masked to its width, so a coordinate above 4095 wraps rather
 * than saturating. */
static inline uint32_t gfx_pointer_event_pack(uint32_t x, uint32_t y, uint32_t buttons) {
    return (x & 0xFFFu) | ((y & 0xFFFu) << 12) | ((buttons & 0xFFu) << 24);
}

static inline uint32_t gfx_pointer_event_x(uint32_t packed) {
    return packed & 0xFFFu;
}
static inline uint32_t gfx_pointer_event_y(uint32_t packed) {
    return (packed >> 12) & 0xFFFu;
}
static inline uint32_t gfx_pointer_event_buttons(uint32_t packed) {
    return (packed >> 24) & 0xFFu;
}

/* Pack/unpack the GFX_EVENT_POINTER_GESTURE arg3 word: x in bits 0..11, y in
 * bits 12..23, a single GFX_POINTER_BUTTON_* in bits 24..27 and a
 * GFX_POINTER_GESTURE_* in bits 28..31 — one button per event, unlike the
 * button mask of gfx_pointer_event_pack. Same 12-bit coordinate wrapping. */
static inline uint32_t gfx_pointer_gesture_pack(uint32_t x, uint32_t y, uint32_t button,
                                                uint32_t gesture) {
    return (x & 0xFFFu) | ((y & 0xFFFu) << 12) | ((button & 0xFu) << 24) | ((gesture & 0xFu) << 28);
}

static inline uint32_t gfx_pointer_gesture_x(uint32_t packed) {
    return packed & 0xFFFu;
}
static inline uint32_t gfx_pointer_gesture_y(uint32_t packed) {
    return (packed >> 12) & 0xFFFu;
}
static inline uint32_t gfx_pointer_gesture_button(uint32_t packed) {
    return (packed >> 24) & 0xFu;
}
static inline uint32_t gfx_pointer_gesture_kind(uint32_t packed) {
    return (packed >> 28) & 0xFu;
}

#endif
