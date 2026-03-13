#include "wamr_runtime.h"

#if defined(WAMR_ENABLED)
#include <stddef.h>
#include "wasm_export.h"
#include "serial.h"
#include "wasm_exec_env.h"
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
#include "process.h"
#endif

extern volatile void *wasmos_wamr_last_native_ptr;
extern volatile uint32_t wasmos_wamr_last_native_index;
extern volatile uint32_t wasmos_wamr_native_calls;
extern volatile uint32_t wasmos_wamr_bytecode_calls;
extern volatile uint32_t wasmos_wamr_call_indirect_count;
extern volatile uint32_t wasmos_wamr_call_indirect_last_fidx;
extern volatile uint32_t wasmos_wamr_call_indirect_last_import;
extern volatile uint8_t wasmos_wamr_last_opcodes[16];
extern volatile uint32_t wasmos_wamr_last_opcodes_len;
extern volatile void *wasmos_wamr_last_code_start;
extern volatile void *wasmos_wamr_last_code_end;
extern volatile uint32_t wasmos_wamr_opcode_exec_count;
extern volatile void *wasmos_wamr_last_frame_ip;
extern volatile void *wasmos_wamr_last_func;

static void memzero8(void *dst, uint32_t size) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < size; ++i) {
        d[i] = 0;
    }
}

static void
serial_write_hex64_local(uint64_t value)
{
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
}

static void
log_exec_env_layout(const char *label, wasm_exec_env_t exec_env)
{
    if (!exec_env) {
        return;
    }
    WASMExecEnv *env = (WASMExecEnv *)exec_env;
    serial_write(label);
    serial_write(" exec_env=");
    serial_write_hex64_local((uint64_t)(uintptr_t)env);
    serial_write(label);
    serial_write(" wasm_stack.bottom=");
    serial_write_hex64_local((uint64_t)(uintptr_t)env->wasm_stack.bottom);
    serial_write(label);
    serial_write(" wasm_stack.top=");
    serial_write_hex64_local((uint64_t)(uintptr_t)env->wasm_stack.top);
    serial_write(label);
    serial_write(" wasm_stack.top_boundary=");
    serial_write_hex64_local((uint64_t)(uintptr_t)env->wasm_stack.top_boundary);
}

static void
log_opcode_bytes(void)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[4];
    uint32_t len = wasmos_wamr_last_opcodes_len;
    if (len > 8) {
        len = 8;
    }
    serial_write("[wamr] opcodes len=");
    serial_write_hex64_local((uint64_t)wasmos_wamr_last_opcodes_len);
    serial_write("[wamr] opcodes bytes=");
    for (uint32_t i = 0; i < len; ++i) {
        uint8_t b = wasmos_wamr_last_opcodes[i];
        buf[0] = hex[(b >> 4) & 0xF];
        buf[1] = hex[b & 0xF];
        buf[2] = ' ';
        buf[3] = '\0';
        serial_write(buf);
    }
    serial_write("\n");
}

int wamr_runtime_init(void) {
    static int logged = 0;
    if (!logged) {
        logged = 1;
#if WASM_ENABLE_FAST_INTERP != 0
        serial_write("[wamr] config fast_interp=1\n");
#else
        serial_write("[wamr] config fast_interp=0\n");
#endif
#if WASM_ENABLE_AOT != 0
        serial_write("[wamr] config aot=1\n");
#else
        serial_write("[wamr] config aot=0\n");
#endif
#if WASM_ENABLE_JIT != 0
        serial_write("[wamr] config jit=1\n");
#else
        serial_write("[wamr] config jit=0\n");
#endif
    }
    return wasm_runtime_init() ? 1 : 0;
}

int wamr_runtime_init_with_pool(void *heap_buf, uint32_t heap_size) {
    if (!heap_buf || heap_size == 0) {
        return 0;
    }
    static int logged = 0;
    if (!logged) {
        logged = 1;
#if WASM_ENABLE_FAST_INTERP != 0
        serial_write("[wamr] config fast_interp=1\n");
#else
        serial_write("[wamr] config fast_interp=0\n");
#endif
#if WASM_ENABLE_AOT != 0
        serial_write("[wamr] config aot=1\n");
#else
        serial_write("[wamr] config aot=0\n");
#endif
#if WASM_ENABLE_JIT != 0
        serial_write("[wamr] config jit=1\n");
#else
        serial_write("[wamr] config jit=0\n");
#endif
    }
    RuntimeInitArgs init_args;
    memzero8(&init_args, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = heap_buf;
    init_args.mem_alloc_option.pool.heap_size = heap_size;
    return wasm_runtime_full_init(&init_args) ? 1 : 0;
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

    if (error_buf && error_buf_size > 0) {
        error_buf[0] = '\0';
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

    if (error_buf && error_buf_size > 0) {
        error_buf[0] = '\0';
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

int wamr_register_natives(const char *module_name,
                          const wamr_native_symbol_t *symbols,
                          uint32_t symbol_count) {
    if (!module_name || !symbols || symbol_count == 0) {
        return 0;
    }
    return wasm_runtime_register_natives(module_name,
                                         (NativeSymbol *)symbols,
                                         symbol_count) ? 1 : 0;
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
        (wasm_module_inst_t)instance, func_name);
    if (!func) {
        return 0;
    }

    uint32_t stack = stack_size ? stack_size : WAMR_DEFAULT_STACK_SIZE;
    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(
        (wasm_module_inst_t)instance, stack);
    if (!exec_env) {
        return 0;
    }
    log_exec_env_layout("[wamr] exec_env", exec_env);

#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
    preempt_disable();
#endif
    wasmos_wamr_last_native_ptr = 0;
    wasmos_wamr_last_native_index = 0;
    wasmos_wamr_native_calls = 0;
    wasmos_wamr_bytecode_calls = 0;
    wasmos_wamr_call_indirect_count = 0;
    wasmos_wamr_call_indirect_last_fidx = 0;
    wasmos_wamr_call_indirect_last_import = 0;
    wasmos_wamr_last_code_start = 0;
    wasmos_wamr_last_code_end = 0;
    wasmos_wamr_opcode_exec_count = 0;
    wasmos_wamr_last_frame_ip = 0;
    wasmos_wamr_last_func = 0;
    int ok = wasm_runtime_call_wasm(exec_env, func, argc, argv) ? 1 : 0;
    serial_write("[wamr] last native ptr=");
    serial_write_hex64_local((uint64_t)(uintptr_t)wasmos_wamr_last_native_ptr);
    serial_write("[wamr] last native idx=");
    serial_write_hex64_local((uint64_t)wasmos_wamr_last_native_index);
    serial_write("[wamr] native call count=");
    serial_write_hex64_local((uint64_t)wasmos_wamr_native_calls);
    serial_write("[wamr] bytecode call count=");
    serial_write_hex64_local((uint64_t)wasmos_wamr_bytecode_calls);
    serial_write("[wamr] call_indirect count=");
    serial_write_hex64_local((uint64_t)wasmos_wamr_call_indirect_count);
    serial_write("[wamr] call_indirect last fidx=");
    serial_write_hex64_local((uint64_t)wasmos_wamr_call_indirect_last_fidx);
    serial_write("[wamr] call_indirect last import=");
    serial_write_hex64_local((uint64_t)wasmos_wamr_call_indirect_last_import);
    serial_write("[wamr] code start=");
    serial_write_hex64_local((uint64_t)(uintptr_t)wasmos_wamr_last_code_start);
    serial_write("[wamr] code end=");
    serial_write_hex64_local((uint64_t)(uintptr_t)wasmos_wamr_last_code_end);
    serial_write("[wamr] opcode exec count=");
    serial_write_hex64_local((uint64_t)wasmos_wamr_opcode_exec_count);
    serial_write("[wamr] last frame ip=");
    serial_write_hex64_local((uint64_t)(uintptr_t)wasmos_wamr_last_frame_ip);
    serial_write("[wamr] last func=");
    serial_write_hex64_local((uint64_t)(uintptr_t)wasmos_wamr_last_func);
    log_opcode_bytes();
    const char *exc = wasm_runtime_get_exception((wasm_module_inst_t)instance);
    if (exc) {
        serial_write("[wamr] exception ");
        serial_write(exc);
        serial_write("\n");
    }
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
    preempt_enable();
#endif
    wasm_runtime_destroy_exec_env(exec_env);
    return ok;
}

#else

int wamr_runtime_init(void) {
    return 0;
}

int wamr_runtime_init_with_pool(void *heap_buf, uint32_t heap_size) {
    (void)heap_buf;
    (void)heap_size;
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

int wamr_register_natives(const char *module_name,
                          const wamr_native_symbol_t *symbols,
                          uint32_t symbol_count) {
    (void)module_name;
    (void)symbols;
    (void)symbol_count;
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
