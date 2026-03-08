#ifndef WASMOS_WAMR_RUNTIME_H
#define WASMOS_WAMR_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WAMR_DEFAULT_STACK_SIZE
#define WAMR_DEFAULT_STACK_SIZE (64 * 1024)
#endif

#ifndef WAMR_DEFAULT_HEAP_SIZE
#define WAMR_DEFAULT_HEAP_SIZE (64 * 1024)
#endif

typedef struct wamr_module wamr_module_t;
typedef struct wamr_instance wamr_instance_t;
typedef struct {
    const char *symbol;
    void *func_ptr;
    const char *signature;
    void *attachment;
} wamr_native_symbol_t;

int wamr_runtime_init(void);
int wamr_runtime_init_with_pool(void *heap_buf, uint32_t heap_size);
void wamr_runtime_shutdown(void);

int wamr_load_module(const uint8_t *buf, uint32_t size,
                     wamr_module_t **out_module,
                     char *error_buf, uint32_t error_buf_size);

int wamr_instantiate_module(wamr_module_t *module,
                            uint32_t stack_size,
                            uint32_t heap_size,
                            wamr_instance_t **out_instance,
                            char *error_buf, uint32_t error_buf_size);

int wamr_register_natives(const char *module_name,
                          const wamr_native_symbol_t *symbols,
                          uint32_t symbol_count);

void wamr_deinstantiate_module(wamr_instance_t *instance);
void wamr_unload_module(wamr_module_t *module);

int wamr_call_function(wamr_instance_t *instance,
                       const char *func_name,
                       uint32_t argc,
                       uint32_t argv[],
                       uint32_t stack_size);

#ifdef __cplusplus
}
#endif

#endif
