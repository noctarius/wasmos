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
