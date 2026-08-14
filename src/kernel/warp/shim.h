/* warp/shim.h - Kernel-side ABI shim declarations for the WARP JIT runtime.
 *
 * Mirrors the interface exposed by wasm3/shim.h so callers can be switched
 * between runtimes via WASMOS_WASM_RUNTIME without changing call sites.
 * The bodies live in warp/shim.cpp; the C++ ABI they depend on (operator
 * new/delete in warp/shim.cpp, the __cxa_* stubs in warp/cxx_abi.cpp) is part
 * of the same runtime block. */
#ifndef WASMOS_WARP_SHIM_H
#define WASMOS_WARP_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create or overwrite the per-pid heap configuration.  `initial_size` is the app's
 * declared linear-memory footprint in bytes and doubles as the map_auto scan ceiling
 * once the linmem block moves into its VA slot; `max_size` is the growth cap in bytes.
 * Neither is rounded or clamped here.  A pid of 0 is ignored.  The first call also
 * installs the kernel allocator into vb::WasmModule::initEnvironment and creates the
 * per-pid table, so it must run before any module is constructed for that pid. */
void warp_heap_configure(uint32_t pid, uint64_t initial_size, uint64_t max_size);

/* Bind this CPU's WARP allocator to `pid` and return the pid it displaced, which the
 * caller must hand back to warp_heap_restore_pid to unwind the binding.  Every
 * allocation made on this CPU until then is charged to `pid`; a pid of 0 unbinds.
 * Takes no lock and disables no preemption. */
uint32_t warp_heap_bind_pid(uint32_t pid);
/* Restore the CPU-local allocator binding saved by warp_heap_bind_pid. */
void warp_heap_restore_pid(uint32_t previous_pid);

/* Enter/leave a WARP runtime region for `pid`.  Currently exactly the bind/restore
 * pair above — no lock is taken, so the "one CPU inside WARP at a time" invariant the
 * rest of the backend assumes is not enforced here (see the FIXME(smp-warp) at the
 * definition).  Unlike wasm3_runtime_enter these do NOT disable preemption, because a
 * ring-3 WARP guest must stay timer-preemptible. */
uint32_t warp_runtime_enter(uint32_t pid);
void warp_runtime_leave(uint32_t previous_pid);

/* Drop all per-pid heap state on process exit: frees the dedicated-VA linmem slot
 * (idempotent with the synchronous teardown in warp_kfree) and removes the config
 * entry.  A pid of 0 is ignored.  Safe to call for a pid that was never configured. */
void warp_heap_release(uint32_t pid);

/* Configured linear-memory footprint in bytes for `pid` (the `initial_size` passed to
 * warp_heap_configure), or 0 when the pid has no configuration.  This is the declared
 * size, not a measurement of pages actually committed. */
uint64_t warp_heap_committed_bytes(uint32_t pid);

/* Test whether the WARP allocator can currently satisfy a `size`-byte block: allocates
 * and immediately frees one.  Returns 0 when it can, -1 when the allocation failed.
 * Charged to whichever pid this CPU is bound to.  No in-tree caller. */
int warp_heap_probe_growth(size_t size);

/* Dedicated-VA linmem slot: arm the one-shot pid hint before the module's first
 * linmem-allocating probe; read the reserved capacity back to bound map_auto. */

/* Arm the one-shot hint naming the pid whose next page-backed reallocation is the
 * linear-memory block, so that block is moved into a dedicated reserved-VA slot with a
 * pinned base.  A non-zero `reserve_bytes` arms it; the value itself only has to be
 * non-zero, because the slot is the fixed WARP_LINMEM_VA_STRIDE window.  The hint is
 * claimed by whichever CPU is bound to `pid` (CAS), so a spawn on another CPU cannot
 * steal it, and it is consumed by the first claiming growth — call it immediately
 * before the runtime-setup phase, never before compilation, or a compiler scratch
 * buffer takes the slot instead. */
void warp_linmem_reserve_hint(uint32_t pid, uint64_t reserve_bytes);

/* Reserved linmem capacity in bytes for `pid`: the ceiling warp_shmem_map_auto scans
 * when placing a window.  0 until the pid's block has moved into its VA slot, and 0
 * for an unconfigured pid.  Set to the pid's configured heap size, NOT the 2 GiB slot
 * stride — a slot-sized ceiling would push a window past the module's own maximum. */
uint64_t warp_linmem_reserved_bytes(uint32_t pid);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
