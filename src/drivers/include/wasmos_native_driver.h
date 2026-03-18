#ifndef WASMOS_NATIVE_DRIVER_H
#define WASMOS_NATIVE_DRIVER_H

/*
 * Self-contained ABI header shared between the kernel loader and native drivers.
 * Only depends on <stdint.h> so it compiles in freestanding driver builds.
 *
 * Struct layouts must match framebuffer_info_t and ipc_message_t in the kernel.
 * native_driver.c enforces this with _Static_assert.
 */

#include <stdint.h>

typedef struct {
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;
    uint32_t framebuffer_reserved;
} nd_framebuffer_info_t;

typedef struct {
    uint32_t type;
    uint32_t source;
    uint32_t destination;
    uint32_t request_id;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
} nd_ipc_message_t;

/*
 * Kernel API function table passed to every native driver's initialize().
 * All pointers resolve to shared higher-half kernel code, valid from any
 * process address space.
 */
typedef struct wasmos_driver_api {
    /* Console */
    int      (*console_write)(const char *ptr, int len);
    int      (*console_read)(char *ptr, int len);

    /* Framebuffer — framebuffer_map maps the physical framebuffer into the
     * driver's device address region and returns a virtual pointer to it. */
    int      (*framebuffer_info)(nd_framebuffer_info_t *out);
    void    *(*framebuffer_map)(uint32_t size);
    int      (*framebuffer_pixel)(uint32_t x, uint32_t y, uint32_t color);

    /* I/O ports */
    uint8_t  (*io_in8)(uint16_t port);
    uint16_t (*io_in16)(uint16_t port);
    void     (*io_out8)(uint16_t port, uint8_t val);
    void     (*io_out16)(uint16_t port, uint16_t val);

    /* IPC */
    uint32_t (*ipc_create_endpoint)(void);
    int      (*ipc_send)(uint32_t sender_context_id, uint32_t endpoint,
                         const nd_ipc_message_t *message);
    int      (*ipc_recv)(uint32_t receiver_context_id, uint32_t endpoint,
                         nd_ipc_message_t *out_message);

    /* Scheduler */
    void     (*sched_yield)(void);
    uint32_t (*sched_current_pid)(void);

    /* Process */
    void     (*proc_exit)(int code);
} wasmos_driver_api_t;

/* Entry point that every native driver must provide via ELF e_entry. */
typedef int (*native_driver_entry_fn_t)(wasmos_driver_api_t *api,
                                        int module_count,
                                        int arg2, int arg3);

#endif