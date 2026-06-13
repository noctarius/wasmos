/* warp/shim.cpp - Freestanding C++ ABI shim and memory-region bindings for WARP.
 *
 * TODO: implement:
 *   - operator new / operator delete wrappers around the kernel slab allocator
 *   - __cxa_pure_virtual / __cxa_atexit / __dso_handle stubs
 *   - __cxa_guard_acquire / __cxa_guard_release (single-threaded stubs initially)
 *   - .init_array runner (call from kmain before warp_runtime_enter)
 *   - per-PID compiler/output memory region management (mirrors wasm3/shim.c
 *     heap chunks but using WARP's two-region model)
 */

#include "shim.h"

/* TODO: remove placeholder when implementation is complete */
void
warp_heap_configure(uint32_t /*pid*/, uint64_t /*initial_size*/, uint64_t /*max_size*/)
{
}

uint32_t
warp_heap_bind_pid(uint32_t /*pid*/)
{
    return 0;
}

void
warp_heap_restore_pid(uint32_t /*previous_pid*/)
{
}

uint32_t
warp_runtime_enter(uint32_t /*pid*/)
{
    return 0;
}

void
warp_runtime_leave(uint32_t /*previous_pid*/)
{
}

void
warp_heap_release(uint32_t /*pid*/)
{
}

uint64_t
warp_heap_committed_bytes(uint32_t /*pid*/)
{
    return 0;
}

int
warp_heap_probe_growth(size_t /*size*/)
{
    return 0;
}
