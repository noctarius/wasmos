#include "wamr_runtime.h"

#if defined(WAMR_ENABLED)
#include "wasm_export.h"

int wamr_runtime_init(void) {
    return wasm_runtime_init() ? 1 : 0;
}

void wamr_runtime_shutdown(void) {
    wasm_runtime_destroy();
}

int wamr_load_module(const uint8_t *buf, uint32_t size,
                     wamr_module_t **out_module,
                     char *error_buf, uint32_t error_buf_size) {
    if (!out_module || !buf || size == 0) {
        return 0;
    }

    wasm_module_t module = wasm_runtime_load((uint8_t *)buf, size,
                                             error_buf, error_buf_size);
    if (!module) {
        return 0;
    }

    *out_module = (wamr_module_t *)module;
    return 1;
}

int wamr_instantiate_module(wamr_module_t *module,
                            uint32_t stack_size,
                            uint32_t heap_size,
                            wamr_instance_t **out_instance,
                            char *error_buf, uint32_t error_buf_size) {
    if (!out_instance || !module) {
        return 0;
    }

    uint32_t stack = stack_size ? stack_size : WAMR_DEFAULT_STACK_SIZE;
    uint32_t heap = heap_size ? heap_size : WAMR_DEFAULT_HEAP_SIZE;

    wasm_module_inst_t inst = wasm_runtime_instantiate(
        (wasm_module_t)module, stack, heap, error_buf, error_buf_size);
    if (!inst) {
        return 0;
    }

    *out_instance = (wamr_instance_t *)inst;
    return 1;
}

void wamr_deinstantiate_module(wamr_instance_t *instance) {
    if (!instance) {
        return;
    }
    wasm_runtime_deinstantiate((wasm_module_inst_t)instance);
}

void wamr_unload_module(wamr_module_t *module) {
    if (!module) {
        return;
    }
    wasm_runtime_unload((wasm_module_t)module);
}

int wamr_call_function(wamr_instance_t *instance,
                       const char *func_name,
                       uint32_t argc,
                       uint32_t argv[],
                       uint32_t stack_size) {
    if (!instance || !func_name) {
        return 0;
    }

    wasm_function_inst_t func = wasm_runtime_lookup_function(
        (wasm_module_inst_t)instance, func_name, NULL);
    if (!func) {
        return 0;
    }

    uint32_t stack = stack_size ? stack_size : WAMR_DEFAULT_STACK_SIZE;
    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(
        (wasm_module_inst_t)instance, stack);
    if (!exec_env) {
        return 0;
    }

    int ok = wasm_runtime_call_wasm(exec_env, func, argc, argv) ? 1 : 0;
    wasm_runtime_destroy_exec_env(exec_env);
    return ok;
}

#else

int wamr_runtime_init(void) {
    return 0;
}

void wamr_runtime_shutdown(void) {
}

int wamr_load_module(const uint8_t *buf, uint32_t size,
                     wamr_module_t **out_module,
                     char *error_buf, uint32_t error_buf_size) {
    (void)buf;
    (void)size;
    (void)out_module;
    (void)error_buf;
    (void)error_buf_size;
    return 0;
}

int wamr_instantiate_module(wamr_module_t *module,
                            uint32_t stack_size,
                            uint32_t heap_size,
                            wamr_instance_t **out_instance,
                            char *error_buf, uint32_t error_buf_size) {
    (void)module;
    (void)stack_size;
    (void)heap_size;
    (void)out_instance;
    (void)error_buf;
    (void)error_buf_size;
    return 0;
}

void wamr_deinstantiate_module(wamr_instance_t *instance) {
    (void)instance;
}

void wamr_unload_module(wamr_module_t *module) {
    (void)module;
}

int wamr_call_function(wamr_instance_t *instance,
                       const char *func_name,
                       uint32_t argc,
                       uint32_t argv[],
                       uint32_t stack_size) {
    (void)instance;
    (void)func_name;
    (void)argc;
    (void)argv;
    (void)stack_size;
    return 0;
}

#endif
