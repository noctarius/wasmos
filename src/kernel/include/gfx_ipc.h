/* gfx_ipc.h - IPC message types and pixel-format constants for the GFX compositor (kernel copy). */
#ifndef WASMOS_KERNEL_GFX_IPC_H
#define WASMOS_KERNEL_GFX_IPC_H

#include <stdint.h>

#define FB_IPC_ABI_MAGIC   0x46424950u /* FBIP */
#define FB_IPC_ABI_VERSION 1u

#define GFX_IPC_ABI_MAGIC   0x47465850u /* GFXP */
#define GFX_IPC_ABI_VERSION 1u

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

enum {
    GFX_IPC_CREATE_WINDOW = 0x0200,
    GFX_IPC_DESTROY_WINDOW = 0x0201,
    GFX_IPC_RESIZE_WINDOW = 0x0202,
    GFX_IPC_ALLOC_SHARED_BUFFER = 0x0203,
    GFX_IPC_SUBMIT_COMMANDS = 0x0204,
    GFX_IPC_PRESENT_WINDOW = 0x0205,
    GFX_IPC_POLL_EVENT = 0x0206,
    GFX_IPC_RELEASE_SHARED_BUFFER = 0x0207,
    GFX_IPC_SET_DISPLAY_MODE = 0x0208,
    GFX_IPC_LIST_WINDOWS = 0x0209,
    GFX_IPC_FOCUS_WINDOW = 0x020A,
    GFX_IPC_SET_WINDOW_FLAGS = 0x020B,
    GFX_IPC_GET_DISPLAY_INFO = 0x020C,
    GFX_IPC_MOVE_WINDOW = 0x020D,
    GFX_IPC_SET_WINDOW_TITLE = 0x020E, /* arg0=window_id arg1=shmem_id arg2=title_len arg3=0 */
    GFX_IPC_GET_WINDOW_TITLE = 0x020F, /* arg0=window_id arg1=shmem_id arg2=max_len arg3=0; reply arg1=actual_len */
    GFX_IPC_RESP = 0x0280,
    GFX_IPC_ERROR = 0x02FF
};

/* Window flags for GFX_IPC_SET_WINDOW_FLAGS. These bits compose. */
#define GFX_WINDOW_FLAG_TOPMOST          (1u << 0)
#define GFX_WINDOW_FLAG_NO_CHROME        (1u << 1)
#define GFX_WINDOW_FLAG_INVISIBLE        (1u << 2)
#define GFX_WINDOW_FLAG_PASSTHROUGH_ZERO (1u << 3)
#define GFX_WINDOW_FLAG_NO_ACTIVATE      (1u << 4)
#define GFX_WINDOW_FLAG_NO_CONTENT       (1u << 5)
#define GFX_WINDOW_FLAG_NO_TASK_LIST     (1u << 6) /* exclude from GFX_IPC_LIST_WINDOWS */

/* Provisional v1 message argument contracts:
 * - GFX_IPC_CREATE_WINDOW:  arg0=width arg1=height arg2=GFX_IPC_ABI_MAGIC
 *                           arg3=(version<<16)|opcode
 * - GFX_IPC_DESTROY_WINDOW: arg0=window_id arg1..arg3 reserved
 * - GFX_IPC_RESIZE_WINDOW:  arg0=window_id arg1=width arg2=height
 * - GFX_IPC_ALLOC_SHARED_BUFFER:
 *                           arg0=window_id(0=unbound) arg1=width arg2=height
 *                           reply: arg1=buffer_id arg2=shmem_id arg3=stride
 * - GFX_IPC_PRESENT_WINDOW: arg0=window_id arg1=buffer_id
 *                           arg2=damage_count arg3=damage_shmem_id
 * - GFX_IPC_RELEASE_SHARED_BUFFER:
 *                           arg0=buffer_id arg1..arg3 reserved
 * - GFX_IPC_SET_DISPLAY_MODE:
 *                           arg0=width arg1=height arg2/arg3 reserved
 *                           reply: arg1=width arg2=height
 * - GFX_IPC_SET_WINDOW_TITLE: arg0=window_id arg1=shmem_id arg2=title_len(1..47) arg3=0
 *                           Caller writes title bytes to shmem before sending.
 *                           Only the window owner may set its title.
 * - GFX_IPC_GET_WINDOW_TITLE: arg0=window_id arg1=shmem_id(0=query-only) arg2=max_len arg3=0
 *                           reply arg1=actual_len (0 if no title set).
 *                           If shmem_id==0 only the length is returned.
 */

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} gfx_rect_t;

enum {
    GFX_EVENT_NONE = 0,
    GFX_EVENT_FOCUS_GAINED = 1,
    GFX_EVENT_FOCUS_LOST = 2,
    GFX_EVENT_KEY = 3,
    GFX_EVENT_POINTER = 4,
    GFX_EVENT_CLOSE_REQUEST = 5,
    GFX_EVENT_RESIZE = 6,
    GFX_EVENT_POINTER_GESTURE = 7
};

enum {
    GFX_POINTER_BUTTON_LEFT = 1,
    GFX_POINTER_BUTTON_RIGHT = 2,
    GFX_POINTER_BUTTON_MIDDLE = 3
};

enum {
    GFX_POINTER_GESTURE_DOWN = 1,
    GFX_POINTER_GESTURE_UP = 2,
    GFX_POINTER_GESTURE_CLICK = 3,
    GFX_POINTER_GESTURE_DOUBLE_CLICK = 4,
    GFX_POINTER_GESTURE_DRAG_START = 5,
    GFX_POINTER_GESTURE_DRAG_MOVE = 6,
    GFX_POINTER_GESTURE_DRAG_END = 7
};

static inline uint32_t
gfx_pointer_gesture_pack(uint32_t x, uint32_t y, uint32_t button, uint32_t gesture)
{
    return (x & 0xFFFu) |
           ((y & 0xFFFu) << 12) |
           ((button & 0xFu) << 24) |
           ((gesture & 0xFu) << 28);
}

static inline uint32_t gfx_pointer_gesture_x(uint32_t packed) { return packed & 0xFFFu; }
static inline uint32_t gfx_pointer_gesture_y(uint32_t packed) { return (packed >> 12) & 0xFFFu; }
static inline uint32_t gfx_pointer_gesture_button(uint32_t packed) { return (packed >> 24) & 0xFu; }
static inline uint32_t gfx_pointer_gesture_kind(uint32_t packed) { return (packed >> 28) & 0xFu; }

#endif
