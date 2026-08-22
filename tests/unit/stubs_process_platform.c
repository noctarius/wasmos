/* stubs_process_platform.c — the platform surface process.c calls but no
 * lifecycle test exercises.
 *
 * process.c owns two separable things: the process/thread LIFECYCLE state
 * machine (states, exit, reap, wait/join, the scheduler's per-dispatch
 * bookkeeping), and the machinery that gives a process an address space and a
 * stack. A host test can drive the first for real; the second has no host
 * analogue at all — there is no CR3 to load, no physical frame allocator, and
 * no higher-half alias.
 *
 * These stubs make the second group succeed cheaply and inertly so the first
 * group can be driven. Every one of them is a deliberate "not under test":
 *
 *   - Paging/mm/pfa: report success and hand back plausible non-zero values, so
 *     a spawn does not fail for want of an address space. Nothing here records
 *     mappings, so no test may assert on them.
 *   - Runtime teardown hooks (warp/wasm3/xfer_buffer/msi/irq/subsystem): a reap
 *     calls all of them; none has state a lifecycle test observes.
 *   - context_switch: never reached. The WASMOS_PROCESS_TEST_SEAMS arm of
 *     process_run_worker_on_stack calls a worker entry directly instead of
 *     switching stacks, so a test drives worker threads without a context
 *     switch ever happening. Reaching it means the seam was bypassed, which
 *     aborts rather than corrupting the host's stack.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "memory.h"
#include "process.h"
#include "arch/x86_64/smp.h"

/* process.c and its neighbours each define this locally; there is no header to
 * take it from. Only the alignment of the stub allocator depends on it. */
#define STUB_PAGE_SIZE 4096u

/* A fake higher half that is a no-op alias: process.c adds this base to a
 * pointer below it and expects the result to be a valid alias of the same
 * object. Zero is the only value for which "add the base" is the identity, so
 * the host's real pointers survive every rebase in the file. */
uint64_t paging_get_higher_half_base(void) {
    return 0;
}

/* Non-zero and stable: a spawn treats 0 as "no address space". The value is
 * never dereferenced because nothing here walks a page table. */
#define FAKE_ROOT_TABLE 0x1000u

uint64_t paging_get_root_table(void) {
    return FAKE_ROOT_TABLE;
}
uint64_t paging_get_current_root_table(void) {
    return FAKE_ROOT_TABLE;
}
int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)virt;
    (void)phys;
    (void)flags;
    return 0;
}
int paging_unmap_4k(uint64_t virt) {
    (void)virt;
    return 0;
}
int paging_clone_low_slot_in_root(uint64_t root_table) {
    (void)root_table;
    return 0;
}
int paging_strip_low_slot_in_root(uint64_t root_table) {
    (void)root_table;
    return 0;
}
int paging_verify_user_root_no_low_slot(uint64_t root_table, int log_failures) {
    (void)root_table;
    (void)log_failures;
    return 0;
}

/* Frames come from the host heap, page-aligned so the callers' masking and
 * guard-page arithmetic behave. pfa_free_pages cannot free them: process.c
 * frees sub-ranges of an allocation (guard pages separately from the stack),
 * which free() cannot express. A lifecycle test leaks its stacks by design;
 * the alternative is a real allocator, which is a different test's subject. */
uint64_t pfa_alloc_pages_below(uint64_t pages, uint64_t max_addr) {
    void* p = 0;
    (void)max_addr;
    if (pages == 0) {
        return 0;
    }
    if (posix_memalign(&p, STUB_PAGE_SIZE, (size_t)(pages * STUB_PAGE_SIZE)) != 0 || !p) {
        return 0;
    }
    return (uint64_t)(uintptr_t)p;
}
void pfa_free_pages(uint64_t base, uint64_t pages) {
    (void)base;
    (void)pages;
}

/* One shared context record, enough that mm_context_get() returns non-NULL for
 * any id. Region lookups report "no such region", which is what makes the
 * region-walking paths skip rather than fabricate a mapping. */
static mm_context_t g_stub_ctx;

mm_context_t* mm_context_create(uint32_t id) {
    (void)id;
    return &g_stub_ctx;
}
mm_context_t* mm_context_get(uint32_t id) {
    (void)id;
    return &g_stub_ctx;
}
int mm_context_destroy(uint32_t id) {
    (void)id;
    return 0;
}
int mm_context_region_at(mm_context_t* ctx, uint32_t index, mem_region_t* out_region) {
    (void)ctx;
    (void)index;
    (void)out_region;
    return -1;
}
uint64_t mm_context_root_table(uint32_t id) {
    (void)id;
    return FAKE_ROOT_TABLE;
}

void cpu_set_kernel_stack(uint64_t rsp0) {
    (void)rsp0;
}

/* No region table here (mm_context_region_at reports none either), so every
 * typed lookup misses. That is what makes the region-walking paths skip. */
int mm_context_region_for_type(mm_context_t* ctx, mem_region_type_t type,
                               mem_region_t* out_region) {
    (void)ctx;
    (void)type;
    (void)out_region;
    return -1;
}

/* A frozen clock. Nothing in a lifecycle test measures elapsed time, and a
 * monotonic host clock would make deadline arithmetic vary run to run; the
 * scheduler's timeout sweep is driven from the scheduler, which these tests do
 * not run, so an armed deadline never fires. */
uint64_t timer_ticks(void) {
    return 0;
}
uint64_t timer_ms_to_ticks(uint32_t ms) {
    return (uint64_t)ms;
}

/* The kernel log, discarded. A lifecycle test asserts on counters and state,
 * never on log text, and process.c logs on paths these tests take by design
 * (the refusal report among them) -- printing them would bury the results. */
void klog_write(const char* s) {
    (void)s;
}
void klog_printf(const char* fmt, ...) {
    (void)fmt;
}
void serial_write_hex64(uint64_t v) {
    (void)v;
}

/* Per-context teardown hooks a reap fans out to. */
void irq_release_context(uint32_t context_id) {
    (void)context_id;
}
void msi_release_context(uint32_t context_id) {
    (void)context_id;
}
void xfer_buffer_drop_context(uint32_t context_id) {
    (void)context_id;
}
void wasmos_subsystem_registry_drop_owner(uint32_t context_id) {
    (void)context_id;
}
void ipc_endpoints_release_owner(uint32_t context_id) {
    (void)context_id;
}
void warp_release_pid(uint32_t pid) {
    (void)pid;
}
void wasm3_release_pid(uint32_t pid) {
    (void)pid;
}
void native_driver_heap_release(uint32_t pid) {
    (void)pid;
}
uint64_t native_driver_heap_committed_bytes(uint32_t pid) {
    (void)pid;
    return 0;
}
void wasm3_heap_release(uint32_t pid) {
    (void)pid;
}
uint64_t wasm3_heap_committed_bytes(uint32_t pid) {
    (void)pid;
    return 0;
}

/* Kernel image bounds, used only by the alias/range checks. A range that
 * contains nothing makes every "is this pointer inside the kernel image"
 * question answer no, which is true of host pointers. */
/* NOLINTNEXTLINE(bugprone-reserved-identifier) — matches the linker symbol name
 * process.c resolves, which is not ours to rename. */
char __kernel_start[1];
/* NOLINTNEXTLINE(bugprone-reserved-identifier) */
char __kernel_end[1];

void process_preempt_trampoline(void) {}

/* An inert switch: a dispatched non-worker thread makes no progress and reports
 * back as having yielded.
 *
 * A real context_switch saves the scheduler's registers and resumes the
 * thread's, and there is no host equivalent -- the target context was never
 * populated by anything (the seam in process_run_worker_on_stack calls worker
 * entries directly instead of switching). Returning immediately models "the
 * thread was dispatched and yielded without running", which is exactly what a
 * lifecycle test wants from a main thread: it must be schedulable, so its
 * process is dispatchable and its worker threads get picked up, but it must not
 * need to execute.
 *
 * last_run_result MUST be set here. The dispatcher reads it after the switch
 * returns to decide what happened to the thread, and a stale value from a
 * previous dispatch -- THREAD_EXITED, say -- would drive a transition the
 * thread never asked for. */
void context_switch(process_context_t* out, process_context_t* in) {
    (void)out;
    (void)in;
    cpu_local()->last_run_result = PROCESS_RUN_YIELDED;
}
void context_switch_to(process_context_t* in) {
    (void)in;
    cpu_local()->last_run_result = PROCESS_RUN_YIELDED;
}
