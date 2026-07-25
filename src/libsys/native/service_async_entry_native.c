/* service_async_entry_native.c - common ELF entry for native async services. */
#include "wasmos/libsys_native.h"

extern wasmos_sys_native_async_service_config_t wasmos_async_service;

int async_initialize(wasmos_driver_api_t* api, int module_count, int arg2, int arg3) {
    (void)module_count;
    (void)arg2;
    (void)arg3;
    wasmos_sys_native_service_init(&wasmos_async_service.service, wasmos_async_service.root_stack,
                                   wasmos_async_service.root_stack_size);
    wasmos_async_service.service.idle = wasmos_async_service.idle;
    return wasmos_sys_native_service_run(&wasmos_async_service.service, api,
                                         wasmos_async_service.main, wasmos_async_service.user);
}
