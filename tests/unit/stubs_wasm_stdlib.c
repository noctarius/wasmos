/* stubs_wasm_stdlib.c - host-compilation stubs for WASM-specific symbols in
 * stdlib.c. Only needed when compiling for unit tests on the host; the real
 * WASM build uses the linker-provided __heap_base symbol. */
#include <stdint.h>

/* The same compile line defines __builtin_wasm_memory_size/_grow away to 0 and
 * -1, so on the host the heap can never grow past this base and every malloc()
 * returns NULL. Only the allocator-independent entry points -- strtol and
 * friends -- are meaningfully exercised here. */
uint8_t __heap_base = 0;
