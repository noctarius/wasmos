#ifndef WASMOS_LIBSYS_ZIG_EXPORTS_H
#define WASMOS_LIBSYS_ZIG_EXPORTS_H

#include <stdint.h>
#include "wasmos_native_driver.h"
#include "wasmos_driver_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

void wasmos_sys_ipc_pack_name16_zig(const uint8_t *name, uint32_t name_len, uint32_t out_args[4]);
void wasmos_sys_ipc_unpack_name16_zig(uint32_t arg0,
                                      uint32_t arg1,
                                      uint32_t arg2,
                                      uint32_t arg3,
                                      uint8_t *out,
                                      uint32_t out_len);
void wasmos_sys_ipc_recv_loop_zig(wasmos_driver_api_t *api, uint32_t receiver_endpoint);

int32_t wasmos_sys_ipc_recv_matching_zig(wasmos_driver_api_t *api,
                                         uint32_t receiver_endpoint,
                                         uint32_t request_id,
                                         nd_ipc_message_t *out_message);

int32_t wasmos_sys_ipc_call_zig(wasmos_driver_api_t *api,
                                uint32_t source_endpoint,
                                uint32_t destination,
                                uint32_t request_id,
                                uint32_t msg_type,
                                uint32_t arg0,
                                uint32_t arg1,
                                uint32_t arg2,
                                uint32_t arg3,
                                nd_ipc_message_t *out_message);

int32_t wasmos_sys_svc_register_zig(wasmos_driver_api_t *api,
                                    uint32_t proc_endpoint,
                                    uint32_t source_endpoint,
                                    const uint8_t *name,
                                    uint32_t name_len,
                                    uint32_t request_id);

int32_t wasmos_sys_svc_lookup_zig(wasmos_driver_api_t *api,
                                  uint32_t proc_endpoint,
                                  uint32_t source_endpoint,
                                  const uint8_t *name,
                                  uint32_t name_len,
                                  uint32_t request_id);

int32_t wasmos_sys_svc_lookup_retry_zig(wasmos_driver_api_t *api,
                                        uint32_t proc_endpoint,
                                        uint32_t source_endpoint,
                                        const uint8_t *name,
                                        uint32_t name_len,
                                        uint32_t request_id_base,
                                        uint32_t attempts);

int32_t wasmos_sys_ipc_send_retry_zig(wasmos_driver_api_t *api,
                                      uint32_t destination_endpoint,
                                      uint32_t source_endpoint,
                                      uint32_t msg_type,
                                      uint32_t request_id,
                                      uint32_t arg0,
                                      uint32_t arg1,
                                      uint32_t arg2,
                                      uint32_t arg3,
                                      uint32_t retries);

#ifdef __cplusplus
}
#endif

#endif
