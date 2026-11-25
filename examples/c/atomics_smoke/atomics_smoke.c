/* atomics_smoke - exercises the WebAssembly atomic loads and stores a guest
 * uses to publish data to another CPU or a device: `i32.atomic.load`,
 * `.load8_u`, `.load16_u`, `i32.atomic.store`, `.store8` and `.store16` (the
 * threads proposal's 0xFE prefix), which this module is compiled to emit
 * (-matomics, BUILD.py). In the wasm memory model they are sequentially
 * consistent, so an atomic store of an index orders every plain store before
 * it and an atomic load of it orders every plain load after it -- which is how
 * a guest publishes a ring entry. Without the feature they compile to plain
 * loads and stores and order nothing on a weakly ordered CPU.
 *
 * Without arguments it round-trips a value through each width and prints
 * "[test] atomics ok" when every loaded value is the one stored. With the
 * argument `misaligned` it performs a 4-byte atomic load from an address that
 * is not 4-byte aligned, which the threads proposal requires to trap; reaching
 * the line after it is a failure and prints so. */
#include <stdint.h>
#include "stdio.h"

static uint32_t g_word;
static uint16_t g_half;
static uint8_t g_byte;
static uint8_t g_bytes[8] __attribute__((aligned(4)));

static int has_arg(int argc, char** argv, const char* want) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* w = want;
        while (*a && *a == *w) {
            ++a;
            ++w;
        }
        if (*a == '\0' && *w == '\0') {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (has_arg(argc, argv, "misaligned")) {
        uint32_t* p = (uint32_t*)(uintptr_t)((uintptr_t)g_bytes + 1u);
        uint32_t v = __atomic_load_n(p, __ATOMIC_SEQ_CST);
        printf("[test] atomics misaligned load did not trap (%u)\n", (unsigned)v);
        return 1;
    }
    __atomic_store_n(&g_word, 0x5eed1234u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_half, (uint16_t)0xbeefu, __ATOMIC_RELEASE);
    __atomic_store_n(&g_byte, (uint8_t)0xa5u, __ATOMIC_RELEASE);
    uint32_t w = __atomic_load_n(&g_word, __ATOMIC_SEQ_CST);
    uint16_t h = __atomic_load_n(&g_half, __ATOMIC_ACQUIRE);
    uint8_t c = __atomic_load_n(&g_byte, __ATOMIC_ACQUIRE);
    if (w != 0x5eed1234u || h != 0xbeefu || c != 0xa5u) {
        printf("[test] atomics FAILED: word=%x half=%x byte=%x\n",
               (unsigned)w,
               (unsigned)h,
               (unsigned)c);
        return 1;
    }
    printf("[test] atomics ok\n");
    return 0;
}
