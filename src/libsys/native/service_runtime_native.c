/* service_runtime_native.c - native loader ABI to root-coroutine bootstrap. */
#include "wasmos/libsys_native.h"

static void service_root(void* arg) {
    wasmos_sys_native_service_t* service = (wasmos_sys_native_service_t*)arg;
    int32_t result = -1;

    if (service && service->api && service->main) {
        result = service->main(service->api, &service->runtime, service->user);
    }
    wasmos_native_coroutine_exit(result);
}

void wasmos_sys_native_service_init(wasmos_sys_native_service_t* service, void* root_stack,
                                    size_t root_stack_size) {
    if (!service) {
        return;
    }
    *service = (wasmos_sys_native_service_t){0};
    service->root_stack = root_stack;
    service->root_stack_size = root_stack_size;
}

int32_t wasmos_sys_native_service_run(wasmos_sys_native_service_t* service,
                                      wasmos_driver_api_t* api,
                                      wasmos_sys_native_service_main_fn main, void* user) {
    int32_t result = -1;

    if (!service || !api || !main || !service->root_stack || service->root_stack_size < 1024u) {
        return -1;
    }
    wasmos_native_coroutine_runtime_init(&service->runtime);
    service->api = api;
    service->main = main;
    service->user = user;
    if (!wasmos_async_start(&service->runtime, &service->root, service->root_stack,
                            service->root_stack_size, service_root, service)) {
        return -1;
    }
    while (service->root.state != WASMOS_NATIVE_COROUTINE_DEAD) {
        if (wasmos_native_coroutine_run_budget(&service->runtime, 1u) < 0) {
            return -1;
        }
        if (service->root.state != WASMOS_NATIVE_COROUTINE_DEAD) {
            /* Runs on the kernel-thread stack: a service idle hook may safely
             * block here, unlike inside the root coroutine. */
            if (service->idle) {
                service->idle(service->user);
            } else if (api->sched_yield) {
                api->sched_yield();
            }
        }
    }
    if (wasmos_native_coroutine_join(&service->root, &result) != 0) {
        return -1;
    }
    return result;
}
