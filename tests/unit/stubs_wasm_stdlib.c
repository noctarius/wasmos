/* stubs_wasm_stdlib.c - host-compilation stubs for WASM-specific symbols in
 * stdlib.c. Only needed when compiling for unit tests on the host; the real
 * WASM build uses the linker-provided __heap_base symbol. */
#include <stdint.h>

/* Placeholder heap base — heap_init/malloc are not under test; strtol is. */
uint8_t __heap_base = 0;
