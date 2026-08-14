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

/* Bind the native slab allocator (heap_native.c) to the driver_api page hooks.
 * Call once at service startup before the first malloc/free/calloc/realloc. */
void wasmos_native_heap_init(wasmos_driver_api_t* api);

/* Pack the first min(name_len, 16) bytes of `name` into four IPC args (4 bytes
 * each, little-endian), zero-filling the rest. A longer name is TRUNCATED, and
 * a name that exactly fills 16 bytes carries no terminator. Unlike the WASM
 * wasmos_sys_ipc_pack_name16() the length is explicit, so embedded NULs are
 * preserved; a NULL name yields four zero args. */
void wasmos_sys_ipc_pack_name16_native(const uint8_t* name, uint32_t name_len,
                                       uint32_t out_args[4]);
/* Inverse of pack_name16_native(): writes at most out_len-1 bytes plus a NUL,
 * stopping early at the first zero byte. Does nothing for a NULL out or a zero
 * out_len. */
void wasmos_sys_ipc_unpack_name16_native(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                                         uint8_t* out, uint32_t out_len);
/* Park a native service that has reached a terminal state: receives on
 * `receiver_endpoint` forever and DISCARDS everything, yielding whenever the
 * queue is empty. Does not return, except immediately when the api table lacks
 * ipc_recv or sched_current_pid. This yield-spins rather than blocking. */
void wasmos_sys_ipc_recv_loop_native(wasmos_driver_api_t* api, uint32_t receiver_endpoint);

/* Fixed capacities of a native event loop's two tables, both embedded by value
 * in wasmos_sys_native_event_loop_t. INTENT_MAX bounds the replies outstanding
 * at once, HANDLER_MAX the distinct message types with a handler; HANDLER_MAX
 * is larger than the WASM variant's because native drivers multiplex more
 * protocols on one endpoint. */
#define WASMOS_SYS_NATIVE_INTENT_MAX 16u
#define WASMOS_SYS_NATIVE_HANDLER_MAX 24u
/* Sentinel for "no endpoint" in the unsigned native endpoint space, which has
 * no negative values to spare (the WASM variant uses -1 instead). */
#define WASMOS_SYS_NATIVE_ENDPOINT_NONE 0xFFFFFFFFu

/* Random-helper statuses are the packed hrng domain in abi/errors.yaml:
 * WASMOS_ERR_NONE (0) on success, else a negative WASMOS_ERR_HRNG_*. */

/* Pending request awaiting a reply matched by request_id. The slot is freed
 * and cleared before on_resolve runs, so the callback may issue a new intent
 * through the same table. */
typedef struct {
    uint8_t in_use;
    uint32_t request_id;
    void (*on_resolve)(void* user, const nd_ipc_message_t* msg);
    void* user;
} wasmos_sys_native_intent_t;

/* Registered handler for a specific IPC message type; unlike an intent it
 * stays registered across deliveries. */
typedef struct {
    uint8_t in_use;
    uint32_t msg_type;
    void (*on_message)(void* user, const nd_ipc_message_t* msg);
    void* user;
} wasmos_sys_native_handler_t;

/* Event loop state for native drivers: intent table for request/reply matching
 * and handler table for unsolicited message dispatch. Caller-owned; `api` is
 * the borrowed driver table every operation goes through, and next_request_id
 * is the per-loop counter intent_send() draws from, so concurrent loops in one
 * service need disjoint ranges. Owns no select-set: poll() never blocks. */
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
/* Runs once per entropy request, with the packed hrng status (0 on success).
 * The request record and its destination buffer are the caller's again once it
 * returns. */
typedef void (*wasmos_sys_native_random_complete_fn)(void* user, int32_t status);

/* Caller-owned bridge between one non-blocking IPC request and a local future.
 * reply is copied before the future resolves, so its address remains valid for
 * the caller's operation lifetime. A reply_status callback returns zero to
 * resolve; any other value rejects, and a non-negative one is normalised to -1
 * because a future rejects only with a negative status. */
typedef int32_t (*wasmos_sys_native_ipc_future_reply_status_fn)(void* user,
                                                                const nd_ipc_message_t* reply);

/* `active` is set between a successful send and the reply (or a cancel);
 * `reply` holds the last delivered message and is the value the future
 * resolves with. */
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

/* Body of a native service, run as the root coroutine so it may await futures
 * and yield. `runtime` is the service's coroutine runtime, for spawning further
 * coroutines. Its return value becomes the service's exit status. */
typedef int32_t (*wasmos_sys_native_service_main_fn)(wasmos_driver_api_t* api,
                                                     wasmos_native_coroutine_runtime_t* runtime,
                                                     void* user);

/* Optional idle hook. wasmos_sys_native_service_run() calls it once per pump
 * iteration, on the kernel-thread stack, in place of a bare sched_yield, for as
 * long as the root coroutine is alive. A service that listens on endpoints can
 * block here (e.g. via ipc_wait/ipc_select_wait) instead of yield-spinning; it
 * must fall back to sched_yield when other coroutines are runnable
 * (wasmos_native_coroutine_runtime_has_ready()) or blocking is otherwise unsafe,
 * so cooperative scheduling continues. Blocking here is safe, unlike inside a
 * coroutine, whose stack the scheduler rejects as an invalid suspended rsp. */
typedef void (*wasmos_sys_native_service_idle_fn)(void* user);

/* Caller-owned native service bootstrap. The loader-facing initialize() keeps
 * its ABI and delegates to service_run(), which executes main in root.
 * root_stack/root_stack_size are the borrowed stack the root coroutine runs on
 * (at least 1024 bytes, and in practice sized for the whole service's deepest
 * call chain); everything else is filled in by init()/run(). */
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
 * its main callback. libsys supplies async_initialize as the ELF entry point.
 * The service must be named `wasmos_async_service` for that entry to find it,
 * and must set root_stack, root_stack_size and main; idle and user are
 * optional. The embedded `service` is scratch state the entry initialises. */
typedef struct {
    wasmos_sys_native_service_t service;
    void* root_stack;
    size_t root_stack_size;
    wasmos_sys_native_service_main_fn main;
    wasmos_sys_native_service_idle_fn idle;
    void* user;
} wasmos_sys_native_async_service_config_t;

/* Caller-owned state of one entropy request. The hrng service caps a single
 * reply at HRNG_MAX_BYTES_PER_REQ, so `len` bytes are gathered as a chain of
 * `chunk_max`-sized requests, with `done` counting the bytes already copied
 * from the mapped transfer buffer `buffer` into `out`. float_out is set only by
 * random_float_async(), which draws into float_word and converts on completion.
 * Fields are runtime state: initialise via one of the *_async starters, not by
 * hand. */
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

/* Static-initialiser form: an unlocked mutex. */
#define WASMOS_SYS_MUTEX_INITIALIZER {0u, 0u}

/* Receive on `receiver_endpoint` until a message carrying `request_id` arrives,
 * write it to out_message and return 0; returns -1 on a receive error or a
 * missing api entry. Messages with any other request_id are consumed and
 * DISCARDED, so this belongs on a private reply endpoint, never on a live
 * service endpoint. Blocks by yield-spinning: an empty queue yields and retries
 * forever, so a reply that never comes hangs the caller. */
int32_t wasmos_sys_ipc_recv_matching_native(wasmos_driver_api_t* api, uint32_t receiver_endpoint,
                                            uint32_t request_id, nd_ipc_message_t* out_message);
/* svc_lookup_native() retried up to `attempts` times (0 means one attempt),
 * yielding between tries so a service that has not registered yet gets a chance
 * to run. Each attempt consumes one request id from request_id_base upwards.
 * Returns the endpoint id (>= 0) or -1 once the attempts are exhausted. */
int32_t wasmos_sys_svc_lookup_retry_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                           uint32_t source_endpoint, const uint8_t* name,
                                           uint32_t name_len, uint32_t request_id_base,
                                           uint32_t attempts);
/* Send, retrying up to `retries` times (0 means one attempt) with a yield
 * between tries. Returns 0 on success, the send status unchanged for any
 * failure other than a full destination queue, -3 (IPC_ERR_FULL) once the
 * retries are used up, and -1 when the api table lacks ipc_send or
 * sched_current_pid. Does not wait for a reply. */
int32_t wasmos_sys_ipc_send_retry_native(wasmos_driver_api_t* api, uint32_t destination_endpoint,
                                         uint32_t source_endpoint, uint32_t msg_type,
                                         uint32_t request_id, uint32_t arg0, uint32_t arg1,
                                         uint32_t arg2, uint32_t arg3, uint32_t retries);
int32_t wasmos_sys_ipc_call_native(wasmos_driver_api_t* api, uint32_t source_endpoint,
                                   uint32_t destination, uint32_t request_id, uint32_t msg_type,
                                   uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                                   nd_ipc_message_t* out_message);

/* Resolve `hostname` (length `hostname_len`, no NUL required) to an IPv4 address
 * through net.stack (NET_IPC_RESOLVE) - the native mirror of the WASM/libc
 * wasmos_net_resolve() helper (src/libc/include/wasmos/net.h). Synchronous from
 * the caller's view: it borrows the name to net-stack read-only and blocks on
 * `source_endpoint` for the reply via wasmos_sys_ipc_call_native(); net-stack
 * itself defers the reply until lwIP's DNS callback fires and never blocks. Use
 * only from native contexts that may block (not a single-threaded reactor - such
 * a service should issue NET_IPC_RESOLVE as an async intent instead). On success
 * returns 0 and writes the resolved address as a network-order IPv4 word (octet a
 * in the low byte) to *out_addr_no; returns a negative value on any failure. */
int32_t wasmos_sys_net_resolve_native(wasmos_driver_api_t* api, uint32_t source_endpoint,
                                      uint32_t stack_endpoint, const char* hostname,
                                      uint32_t hostname_len, uint32_t request_id,
                                      uint32_t* out_addr_no);
/* Register the calling context under `name` (packed into the four IPC args, so
 * at most 16 bytes; longer names are truncated) and block on `source_endpoint`
 * for the SVC_IPC_REGISTER_RESP. `source_endpoint` is both the endpoint being
 * registered and the one the reply lands on. Returns the reply's arg0, which
 * the process manager always sets to 0, or -1 when the api table is unusable,
 * the send fails, or the reply is not a register response. */
int32_t wasmos_sys_svc_register_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                       uint32_t source_endpoint, const uint8_t* name,
                                       uint32_t name_len, uint32_t request_id);
/* Resolve `name` (same 16-byte packing) to a service endpoint, blocking on
 * `source_endpoint` for the reply. Returns the endpoint id (>= 0), or -1 when
 * the api table is unusable, the send fails, the reply is not a lookup
 * response, or the name is unknown (which the process manager reports as
 * 0xFFFFFFFF). */
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
/* Forward byte copy; the ranges must not overlap. A NULL dst or src makes it a
 * no-op, and len is not bounds-checked against either buffer. */
void wasmos_sys_byte_copy_native(uint8_t* dst, const uint8_t* src, uint32_t len);
/* Bounds-checked big-endian loads from a byte buffer, for parsing wire and
 * font formats without unaligned access. Each reads its width at `off` and
 * returns 0 having written *out, or -1 when data/out is NULL or the field would
 * run past data_len. `off + width` is computed in uint32 arithmetic, so an
 * offset within 4 bytes of UINT32_MAX wraps and the bound is not enforced. */
int32_t wasmos_sys_be_u16_native(const uint8_t* data, uint32_t data_len, uint32_t off,
                                 uint16_t* out);
int32_t wasmos_sys_be_i16_native(const uint8_t* data, uint32_t data_len, uint32_t off,
                                 int16_t* out);
int32_t wasmos_sys_be_u32_native(const uint8_t* data, uint32_t data_len, uint32_t off,
                                 uint32_t* out);
int32_t wasmos_sys_find_table_native(const uint8_t* data, uint32_t data_len, const uint8_t tag[4],
                                     uint32_t* out_offset);
/* Pack two values into the halves of one IPC arg: a (or its low 16 bits) in
 * bits 0-15, b in bits 16-31. The signed form truncates each operand to int16
 * first, so unpacking must sign-extend. */
uint32_t wasmos_sys_pack_u16_pair_native(uint32_t a, uint32_t b);
uint32_t wasmos_sys_pack_s16_pair_native(int32_t a, int32_t b);
/* Format `value` as "0x" plus exactly 8 lowercase hex digits, NUL-terminated.
 * Needs out_len >= 11 and returns the 10 characters written, or 0 (writing
 * nothing) when out is NULL or the buffer is too small. */
uint32_t wasmos_sys_hex_u32_native(uint32_t value, uint8_t* out, uint32_t out_len);
/* Bind a caller-owned loop to `api` and `receiver_endpoint` and clear both
 * tables. `request_id_base` seeds the counter intent_send() draws from. Ignores
 * a NULL loop; a NULL api is stored and makes every later loop operation fail
 * rather than fault. */
void wasmos_sys_native_event_loop_init(wasmos_sys_native_event_loop_t* loop,
                                       wasmos_driver_api_t* api, uint32_t receiver_endpoint,
                                       uint32_t request_id_base);
/* Register (or replace) the handler for `msg_type`. Returns 0 on success, -1
 * for a NULL loop or callback, or when all WASMOS_SYS_NATIVE_HANDLER_MAX slots
 * are taken by other types. Registering the same type again rewrites its
 * callback and user pointer rather than consuming a second slot. */
int32_t wasmos_sys_native_event_register(wasmos_sys_native_event_loop_t* loop, uint32_t msg_type,
                                         void (*on_message)(void* user,
                                                            const nd_ipc_message_t* msg),
                                         void* user);
/* Install the fallback handler for messages that match no intent and no typed
 * handler. Returns 0 on success, -1 for a NULL loop or callback. Replaces any
 * previous default. */
int32_t wasmos_sys_native_event_set_default(wasmos_sys_native_event_loop_t* loop,
                                            void (*on_message)(void* user,
                                                               const nd_ipc_message_t* msg),
                                            void* user);
/* Send a request and register `on_resolve` for the reply that carries the
 * allocated request_id, reported through out_request_id (optional).
 * Non-blocking: the reply is delivered from event_loop_poll(). Returns 0 on
 * success, -1 for a NULL loop/callback, an unusable api table, or an exhausted
 * intent table, and the transport's own status when the send itself fails - so
 * unlike the WASM variant a failure is not always -1. The request_id counter
 * advances even on a failed send, and no intent slot is consumed. */
int32_t wasmos_sys_native_intent_send(wasmos_sys_native_event_loop_t* loop,
                                      uint32_t destination_endpoint, uint32_t source_endpoint,
                                      uint32_t msg_type, uint32_t arg0, uint32_t arg1,
                                      uint32_t arg2, uint32_t arg3,
                                      void (*on_resolve)(void* user, const nd_ipc_message_t* msg),
                                      void* user, uint32_t* out_request_id);
/* intent_send() with a caller-chosen `request_id`, for protocols where the id
 * is derived rather than allocated. It must be non-zero and must not already be
 * pending on this loop; the loop's own counter is left untouched, so a caller
 * mixing both forms is responsible for keeping the ranges disjoint. Returns 0
 * on success, -1 on a NULL loop/callback, an unusable api table, a zero or
 * duplicate id, or a full intent table, and the transport status when the send
 * fails. */
int32_t wasmos_sys_native_intent_send_with_request_id(
    wasmos_sys_native_event_loop_t* loop, uint32_t destination_endpoint, uint32_t source_endpoint,
    uint32_t request_id, uint32_t msg_type, uint32_t arg0, uint32_t arg1, uint32_t arg2,
    uint32_t arg3, void (*on_resolve)(void* user, const nd_ipc_message_t* msg), void* user);
/* Drop local tracking of a pending request so its callback can no longer run.
 * The request itself is not recalled: a reply that arrives afterwards matches
 * no intent and falls through to the type handler or the default handler.
 * Silently does nothing for a NULL loop or an id that is not pending. */
void wasmos_sys_native_intent_cancel(wasmos_sys_native_event_loop_t* loop, uint32_t request_id);
/* Dispatch at most `budget` queued messages (0 is treated as 1) and return how
 * many ran, or -1 when the loop has no usable driver_api. Never blocks: unlike
 * the WASM variant it owns no select-set, so a service that must sleep at idle
 * does so in its idle hook.
 * Each message goes to the matching intent, else the handler for its type, else
 * the default handler; a message matching none of those is still counted as
 * handled and dropped. An empty queue ends the drain early. A receive error
 * mid-drain also returns -1, discarding the count of what was already
 * dispatched, so the return is a progress hint and not an exact tally. */
int32_t wasmos_sys_native_event_loop_poll(wasmos_sys_native_event_loop_t* loop, uint32_t budget);
/* Zero the record and put its future back in the pending state, with
 * `reply_status` (optional; NULL accepts every reply) as the resolve/reject
 * decision. Required before the first send and before every reuse. */
void wasmos_sys_native_ipc_future_init(wasmos_sys_native_ipc_future_t* operation,
                                       wasmos_sys_native_ipc_future_reply_status_fn reply_status,
                                       void* user);
/* Issue the request as a loop intent and return the record's future, or NULL
 * when loop/operation is NULL, the record is already in flight, or its future
 * has already settled (call init() again to reuse it). A failed send returns
 * the future ALREADY REJECTED rather than NULL, so a non-NULL return does not
 * imply the request is on the wire. out_request_id (optional) receives the
 * allocated id, or 0 when nothing was sent. */
wasmos_future_t* wasmos_sys_native_ipc_future_send(wasmos_sys_native_event_loop_t* loop,
                                                   wasmos_sys_native_ipc_future_t* operation,
                                                   uint32_t destination_endpoint,
                                                   uint32_t source_endpoint, uint32_t msg_type,
                                                   uint32_t arg0, uint32_t arg1, uint32_t arg2,
                                                   uint32_t arg3, uint32_t* out_request_id);
/* Stops local reply tracking and rejects the future. This does not cancel the
 * transport request; a late reply then matches no intent and falls through
 * to the loop's type handler or default handler, exactly like the WASM variant
 * (wasmos_sys_wasm_ipc_future_cancel). */
void wasmos_sys_native_ipc_future_cancel(wasmos_sys_native_ipc_future_t* operation, int32_t status);
/* Zero a caller-owned service record and record the root coroutine's stack.
 * The stack is borrowed and must stay mapped for the whole run. Ignores a NULL
 * service. */
void wasmos_sys_native_service_init(wasmos_sys_native_service_t* service, void* root_stack,
                                    size_t root_stack_size);
/* Start `main` as the root coroutine and pump it until it exits: each pass runs
 * one ready coroutine and then, while the root is alive, calls the service's
 * idle hook (or sched_yield when there is none) on the kernel-thread stack.
 * Returns main's return value, or -1 for a NULL argument, a root stack below
 * 1024 bytes, a root coroutine that will not start, or a runtime error. Blocks
 * for the service's entire lifetime; `api` and `user` are borrowed for that
 * long. Set service->idle before calling if the service should sleep at idle. */
int32_t wasmos_sys_native_service_run(wasmos_sys_native_service_t* service,
                                      wasmos_driver_api_t* api,
                                      wasmos_sys_native_service_main_fn main, void* user);
/* Start a non-blocking entropy request. Callers retain `request` and `out`
 * until `on_complete` runs, and drive completion through
 * wasmos_sys_native_event_loop_poll(). Gathers `len` bytes into `out` as chunks
 * of at most HRNG_MAX_BYTES_PER_REQ. Returns 0 once the first chunk is on the
 * wire, after which the outcome is reported only through on_complete; a
 * negative packed WASMOS_ERR_HRNG_* means nothing was started and on_complete
 * will NOT run: INVALID for a rejected argument, NOT_READY when no transfer
 * buffer could be acquired or granted, IO_ERROR when the first request could
 * not be sent. */
int32_t wasmos_sys_native_random_bytes_async(wasmos_sys_native_event_loop_t* loop,
                                             uint32_t hrng_endpoint, uint8_t* out, uint32_t len,
                                             wasmos_sys_native_random_request_t* request,
                                             wasmos_sys_native_random_complete_fn on_complete,
                                             void* user);
/* random_bytes_async() for a single uint32: draws sizeof(uint32_t) bytes
 * straight into *out_value, whose byte order is whatever the entropy source
 * produced. Same return convention as random_bytes_async(). */
int32_t wasmos_sys_native_random_int_async(wasmos_sys_native_event_loop_t* loop,
                                           uint32_t hrng_endpoint, uint32_t* out_value,
                                           wasmos_sys_native_random_request_t* request,
                                           wasmos_sys_native_random_complete_fn on_complete,
                                           void* user);
/* random_bytes_async() for a float uniformly distributed in [0, 1): the drawn
 * word is buffered in the request and its top 24 bits are converted into
 * *out_value only when the request completes successfully. Same return
 * convention as random_bytes_async(); *out_value is untouched on failure. */
int32_t wasmos_sys_native_random_float_async(wasmos_sys_native_event_loop_t* loop,
                                             uint32_t hrng_endpoint, float* out_value,
                                             wasmos_sys_native_random_request_t* request,
                                             wasmos_sys_native_random_complete_fn on_complete,
                                             void* user);

/* Reset a mutex to unlocked. Only valid before first use or when the caller
 * knows nobody holds it; it does not release a held lock, it forgets it.
 * NULL is ignored. */
static inline void wasmos_sys_mutex_init(wasmos_sys_mutex_t* mutex) {
    if (!mutex) {
        return;
    }
    mutex->owner_tid = 0u;
    mutex->recursion_depth = 0u;
}

/* Acquire without waiting: returns 0 when the lock is held by this thread
 * (including a recursive re-entry), 1 when another thread holds it, and -1 for
 * a NULL argument, an api table without mutex_try_lock, or a rejected call. */
static inline int32_t wasmos_sys_mutex_try_lock(wasmos_driver_api_t* api,
                                                wasmos_sys_mutex_t* mutex) {
    if (!api || !mutex || !api->mutex_try_lock) {
        return -1;
    }
    return api->mutex_try_lock((uint64_t)(uintptr_t)mutex); // NOLINT(wasmos-reinterpret-cast)
}

/* Acquire, yielding between attempts until it succeeds. Returns 0 once held,
 * or -1 for a NULL argument, an api table without sched_yield, or a rejected
 * call; it never returns 1. Contention is a yield-spin, not a sleep. Must not
 * be called from inside a coroutine that another coroutine on the same worker
 * would have to run to release the lock. */
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

/* Release one level of ownership; the mutex only becomes free when the
 * recursion depth reaches zero. Returns 0 on success, or <0 for a NULL
 * argument, an api table without mutex_unlock, or a caller that is not the
 * owner. */
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
