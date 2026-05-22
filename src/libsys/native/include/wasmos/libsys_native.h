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
#define WASMOS_SYS_NATIVE_HANDLER_MAX 16u

typedef struct {
    uint8_t in_use;
    uint32_t request_id;
    void (*on_resolve)(void *user, const nd_ipc_message_t *msg);
    void *user;
} wasmos_sys_native_intent_t;

typedef struct {
    uint8_t in_use;
    uint32_t msg_type;
    void (*on_message)(void *user, const nd_ipc_message_t *msg);
    void *user;
} wasmos_sys_native_handler_t;

typedef struct {
    wasmos_driver_api_t *api;
    uint32_t receiver_endpoint;
    uint32_t next_request_id;
    wasmos_sys_native_intent_t intents[WASMOS_SYS_NATIVE_INTENT_MAX];
    wasmos_sys_native_handler_t handlers[WASMOS_SYS_NATIVE_HANDLER_MAX];
} wasmos_sys_native_event_loop_t;

int32_t wasmos_sys_ipc_recv_matching_native(wasmos_driver_api_t *api, uint32_t receiver_endpoint, uint32_t request_id, nd_ipc_message_t *out_message);
int32_t wasmos_sys_svc_lookup_retry_native(wasmos_driver_api_t *api, uint32_t proc_endpoint, uint32_t source_endpoint, const uint8_t *name, uint32_t name_len, uint32_t request_id_base, uint32_t attempts);
int32_t wasmos_sys_ipc_send_retry_native(wasmos_driver_api_t *api, uint32_t destination_endpoint, uint32_t source_endpoint, uint32_t msg_type, uint32_t request_id, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t retries);
int32_t wasmos_sys_ipc_call_native(wasmos_driver_api_t *api, uint32_t source_endpoint, uint32_t destination, uint32_t request_id, uint32_t msg_type, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, nd_ipc_message_t *out_message);
int32_t wasmos_sys_svc_register_native(wasmos_driver_api_t *api, uint32_t proc_endpoint, uint32_t source_endpoint, const uint8_t *name, uint32_t name_len, uint32_t request_id);
int32_t wasmos_sys_svc_lookup_native(wasmos_driver_api_t *api, uint32_t proc_endpoint, uint32_t source_endpoint, const uint8_t *name, uint32_t name_len, uint32_t request_id);
int32_t wasmos_sys_buffer_copy_from_native(wasmos_driver_api_t *api, uint32_t kind, uint32_t source_endpoint, uint32_t borrow_flags, void *dst, int32_t len, int32_t offset);
int32_t wasmos_sys_buffer_write_to_native(wasmos_driver_api_t *api, uint32_t kind, uint32_t source_endpoint, uint32_t borrow_flags, const void *src, int32_t len, int32_t offset);
int32_t wasmos_sys_fs_read_path_native(wasmos_driver_api_t *api, uint32_t fs_endpoint, uint32_t reply_endpoint, uint32_t request_id, const uint8_t *path, uint32_t path_len, uint8_t *out_text, int32_t out_text_len);
void wasmos_sys_byte_copy_native(uint8_t *dst, const uint8_t *src, uint32_t len);
int32_t wasmos_sys_be_u16_native(const uint8_t *data, uint32_t data_len, uint32_t off, uint16_t *out);
int32_t wasmos_sys_be_i16_native(const uint8_t *data, uint32_t data_len, uint32_t off, int16_t *out);
int32_t wasmos_sys_be_u32_native(const uint8_t *data, uint32_t data_len, uint32_t off, uint32_t *out);
int32_t wasmos_sys_find_table_native(const uint8_t *data, uint32_t data_len, const uint8_t tag[4], uint32_t *out_offset);
uint32_t wasmos_sys_pack_u16_pair_native(uint32_t a, uint32_t b);
uint32_t wasmos_sys_pack_s16_pair_native(int32_t a, int32_t b);
void wasmos_sys_native_event_loop_init(wasmos_sys_native_event_loop_t *loop,
                                       wasmos_driver_api_t *api,
                                       uint32_t receiver_endpoint,
                                       uint32_t request_id_base);
int32_t wasmos_sys_native_event_register(wasmos_sys_native_event_loop_t *loop,
                                         uint32_t msg_type,
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
int32_t wasmos_sys_native_event_loop_poll(wasmos_sys_native_event_loop_t *loop, uint32_t budget);

#ifdef __cplusplus
}
#endif

#endif
