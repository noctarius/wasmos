/* imports.h - WASM/native duality shim for hostcall import declarations.
 * When compiled for WASM (__wasm__) symbols are declared as extern imports;
 * for x86_64 native builds they map to wasmos_native_driver.h function-pointer
 * calls via the global g_wasmos_driver_api table. */
#ifndef WASMOS_LIBC_IMPORTS_H
#define WASMOS_LIBC_IMPORTS_H

#include <stdint.h>

#if defined(__wasm__)
#define WASMOS_WASM_IMPORT(module_name, symbol_name) \
    __attribute__((import_module(module_name), import_name(symbol_name)))
#define WASMOS_WASM_EXPORT __attribute__((visibility("default")))
#else
#define WASMOS_WASM_IMPORT(module_name, symbol_name)
#define WASMOS_WASM_EXPORT
#endif

#endif
