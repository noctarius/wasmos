/* imports.h - WASM/native duality shim for hostcall import declarations.
 * When compiled for WASM (__wasm__) the attribute marks a symbol as an import
 * from the named module; on x86_64 native builds it expands to nothing, leaving
 * a plain extern that the native component must satisfy itself — natively built
 * drivers and services call through the wasmos_driver_api_t vtable they receive
 * at entry (wasmos_native_driver.h) or the int-0x80 syscalls in
 * wasmos/syscall_x86_64.h instead. */
#ifndef WASMOS_LIBC_IMPORTS_H
#define WASMOS_LIBC_IMPORTS_H

#include <stdint.h>

#if defined(__wasm__)
#define WASMOS_WASM_IMPORT(module_name, symbol_name)                                               \
    __attribute__((import_module(module_name), import_name(symbol_name)))
#define WASMOS_WASM_EXPORT __attribute__((visibility("default")))
#else
#define WASMOS_WASM_IMPORT(module_name, symbol_name)
#define WASMOS_WASM_EXPORT
#endif

#endif
