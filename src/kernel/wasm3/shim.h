/* shim.h - Declarations for kernel-internal wasm3 platform shims. */
#ifndef WASMOS_WASM3_SHIM_H
#define WASMOS_WASM3_SHIM_H

#include <stddef.h>
#include <stdint.h>

/* Size the per-pid heap arena.  `initial_size` becomes the preferred chunk size and
 * `max_size` the total commit ceiling, both in bytes; 0 for either selects the default
 * (1024 pages) and the 2 GiB cap respectively.  Both are clamped into
 * [32 pages, 2 GiB] and `initial_size` is further clamped to `max_size`, so the values
 * read back are not necessarily the ones passed.  A pid of 0 is ignored.  Allocates a
 * slot on first use; the slot table is fixed at PROCESS_MAX_COUNT entries. */
void wasm3_heap_configure(uint32_t pid, uint64_t initial_size, uint64_t max_size);

/* Bind this CPU's wasm3 allocator to `pid` and return the pid it displaced, which the
 * caller must hand back to wasm3_heap_restore_pid.  malloc/calloc/realloc/free on this
 * CPU are charged to `pid` until then; a pid of 0 unbinds and falls back to the
 * currently running process. */
uint32_t wasm3_heap_bind_pid(uint32_t pid);
/* Restore the CPU-local allocator binding saved by wasm3_heap_bind_pid. */
void wasm3_heap_restore_pid(uint32_t previous_pid);

/* Enter/leave a wasm3 interpreter region for `pid`.  In addition to the CPU-local heap
 * binding these disable and re-enable preemption, keeping the binding coherent for the
 * whole m3_* call — which is also why a wasm3 guest is not timer-preempted while it
 * runs.  The pair must nest: pass warp/wasm3_runtime_enter's return value back to
 * leave. */
uint32_t wasm3_runtime_enter(uint32_t pid);
void wasm3_runtime_leave(uint32_t previous_pid);

/* Drop all heap state for an exiting pid: releases the dedicated-VA linear-memory slot
 * (idempotent with the runtime's own free), returns every arena chunk to the frame
 * allocator, and clears the slot for reuse.  A pid of 0 is ignored. */
void wasm3_heap_release(uint32_t pid);

/* Bytes of arena actually committed for `pid` (the sum of its chunk sizes), or 0 for a
 * pid with no slot.  This is a measurement, not the configured ceiling; it excludes
 * the dedicated-VA linear-memory slot, which is not an arena chunk. */
uint64_t wasm3_heap_committed_bytes(uint32_t pid);

/* Test whether the arena can currently satisfy a `size`-byte allocation: allocates,
 * writes and re-reads the first and last byte, then frees.  Returns 0 when it can, -1
 * for a zero `size`, an allocation failure, or a read-back mismatch.  Charged to
 * whichever pid this CPU is bound to.  No in-tree caller. */
int wasm3_heap_probe_growth(size_t size);

/* Resolve the physical base backing `size` bytes at kernel pointer `ptr` inside
 * `pid`'s arena.  `ptr` must be a kernel direct-map address returned by this
 * allocator; a pointer into the dedicated-VA linear-memory slot does not resolve,
 * since those pages are scattered and not in an arena chunk.  Returns 0 with
 * *out_phys_base set, or -1 on a null/zero argument, an unknown pid, a pointer outside
 * every chunk, or a range that runs past the end of its chunk.  The physical run is
 * contiguous only because it lies within one chunk.  No in-tree caller. */
int wasm3_heap_query_phys(uint32_t pid, const void* ptr, uint64_t size, uint64_t* out_phys_base);

#endif
