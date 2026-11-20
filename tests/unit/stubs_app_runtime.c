/* Runtime start hooks for host tests that link wasmos_app.c only to exercise
 * its parser. The tests never start a package, so every hook reports failure;
 * a test that does reach one sees a failed start rather than a crash. */

#include "native_driver.h"
#include "wasm_driver.h"

int native_driver_start(uint32_t context_id, const uint8_t* elf_data, uint32_t elf_size,
                        const char* name, const uint32_t* init_argv, uint32_t init_argc) {
    (void)context_id;
    (void)elf_data;
    (void)elf_size;
    (void)name;
    (void)init_argv;
    (void)init_argc;
    return -1;
}

int wasm_driver_start(wasm_driver_t* driver, const wasm_driver_manifest_t* manifest,
                      uint32_t owner_context_id) {
    (void)driver;
    (void)manifest;
    (void)owner_context_id;
    return -1;
}

void wasm_driver_stop(wasm_driver_t* driver) {
    (void)driver;
}

int wasm_driver_call_unlocked(wasm_driver_t* driver, const char* export_name, uint32_t argc,
                              uint32_t* argv) {
    (void)driver;
    (void)export_name;
    (void)argc;
    (void)argv;
    return -1;
}
