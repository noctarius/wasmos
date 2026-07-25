/* libsys_native.h - native (non-WASM) variant of libsys: function-pointer API
 * table (wasmos_driver_api_t) instead of hostcall imports, _native suffixes */
#ifndef WASMOS_LIBSYS_NATIVE_H
#define WASMOS_LIBSYS_NATIVE_H

#include <stdint.h>
#include "wasmos_native_driver.h"
#include "wasmos_driver_abi.h"
#include "wasmos/coroutine_native.h"

/* Note: this header does NOT include wasmos_cast.h and uses raw integer<->pointer
 * double-casts (with NOLINT below) rather than ptr_cast/addr_cast. It is consumed
 * by the native libsys Zig module (c_abi.zig @cImport), whose translate-c build
 * does not carry the libc include path, so wasmos_cast.h is unreachable there. */

#ifdef __cplusplus
extern "C" {
#endif

void wasmos_sys_ipc_pack_name16_native(const uint8_t* name, uint32_t name_len,
                                       uint32_t out_args[4]);
void wasmos_sys_ipc_unpack_name16_native(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                                         uint8_t* out, uint32_t out_len);
void wasmos_sys_ipc_recv_loop_native(wasmos_driver_api_t* api, uint32_t receiver_endpoint);

#define WASMOS_SYS_NATIVE_INTENT_MAX 16u
#define WASMOS_SYS_NATIVE_HANDLER_MAX 24u
#define WASMOS_SYS_NATIVE_ENDPOINT_NONE 0xFFFFFFFFu

enum {
    WASMOS_SYS_RANDOM_STATUS_OK = 0,
    WASMOS_SYS_RANDOM_STATUS_INVALID = HRNG_STATUS_INVALID,
    WASMOS_SYS_RANDOM_STATUS_NOT_READY = HRNG_STATUS_NOT_READY,
    WASMOS_SYS_RANDOM_STATUS_IO = HRNG_STATUS_IO_ERROR,
    WASMOS_SYS_RANDOM_STATUS_PROTOCOL = -6
};

/* Pending request awaiting a reply matched by request_id. */
typedef struct {
    uint8_t in_use;
    uint32_t request_id;
    void (*on_resolve)(void* user, const nd_ipc_message_t* msg);
    void* user;
} wasmos_sys_native_intent_t;

/* Registered handler for a specific IPC message type. */
typedef struct {
    uint8_t in_use;
    uint32_t msg_type;
    void (*on_message)(void* user, const nd_ipc_message_t* msg);
    void* user;
} wasmos_sys_native_handler_t;

/* Event loop state for native drivers: intent table for request/reply matching
 * and handler table for unsolicited message dispatch. */
typedef struct {
    wasmos_driver_api_t* api;
    uint32_t receiver_endpoint;
    uint32_t next_request_id;
    void (*default_on_message)(void* user, const nd_ipc_message_t* msg);
    void* default_user;
    wasmos_sys_native_intent_t intents[WASMOS_SYS_NATIVE_INTENT_MAX];
    wasmos_sys_native_handler_t handlers[WASMOS_SYS_NATIVE_HANDLER_MAX];
} wasmos_sys_native_event_loop_t;

typedef struct wasmos_sys_native_random_request wasmos_sys_native_random_request_t;
typedef void (*wasmos_sys_native_random_complete_fn)(void* user, int32_t status);

/* Caller-owned bridge between one non-blocking IPC request and a local future.
 * reply is copied before the future resolves, so its address remains valid for
 * the caller's operation lifetime. A reply_status callback returns zero to
 * resolve or a negative protocol status to reject. */
typedef int32_t (*wasmos_sys_native_ipc_future_reply_status_fn)(void* user,
                                                                const nd_ipc_message_t* reply);

typedef struct {
    wasmos_future_t future;
    wasmos_promise_t promise;
    wasmos_sys_native_event_loop_t* loop;
    nd_ipc_message_t reply;
    wasmos_sys_native_ipc_future_reply_status_fn reply_status;
    void* user;
    uint32_t request_id;
    uint8_t active;
} wasmos_sys_native_ipc_future_t;

typedef int32_t (*wasmos_sys_native_service_main_fn)(wasmos_driver_api_t* api,
                                                     wasmos_native_coroutine_runtime_t* runtime,
                                                     void* user);

/* Optional idle hook. The pump calls it, on the kernel-thread stack, in place
 * of a bare sched_yield whenever the root coroutine has yielded and no other
 * coroutine ran. A service that listens on endpoints can block here (e.g. via
 * ipc_wait/ipc_select_wait) instead of yield-spinning; it must fall back to
 * sched_yield when it is not safe to block so cooperative scheduling continues.
 * Blocking here is safe (unlike inside a coroutine, whose stack the scheduler
 * rejects as an invalid suspended rsp). */
typedef void (*wasmos_sys_native_service_idle_fn)(void* user);

/* Caller-owned native service bootstrap. The loader-facing initialize() keeps
 * its ABI and delegates to service_run(), which executes main in root. */
typedef struct {
    wasmos_native_coroutine_runtime_t runtime;
    wasmos_native_coroutine_t root;
    void* root_stack;
    size_t root_stack_size;
    wasmos_driver_api_t* api;
    wasmos_sys_native_service_main_fn main;
    wasmos_sys_native_service_idle_fn idle;
    void* user;
} wasmos_sys_native_service_t;

/* A native async service defines this one global configuration and implements
 * its main callback. libsys supplies async_initialize as the ELF entry point. */
typedef struct {
    wasmos_sys_native_service_t service;
    void* root_stack;
    size_t root_stack_size;
    wasmos_sys_native_service_main_fn main;
    wasmos_sys_native_service_idle_fn idle;
    void* user;
} wasmos_sys_native_async_service_config_t;

struct wasmos_sys_native_random_request {
    wasmos_sys_native_event_loop_t* loop;
    uint32_t hrng_endpoint;
    uint32_t buffer_id;
    uint32_t chunk_max;
    uint32_t len;
    uint32_t done;
    uint8_t* buffer;
    uint8_t* out;
    uint32_t float_word;
    float* float_out;
    wasmos_sys_native_random_complete_fn on_complete;
    void* user;
};

/* Recursive mutex state; same binary layout as wasmos_mutex_t in libc/wasm. */
typedef struct {
    volatile uint32_t owner_tid;
    volatile uint32_t recursion_depth;
} wasmos_sys_mutex_t;

#define WASMOS_SYS_MUTEX_INITIALIZER {0u, 0u}

int32_t wasmos_sys_ipc_recv_matching_native(wasmos_driver_api_t* api, uint32_t receiver_endpoint,
                                            uint32_t request_id, nd_ipc_message_t* out_message);
int32_t wasmos_sys_svc_lookup_retry_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                           uint32_t source_endpoint, const uint8_t* name,
                                           uint32_t name_len, uint32_t request_id_base,
                                           uint32_t attempts);
int32_t wasmos_sys_ipc_send_retry_native(wasmos_driver_api_t* api, uint32_t destination_endpoint,
                                         uint32_t source_endpoint, uint32_t msg_type,
                                         uint32_t request_id, uint32_t arg0, uint32_t arg1,
                                         uint32_t arg2, uint32_t arg3, uint32_t retries);
int32_t wasmos_sys_ipc_call_native(wasmos_driver_api_t* api, uint32_t source_endpoint,
                                   uint32_t destination, uint32_t request_id, uint32_t msg_type,
                                   uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                                   nd_ipc_message_t* out_message);
int32_t wasmos_sys_svc_register_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                       uint32_t source_endpoint, const uint8_t* name,
                                       uint32_t name_len, uint32_t request_id);
int32_t wasmos_sys_svc_lookup_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                     uint32_t source_endpoint, const uint8_t* name,
                                     uint32_t name_len, uint32_t request_id);

/* Class-based service discovery for native drivers — mirrors the WASM/libc
 * wrappers (wasmos_svc_register_class / _lookup_class / _subscribe_class in
 * src/libc/include/wasmos/ipc.h) and the SVC_IPC_*_CLASS_* wire ABI in
 * wasmos_driver_abi.h. Unlike the WASM path these do not use read/write
 * hostcalls: xfer_buffer_acquire hands back a mapped pointer, so the descriptor
 * and returned entries are read/written in place. source_endpoint is the
 * caller's reply/control endpoint (distinct from a live service endpoint). */
int32_t wasmos_sys_svc_register_class_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                             uint32_t source_endpoint, uint32_t service_endpoint,
                                             const uint8_t* name, uint32_t name_len,
                                             const uint8_t* class_name, uint32_t class_len,
                                             uint32_t instance, uint32_t request_id);
int32_t wasmos_sys_svc_lookup_class_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                           uint32_t source_endpoint, const uint8_t* class_name,
                                           uint32_t class_len, svc_class_entry_t* out,
                                           uint32_t max_entries, uint32_t request_id);
int32_t wasmos_sys_svc_subscribe_class_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                              uint32_t source_endpoint, uint32_t notify_endpoint,
                                              const uint8_t* class_name, uint32_t class_len,
                                              uint32_t request_id);
void wasmos_sys_byte_copy_native(uint8_t* dst, const uint8_t* src, uint32_t len);
int32_t wasmos_sys_be_u16_native(const uint8_t* data, uint32_t data_len, uint32_t off,
                                 uint16_t* out);
int32_t wasmos_sys_be_i16_native(const uint8_t* data, uint32_t data_len, uint32_t off,
                                 int16_t* out);
int32_t wasmos_sys_be_u32_native(const uint8_t* data, uint32_t data_len, uint32_t off,
                                 uint32_t* out);
int32_t wasmos_sys_find_table_native(const uint8_t* data, uint32_t data_len, const uint8_t tag[4],
                                     uint32_t* out_offset);
uint32_t wasmos_sys_pack_u16_pair_native(uint32_t a, uint32_t b);
uint32_t wasmos_sys_pack_s16_pair_native(int32_t a, int32_t b);
uint32_t wasmos_sys_hex_u32_native(uint32_t value, uint8_t* out, uint32_t out_len);
void wasmos_sys_native_event_loop_init(wasmos_sys_native_event_loop_t* loop,
                                       wasmos_driver_api_t* api, uint32_t receiver_endpoint,
                                       uint32_t request_id_base);
int32_t wasmos_sys_native_event_register(wasmos_sys_native_event_loop_t* loop, uint32_t msg_type,
                                         void (*on_message)(void* user,
                                                            const nd_ipc_message_t* msg),
                                         void* user);
int32_t wasmos_sys_native_event_set_default(wasmos_sys_native_event_loop_t* loop,
                                            void (*on_message)(void* user,
                                                               const nd_ipc_message_t* msg),
                                            void* user);
int32_t wasmos_sys_native_intent_send(wasmos_sys_native_event_loop_t* loop,
                                      uint32_t destination_endpoint, uint32_t source_endpoint,
                                      uint32_t msg_type, uint32_t arg0, uint32_t arg1,
                                      uint32_t arg2, uint32_t arg3,
                                      void (*on_resolve)(void* user, const nd_ipc_message_t* msg),
                                      void* user, uint32_t* out_request_id);
int32_t wasmos_sys_native_intent_send_with_request_id(
    wasmos_sys_native_event_loop_t* loop, uint32_t destination_endpoint, uint32_t source_endpoint,
    uint32_t request_id, uint32_t msg_type, uint32_t arg0, uint32_t arg1, uint32_t arg2,
    uint32_t arg3, void (*on_resolve)(void* user, const nd_ipc_message_t* msg), void* user);
void wasmos_sys_native_intent_cancel(wasmos_sys_native_event_loop_t* loop, uint32_t request_id);
int32_t wasmos_sys_native_event_loop_poll(wasmos_sys_native_event_loop_t* loop, uint32_t budget);
void wasmos_sys_native_ipc_future_init(wasmos_sys_native_ipc_future_t* operation,
                                       wasmos_sys_native_ipc_future_reply_status_fn reply_status,
                                       void* user);
wasmos_future_t* wasmos_sys_native_ipc_future_send(wasmos_sys_native_event_loop_t* loop,
                                                   wasmos_sys_native_ipc_future_t* operation,
                                                   uint32_t destination_endpoint,
                                                   uint32_t source_endpoint, uint32_t msg_type,
                                                   uint32_t arg0, uint32_t arg1, uint32_t arg2,
                                                   uint32_t arg3, uint32_t* out_request_id);
/* Stops local reply tracking and rejects the future. This does not cancel the
 * transport request; a late reply is discarded by its request_id. */
void wasmos_sys_native_ipc_future_cancel(wasmos_sys_native_ipc_future_t* operation, int32_t status);
void wasmos_sys_native_service_init(wasmos_sys_native_service_t* service, void* root_stack,
                                    size_t root_stack_size);
int32_t wasmos_sys_native_service_run(wasmos_sys_native_service_t* service,
                                      wasmos_driver_api_t* api,
                                      wasmos_sys_native_service_main_fn main, void* user);
int32_t wasmos_sys_native_random_bytes_async(wasmos_sys_native_event_loop_t* loop,
                                             uint32_t hrng_endpoint, uint8_t* out, uint32_t len,
                                             wasmos_sys_native_random_request_t* request,
                                             wasmos_sys_native_random_complete_fn on_complete,
                                             void* user);
int32_t wasmos_sys_native_random_int_async(wasmos_sys_native_event_loop_t* loop,
                                           uint32_t hrng_endpoint, uint32_t* out_value,
                                           wasmos_sys_native_random_request_t* request,
                                           wasmos_sys_native_random_complete_fn on_complete,
                                           void* user);
int32_t wasmos_sys_native_random_float_async(wasmos_sys_native_event_loop_t* loop,
                                             uint32_t hrng_endpoint, float* out_value,
                                             wasmos_sys_native_random_request_t* request,
                                             wasmos_sys_native_random_complete_fn on_complete,
                                             void* user);

static inline void wasmos_sys_mutex_init(wasmos_sys_mutex_t* mutex) {
    if (!mutex) {
        return;
    }
    mutex->owner_tid = 0u;
    mutex->recursion_depth = 0u;
}

static inline int32_t wasmos_sys_mutex_try_lock(wasmos_driver_api_t* api,
                                                wasmos_sys_mutex_t* mutex) {
    if (!api || !mutex || !api->mutex_try_lock) {
        return -1;
    }
    return api->mutex_try_lock((uint64_t)(uintptr_t)mutex); // NOLINT(wasmos-reinterpret-cast)
}

static inline int32_t wasmos_sys_mutex_lock(wasmos_driver_api_t* api, wasmos_sys_mutex_t* mutex) {
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

static inline int32_t wasmos_sys_mutex_unlock(wasmos_driver_api_t* api, wasmos_sys_mutex_t* mutex) {
    if (!api || !mutex || !api->mutex_unlock) {
        return -1;
    }
    return api->mutex_unlock((uint64_t)(uintptr_t)mutex); // NOLINT(wasmos-reinterpret-cast)
}

#ifdef __cplusplus
}
#endif

#endif
