/* libsys_native.h - native (non-WASM) variant of libsys: function-pointer API
 * table (wasmos_driver_api_t) instead of hostcall imports, _native suffixes */
#ifndef WASMOS_LIBSYS_NATIVE_H
#define WASMOS_LIBSYS_NATIVE_H

#include <stdint.h>
#include "wasmos_native_driver.h"
#include "wasmos_driver_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

void wasmos_sys_ipc_pack_name16_native(const uint8_t *name, uint32_t name_len, uint32_t out_args[4]);
void wasmos_sys_ipc_unpack_name16_native(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint8_t *out, uint32_t out_len);
void wasmos_sys_ipc_recv_loop_native(wasmos_driver_api_t *api, uint32_t receiver_endpoint);

#define WASMOS_SYS_NATIVE_INTENT_MAX 16u
#define WASMOS_SYS_NATIVE_HANDLER_MAX 24u

/* Pending request awaiting a reply matched by request_id. */
typedef struct {
    uint8_t in_use;
    uint32_t request_id;
    void (*on_resolve)(void *user, const nd_ipc_message_t *msg);
    void *user;
} wasmos_sys_native_intent_t;

/* Registered handler for a specific IPC message type. */
typedef struct {
    uint8_t in_use;
    uint32_t msg_type;
    void (*on_message)(void *user, const nd_ipc_message_t *msg);
    void *user;
} wasmos_sys_native_handler_t;

/* Event loop state for native drivers: intent table for request/reply matching
 * and handler table for unsolicited message dispatch. */
typedef struct {
    wasmos_driver_api_t *api;
    uint32_t receiver_endpoint;
    uint32_t next_request_id;
    void (*default_on_message)(void *user, const nd_ipc_message_t *msg);
    void *default_user;
    wasmos_sys_native_intent_t intents[WASMOS_SYS_NATIVE_INTENT_MAX];
    wasmos_sys_native_handler_t handlers[WASMOS_SYS_NATIVE_HANDLER_MAX];
} wasmos_sys_native_event_loop_t;

/* Recursive mutex state; same binary layout as wasmos_mutex_t in libc/wasm. */
typedef struct {
    volatile uint32_t owner_tid;
    volatile uint32_t recursion_depth;
} wasmos_sys_mutex_t;

#define WASMOS_SYS_MUTEX_INITIALIZER {0u, 0u}

int32_t wasmos_sys_ipc_recv_matching_native(wasmos_driver_api_t *api, uint32_t receiver_endpoint, uint32_t request_id, nd_ipc_message_t *out_message);
int32_t wasmos_sys_svc_lookup_retry_native(wasmos_driver_api_t *api, uint32_t proc_endpoint, uint32_t source_endpoint, const uint8_t *name, uint32_t name_len, uint32_t request_id_base, uint32_t attempts);
int32_t wasmos_sys_ipc_send_retry_native(wasmos_driver_api_t *api, uint32_t destination_endpoint, uint32_t source_endpoint, uint32_t msg_type, uint32_t request_id, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t retries);
int32_t wasmos_sys_ipc_call_native(wasmos_driver_api_t *api, uint32_t source_endpoint, uint32_t destination, uint32_t request_id, uint32_t msg_type, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, nd_ipc_message_t *out_message);
int32_t wasmos_sys_svc_register_native(wasmos_driver_api_t *api, uint32_t proc_endpoint, uint32_t source_endpoint, const uint8_t *name, uint32_t name_len, uint32_t request_id);
int32_t wasmos_sys_svc_lookup_native(wasmos_driver_api_t *api, uint32_t proc_endpoint, uint32_t source_endpoint, const uint8_t *name, uint32_t name_len, uint32_t request_id);
/* Byte-range helpers for native services/drivers. These accept arbitrary
 * len/offset pairs and round the underlying buffer_borrow window up to page
 * size before calling the low-level ABI hook. */
int32_t wasmos_sys_buffer_copy_from_native(wasmos_driver_api_t *api, uint32_t kind, uint32_t source_endpoint, uint32_t borrow_flags, void *dst, int32_t len, int32_t offset);
int32_t wasmos_sys_buffer_write_to_native(wasmos_driver_api_t *api, uint32_t kind, uint32_t source_endpoint, uint32_t borrow_flags, const void *src, int32_t len, int32_t offset);
int32_t wasmos_sys_xfer_buffer_copy_from_endpoint_native(wasmos_driver_api_t *api, uint32_t source_endpoint, void *dst, int32_t len, int32_t offset);
int32_t wasmos_sys_xfer_buffer_write_to_endpoint_native(wasmos_driver_api_t *api, uint32_t source_endpoint, const void *src, int32_t len, int32_t offset);
int32_t wasmos_sys_fs_read_path_native(wasmos_driver_api_t *api, uint32_t fs_endpoint, uint32_t reply_endpoint, uint32_t request_id, const uint8_t *path, uint32_t path_len, uint8_t *out_text, int32_t out_text_len);
void wasmos_sys_byte_copy_native(uint8_t *dst, const uint8_t *src, uint32_t len);
int32_t wasmos_sys_be_u16_native(const uint8_t *data, uint32_t data_len, uint32_t off, uint16_t *out);
int32_t wasmos_sys_be_i16_native(const uint8_t *data, uint32_t data_len, uint32_t off, int16_t *out);
int32_t wasmos_sys_be_u32_native(const uint8_t *data, uint32_t data_len, uint32_t off, uint32_t *out);
int32_t wasmos_sys_find_table_native(const uint8_t *data, uint32_t data_len, const uint8_t tag[4], uint32_t *out_offset);
uint32_t wasmos_sys_pack_u16_pair_native(uint32_t a, uint32_t b);
uint32_t wasmos_sys_pack_s16_pair_native(int32_t a, int32_t b);
uint32_t wasmos_sys_hex_u32_native(uint32_t value, uint8_t *out, uint32_t out_len);
void wasmos_sys_native_event_loop_init(wasmos_sys_native_event_loop_t *loop,
                                       wasmos_driver_api_t *api,
                                       uint32_t receiver_endpoint,
                                       uint32_t request_id_base);
int32_t wasmos_sys_native_event_register(wasmos_sys_native_event_loop_t *loop,
                                         uint32_t msg_type,
                                         void (*on_message)(void *user, const nd_ipc_message_t *msg),
                                         void *user);
int32_t wasmos_sys_native_event_set_default(wasmos_sys_native_event_loop_t *loop,
                                            void (*on_message)(void *user, const nd_ipc_message_t *msg),
                                            void *user);
int32_t wasmos_sys_native_intent_send(wasmos_sys_native_event_loop_t *loop,
                                      uint32_t destination_endpoint,
                                      uint32_t source_endpoint,
                                      uint32_t msg_type,
                                      uint32_t arg0,
                                      uint32_t arg1,
                                      uint32_t arg2,
                                      uint32_t arg3,
                                      void (*on_resolve)(void *user, const nd_ipc_message_t *msg),
                                      void *user,
                                      uint32_t *out_request_id);
int32_t wasmos_sys_native_intent_send_with_request_id(wasmos_sys_native_event_loop_t *loop,
                                                      uint32_t destination_endpoint,
                                                      uint32_t source_endpoint,
                                                      uint32_t request_id,
                                                      uint32_t msg_type,
                                                      uint32_t arg0,
                                                      uint32_t arg1,
                                                      uint32_t arg2,
                                                      uint32_t arg3,
                                                      void (*on_resolve)(void *user, const nd_ipc_message_t *msg),
                                                      void *user);
void wasmos_sys_native_intent_cancel(wasmos_sys_native_event_loop_t *loop, uint32_t request_id);
int32_t wasmos_sys_native_event_loop_poll(wasmos_sys_native_event_loop_t *loop, uint32_t budget);

static inline void
wasmos_sys_mutex_init(wasmos_sys_mutex_t *mutex)
{
    if (!mutex) {
        return;
    }
    mutex->owner_tid = 0u;
    mutex->recursion_depth = 0u;
}

static inline int32_t
wasmos_sys_mutex_try_lock(wasmos_driver_api_t *api, wasmos_sys_mutex_t *mutex)
{
    if (!api || !mutex || !api->mutex_try_lock) {
        return -1;
    }
    return api->mutex_try_lock((uint64_t)(uintptr_t)mutex);
}

static inline int32_t
wasmos_sys_mutex_lock(wasmos_driver_api_t *api, wasmos_sys_mutex_t *mutex)
{
    int32_t rc = -1;
    if (!api || !mutex || !api->sched_yield) {
        return -1;
    }
    /* TODO(user-mutex-futex): add a sleep/wake path so contended user mutexes
     * stop yield-spinning once the kernel grows a futex-style primitive. */
    for (;;) {
        rc = wasmos_sys_mutex_try_lock(api, mutex);
        if (rc != 1) {
            return rc;
        }
        api->sched_yield();
    }
}

static inline int32_t
wasmos_sys_mutex_unlock(wasmos_driver_api_t *api, wasmos_sys_mutex_t *mutex)
{
    if (!api || !mutex || !api->mutex_unlock) {
        return -1;
    }
    return api->mutex_unlock((uint64_t)(uintptr_t)mutex);
}

#ifdef __cplusplus
}
#endif

#endif
