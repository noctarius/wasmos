/* test_hostcall_dma.cpp — the DMA host-call shims, both runtimes, side by side.
 *
 * Regression: 2026-08-21-warp-dma-capability-bypass -- warp_dma_map_borrow
 * performed none of the capability checks its wasm3 counterpart did, so a WARP
 * guest holding any transfer-buffer borrow could program a device to DMA in a
 * direction it was never granted, over an unbounded length, at a physical
 * address outside every window its manifest declared. dma_sync_borrow and
 * dma_unmap_borrow skipped the dma.buffer gate entirely, and neither validated
 * its range or sync opcode. The checks existed once per runtime and only one
 * copy had them.
 *
 * These shims are the last layer before a guest: they validate its arguments,
 * resolve its context, and decide whether the mapping is permitted. An app is
 * supposed to behave identically under wasm3 and WARP, and this is where that
 * promise is kept or broken, so every scenario asserts each runtime's own value
 * AND the two against each other. A row that passes under one runtime and fails
 * under the other is the bug above, restated.
 *
 * The policy side is real: the tests link the actual capability.c and policy.c,
 * so a denial is produced by the same table a driver spawn writes into. The
 * transfer-buffer layer is stubbed, because what is under test is the decision
 * and not the mapping -- a stub is what lets a scenario hand back a device
 * address outside the granted window, and observe whether the shim undid the
 * mapping on its way out.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "test_shuffle.h"

extern "C" {
#include "capability.h"
#include "process.h"
#include "wasmos_driver_abi.h"
#include "xfer_buffer.h"
}

#include "warp/link_dma.h"
#include "wasm3/link_dma.h"

static int g_failures;
static int g_checks;

/* ------------------------------------------------------------------ fixture */

/* Contexts a scenario draws on. CAPABLE holds dma.buffer with both directions,
 * a 4 KiB per-mapping budget and one window; NARROW holds it with TO_DEVICE
 * only, a 256-byte budget and a window that does not contain DEVICE_ADDR;
 * BARE holds no capability record at all. */
#define CTX_CAPABLE 42u
#define CTX_NARROW 43u
#define CTX_BARE 44u
#define CALLER_PID 7u

/* The stub's only live borrow. Any other id is reported as inactive, which is
 * what a forged or stale handle looks like to the real layer. */
#define LIVE_BORROW 5

/* Device address the stubbed mapping reports. Inside CTX_CAPABLE's window and
 * outside CTX_NARROW's, so the same call is permitted for one and refused for
 * the other on the range check alone. */
#define DEVICE_ADDR 0x10001000ull
#define CAPABLE_WINDOW_BASE 0x10000000ull
#define CAPABLE_WINDOW_LEN 0x00010000ull
#define NARROW_WINDOW_BASE 0x20000000ull
#define NARROW_WINDOW_LEN 0x00001000ull
#define CAPABLE_MAX_BYTES 4096u
#define NARROW_MAX_BYTES 256u

static process_t g_proc;
static int g_have_process = 1;

/* What the stubbed transfer-buffer layer reports and what it recorded. */
static uint64_t g_stub_device_addr = DEVICE_ADDR;
static int g_stub_map_rc = WASMOS_ERR_NONE;
static int g_stub_map_calls;
static int g_stub_unmap_calls;
static int g_stub_sync_calls;

extern "C" {

/* Process environment: one process, whose context the scenario chooses. */
uint32_t process_current_pid(void) {
    return g_have_process ? CALLER_PID : 0u;
}
process_t* process_get(uint32_t pid) {
    return (g_have_process && pid == CALLER_PID) ? &g_proc : nullptr;
}
process_t* process_find_by_context(uint32_t context_id) {
    return (g_have_process && context_id == g_proc.context_id) ? &g_proc : nullptr;
}

/* Referenced by policy.c's policy_require path only, which nothing here takes:
 * the DMA action is decided by policy_authorize, which never kills a process. */
void process_set_exit_status(process_t* proc, int32_t exit_status) {
    (void)proc;
    (void)exit_status;
}
void process_yield(process_run_result_t result) {
    (void)result;
}
void serial_write(const char* s) {
    (void)s;
}

/* capability.c matches manifest capability names with this. The kernel's own
 * definition lives in libc.c beside its memcmp/strlen, which cannot be linked
 * into a host binary; the contract is in kernel/include/string.h -- 1 only when
 * the lengths match exactly, opposite polarity to strcmp. No kernel_str_ptr
 * rebasing here: a host test has no higher-half alias to rebase through. */
int str_eq_bytes(const uint8_t* bytes, size_t bytes_len, const char* lit) {
    size_t i = 0;
    if (!bytes || !lit) {
        return 0;
    }
    while (lit[i]) {
        if (i >= bytes_len || bytes[i] != (uint8_t)lit[i]) {
            return 0;
        }
        i++;
    }
    return i == bytes_len;
}

/* wasm3 needs this symbol for m3ApiReturn's success path; its value is never
 * inspected here, only the slot the shim wrote. */
const char* const m3Err_none = nullptr;

/* The transfer-buffer layer, stubbed. LIVE_BORROW resolves for whichever
 * context is current -- ownership of the borrow is not what these scenarios
 * vary, the capability is -- and every other id is inactive. The mapping is
 * synthetic: `active` and `device_addr` are what the shim goes on to check. */
int xfer_buffer_get_borrowed(uint32_t borrow_id, uint32_t context_id,
                             xfer_buffer_borrow_t* out_borrow,
                             xfer_buffer_dma_mapping_t* out_mapping) {
    if (borrow_id != (uint32_t)LIVE_BORROW) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    }
    if (out_borrow) {
        memset(out_borrow, 0, sizeof(*out_borrow));
        out_borrow->borrower_context_id = context_id;
        out_borrow->borrow_id = borrow_id;
        out_borrow->flags = WASMOS_DMA_DIR_BIDIR;
    }
    if (out_mapping) {
        memset(out_mapping, 0, sizeof(*out_mapping));
        out_mapping->borrow_id = borrow_id;
        out_mapping->device_addr = g_stub_device_addr;
        out_mapping->attached_via_borrow = 1;
        out_mapping->active = 1;
    }
    return WASMOS_ERR_NONE;
}

int xfer_buffer_dma_map_borrow(const xfer_buffer_borrow_t* borrow, uint32_t offset, uint32_t length,
                               uint32_t direction_flags, xfer_buffer_dma_mapping_t* out_mapping) {
    (void)borrow;
    g_stub_map_calls++;
    if (g_stub_map_rc != WASMOS_ERR_NONE) {
        return g_stub_map_rc;
    }
    memset(out_mapping, 0, sizeof(*out_mapping));
    out_mapping->offset = offset;
    out_mapping->length = length;
    out_mapping->direction_flags = direction_flags;
    out_mapping->device_addr = g_stub_device_addr;
    out_mapping->attached_via_borrow = 1;
    out_mapping->active = 1;
    return WASMOS_ERR_NONE;
}

int xfer_buffer_dma_unmap(xfer_buffer_dma_mapping_t* mapping) {
    g_stub_unmap_calls++;
    if (mapping) {
        mapping->active = 0;
    }
    return WASMOS_ERR_NONE;
}

int xfer_buffer_dma_sync(const xfer_buffer_dma_mapping_t* mapping, uint32_t offset,
                         uint32_t length) {
    (void)mapping;
    (void)offset;
    (void)length;
    g_stub_sync_calls++;
    return WASMOS_ERR_NONE;
}

/* Owned by link.c / link.cpp in the real kernel; both seams declare it and
 * resolve the caller's context through it. */
int current_process_context(uint32_t* out_context_id) {
    uint32_t pid = process_current_pid();
    process_t* proc = process_get(pid);
    if (!proc || !out_context_id) {
        return -1;
    }
    *out_context_id = proc->context_id;
    return 0;
}
int warp_current_context_id(uint32_t* out) {
    return current_process_context(out);
}

} /* extern "C" */

/* Rebuilds the capability table and the stub's counters, so no scenario
 * inherits another's grants or observations. capability_init reseeds context 0
 * and drops every other record, which is what makes the grants below
 * repeatable. */
static void reset(void) {
    capability_init();

    memset(&g_proc, 0, sizeof(g_proc));
    g_proc.pid = CALLER_PID;
    g_proc.context_id = CTX_CAPABLE;
    g_proc.name = "dma-hostcall-test";
    g_have_process = 1;

    g_stub_device_addr = DEVICE_ADDR;
    g_stub_map_rc = WASMOS_ERR_NONE;
    g_stub_map_calls = 0;
    g_stub_unmap_calls = 0;
    g_stub_sync_calls = 0;

    const uint8_t dma_name[] = "dma.buffer";
    const uint32_t dma_name_len = (uint32_t)(sizeof(dma_name) - 1);
    const uint32_t cap_flags = 1u << CAP_DMA_BUFFER;

    wasmos_dma_window_t wide = {CAPABLE_WINDOW_BASE, CAPABLE_WINDOW_LEN};
    (void)capability_grant_name(CTX_CAPABLE, dma_name, dma_name_len, 1u);
    (void)capability_set_spawn_profile(
        CTX_CAPABLE, cap_flags, 0, nullptr, 0, WASMOS_DMA_DIR_BIDIR, CAPABLE_MAX_BYTES, 1, &wide);

    wasmos_dma_window_t narrow = {NARROW_WINDOW_BASE, NARROW_WINDOW_LEN};
    (void)capability_grant_name(CTX_NARROW, dma_name, dma_name_len, 1u);
    (void)capability_set_spawn_profile(CTX_NARROW,
                                       cap_flags,
                                       0,
                                       nullptr,
                                       0,
                                       WASMOS_DMA_DIR_TO_DEVICE,
                                       NARROW_MAX_BYTES,
                                       1,
                                       &narrow);

    /* CTX_BARE is left absent on purpose: an unknown context is the deny-by-
     * default case, and capability_context_configured must stay false for it. */
}

/* Points the caller at `ctx` for the rest of the scenario. The shims resolve the
 * caller's context through process_get()->context_id, so this is the only place
 * that has to change -- there is no second copy to keep in step. */
static void use_context(uint32_t ctx) {
    g_proc.context_id = ctx;
}

/* ------------------------------------------- wasm3 raw-call invocation */

/* m3ApiReturnType takes _sp[0] as the return slot and m3ApiGetArg walks
 * forward from _sp[1], so a call is just a stack array. */
static int32_t w3_call(M3RawCall fn, std::initializer_list<int32_t> args) {
    uint64_t sp[16];
    memset(sp, 0, sizeof(sp));
    unsigned i = 1;
    for (int32_t a : args) {
        sp[i++] = (uint64_t)(uint32_t)a;
    }
    (void)fn(nullptr, nullptr, sp, nullptr);
    return (int32_t)(uint32_t)sp[0];
}

/* ------------------------------------------------- per-runtime adapters */

/* One scenario, run through both runtimes. Each adapter returns whatever the
 * shim gave back, narrowed to i32 -- which is how a guest reads it. */
struct Shims {
    const char* name;
    int32_t (*map)(int32_t borrow, int32_t offset, int32_t length, int32_t dir);
    int32_t (*sync)(int32_t borrow, int32_t offset, int32_t length, int32_t op);
    int32_t (*unmap)(int32_t borrow);
};

static int32_t w3_map(int32_t b, int32_t o, int32_t l, int32_t d) {
    return w3_call(wasmos_dma_map_borrow, {b, o, l, d});
}
static int32_t w3_sync(int32_t b, int32_t o, int32_t l, int32_t op) {
    return w3_call(wasmos_dma_sync_borrow, {b, o, l, op});
}
static int32_t w3_unmap(int32_t b) {
    return w3_call(wasmos_dma_unmap_borrow, {b});
}

static int32_t wp_map(int32_t b, int32_t o, int32_t l, int32_t d) {
    return (int32_t)warp_dma_map_borrow(
        (uint32_t)b, (uint32_t)o, (uint32_t)l, (uint32_t)d, nullptr);
}
static int32_t wp_sync(int32_t b, int32_t o, int32_t l, int32_t op) {
    return (int32_t)warp_dma_sync_borrow(
        (uint32_t)b, (uint32_t)o, (uint32_t)l, (uint32_t)op, nullptr);
}
static int32_t wp_unmap(int32_t b) {
    return (int32_t)warp_dma_unmap_borrow((uint32_t)b, nullptr);
}

static const Shims k_wasm3 = {"wasm3", w3_map, w3_sync, w3_unmap};
static const Shims k_warp = {"WARP", wp_map, wp_sync, wp_unmap};

/* ------------------------------------------------------- scenario table */

/* A scenario returns one guest-observable value, run through both runtimes and
 * asserted against a per-runtime expectation AND against the other runtime.
 *
 * `divergent` marks a row where the two are deliberately allowed to disagree.
 * Such a row is checked BOTH ways -- the values must still DIFFER -- so
 * converging them fails this test and forces the row to be reclassified. No row
 * here is divergent: a guest's DMA rights must not depend on which engine
 * happens to be running it, which is the whole subject of this suite.
 *
 * `note` records why a row's expectation is what it is. It is data only. */
struct Scenario {
    const char* what;
    int32_t expect_wasm3;
    int32_t expect_warp;
    bool divergent;
    int32_t (*run)(const Shims& s);
    const char* note;
};

/* -- map: argument validation ------------------------------------------- */

static int32_t s_map_negative_borrow(const Shims& s) {
    return s.map(-1, 0, 64, WASMOS_DMA_DIR_TO_DEVICE);
}
static int32_t s_map_negative_offset(const Shims& s) {
    return s.map(LIVE_BORROW, -16, 64, WASMOS_DMA_DIR_TO_DEVICE);
}
static int32_t s_map_zero_length(const Shims& s) {
    return s.map(LIVE_BORROW, 0, 0, WASMOS_DMA_DIR_TO_DEVICE);
}
static int32_t s_map_negative_direction(const Shims& s) {
    return s.map(LIVE_BORROW, 0, 64, -1);
}
static int32_t s_map_zero_direction(const Shims& s) {
    return s.map(LIVE_BORROW, 0, 64, 0);
}

/* -- map: the capability gate ------------------------------------------- */

static int32_t s_map_no_capability(const Shims& s) {
    use_context(CTX_BARE);
    return s.map(LIVE_BORROW, 0, 64, WASMOS_DMA_DIR_TO_DEVICE);
}
static int32_t s_map_no_process(const Shims& s) {
    g_have_process = 0;
    int32_t rc = s.map(LIVE_BORROW, 0, 64, WASMOS_DMA_DIR_TO_DEVICE);
    g_have_process = 1;
    return rc;
}
static int32_t s_map_direction_not_granted(const Shims& s) {
    use_context(CTX_NARROW);
    return s.map(LIVE_BORROW, 0, 64, WASMOS_DMA_DIR_FROM_DEVICE);
}
static int32_t s_map_direction_partly_granted(const Shims& s) {
    use_context(CTX_NARROW);
    return s.map(LIVE_BORROW, 0, 64, WASMOS_DMA_DIR_BIDIR);
}
static int32_t s_map_over_budget(const Shims& s) {
    return s.map(LIVE_BORROW, 0, (int32_t)CAPABLE_MAX_BYTES + 1, WASMOS_DMA_DIR_TO_DEVICE);
}
static int32_t s_map_at_budget(const Shims& s) {
    int32_t rc = s.map(LIVE_BORROW, 0, (int32_t)CAPABLE_MAX_BYTES, WASMOS_DMA_DIR_TO_DEVICE);
    return rc == (int32_t)DEVICE_ADDR ? 1 : rc;
}

/* -- map: the device-address window ------------------------------------- */

static int32_t s_map_address_outside_window(const Shims& s) {
    use_context(CTX_NARROW);
    return s.map(LIVE_BORROW, 0, 64, WASMOS_DMA_DIR_TO_DEVICE);
}
/* The refusal above must not leave the device programmed: a mapping that is
 * established and then refused has to be undone before the shim returns, or the
 * guest gets a live DMA window it was told it did not get. */
static int32_t s_map_outside_window_undoes_mapping(const Shims& s) {
    use_context(CTX_NARROW);
    (void)s.map(LIVE_BORROW, 0, 64, WASMOS_DMA_DIR_TO_DEVICE);
    return g_stub_unmap_calls;
}
static int32_t s_map_address_over_i32(const Shims& s) {
    g_stub_device_addr = 0x80000000ull;
    wasmos_dma_window_t huge = {0x80000000ull, 0x1000ull};
    (void)capability_set_spawn_profile(CTX_CAPABLE,
                                       1u << CAP_DMA_BUFFER,
                                       0,
                                       nullptr,
                                       0,
                                       WASMOS_DMA_DIR_BIDIR,
                                       CAPABLE_MAX_BYTES,
                                       1,
                                       &huge);
    return s.map(LIVE_BORROW, 0, 64, WASMOS_DMA_DIR_TO_DEVICE);
}

/* -- map: the permitted case -------------------------------------------- */

static int32_t s_map_granted(const Shims& s) {
    int32_t rc = s.map(LIVE_BORROW, 0, 64, WASMOS_DMA_DIR_TO_DEVICE);
    return rc == (int32_t)DEVICE_ADDR ? 1 : rc;
}
static int32_t s_map_granted_keeps_mapping(const Shims& s) {
    (void)s.map(LIVE_BORROW, 0, 64, WASMOS_DMA_DIR_TO_DEVICE);
    return g_stub_unmap_calls;
}
static int32_t s_map_stale_borrow(const Shims& s) {
    return s.map(LIVE_BORROW + 1, 0, 64, WASMOS_DMA_DIR_TO_DEVICE);
}

/* -- sync --------------------------------------------------------------- */

static int32_t s_sync_no_capability(const Shims& s) {
    use_context(CTX_BARE);
    return s.sync(LIVE_BORROW, 0, 64, WASMOS_DMA_SYNC_TO_DEVICE);
}
static int32_t s_sync_no_capability_does_nothing(const Shims& s) {
    use_context(CTX_BARE);
    (void)s.sync(LIVE_BORROW, 0, 64, WASMOS_DMA_SYNC_TO_DEVICE);
    return g_stub_sync_calls;
}
static int32_t s_sync_negative_borrow(const Shims& s) {
    return s.sync(-1, 0, 64, WASMOS_DMA_SYNC_TO_DEVICE);
}
static int32_t s_sync_negative_offset(const Shims& s) {
    return s.sync(LIVE_BORROW, -1, 64, WASMOS_DMA_SYNC_TO_DEVICE);
}
static int32_t s_sync_zero_length(const Shims& s) {
    return s.sync(LIVE_BORROW, 0, 0, WASMOS_DMA_SYNC_TO_DEVICE);
}
static int32_t s_sync_bad_op(const Shims& s) {
    return s.sync(LIVE_BORROW, 0, 64, 9);
}
static int32_t s_sync_granted(const Shims& s) {
    return s.sync(LIVE_BORROW, 0, 64, WASMOS_DMA_SYNC_BIDIR);
}

/* -- unmap -------------------------------------------------------------- */

static int32_t s_unmap_no_capability(const Shims& s) {
    use_context(CTX_BARE);
    return s.unmap(LIVE_BORROW);
}
static int32_t s_unmap_no_capability_does_nothing(const Shims& s) {
    use_context(CTX_BARE);
    (void)s.unmap(LIVE_BORROW);
    return g_stub_unmap_calls;
}
static int32_t s_unmap_negative_borrow(const Shims& s) {
    return s.unmap(-1);
}
static int32_t s_unmap_granted(const Shims& s) {
    return s.unmap(LIVE_BORROW);
}

static const Scenario k_scenarios[] = {
    {"map(negative borrow)",
     WASMOS_ERR_DMA_INVALID,
     WASMOS_ERR_DMA_INVALID,
     false,
     s_map_negative_borrow,
     "a widened negative handle must not be read as a large id"},
    {"map(negative offset)",
     WASMOS_ERR_DMA_INVALID,
     WASMOS_ERR_DMA_INVALID,
     false,
     s_map_negative_offset,
     "WARP took offset as uint32 and never checked its sign"},
    {"map(zero length)",
     WASMOS_ERR_DMA_INVALID,
     WASMOS_ERR_DMA_INVALID,
     false,
     s_map_zero_length,
     nullptr},
    {"map(negative direction)",
     WASMOS_ERR_DMA_INVALID,
     WASMOS_ERR_DMA_INVALID,
     false,
     s_map_negative_direction,
     "WARP rejected only a zero direction, not a negative one"},
    {"map(zero direction)",
     WASMOS_ERR_DMA_INVALID,
     WASMOS_ERR_DMA_INVALID,
     false,
     s_map_zero_direction,
     nullptr},
    {"map(no dma.buffer capability)",
     WASMOS_ERR_DMA_DENY,
     WASMOS_ERR_DMA_DENY,
     false,
     s_map_no_capability,
     "the bug: WARP mapped for a context holding no DMA capability"},
    {"map(no current process)",
     WASMOS_ERR_DMA_DENY,
     WASMOS_ERR_DMA_DENY,
     false,
     s_map_no_process,
     nullptr},
    {"map(direction not granted)",
     WASMOS_ERR_DMA_DENY,
     WASMOS_ERR_DMA_DENY,
     false,
     s_map_direction_not_granted,
     "FROM_DEVICE against a TO_DEVICE-only profile"},
    {"map(one of two directions granted)",
     WASMOS_ERR_DMA_DENY,
     WASMOS_ERR_DMA_DENY,
     false,
     s_map_direction_partly_granted,
     "every bit must be granted, not just one"},
    {"map(length over the budget)",
     WASMOS_ERR_DMA_RANGE,
     WASMOS_ERR_DMA_RANGE,
     false,
     s_map_over_budget,
     "dma_max_bytes bounds a single mapping"},
    {"map(length at the budget)", 1, 1, false, s_map_at_budget, "the boundary is inclusive"},
    {"map(device address outside every window)",
     WASMOS_ERR_DMA_RANGE,
     WASMOS_ERR_DMA_RANGE,
     false,
     s_map_address_outside_window,
     "the address the device is programmed with must be in-window"},
    {"map(outside window undoes the mapping)",
     1,
     1,
     false,
     s_map_outside_window_undoes_mapping,
     "a refused call must not leave a live DMA window behind"},
    {"map(device address past i32)",
     WASMOS_ERR_DMA_ADDR_TOO_LARGE,
     WASMOS_ERR_DMA_ADDR_TOO_LARGE,
     false,
     s_map_address_over_i32,
     "an address the i32 return cannot carry is neither truncated nor reported as "
     "UNAVAILABLE, which means 'no slot or backing'"},
    {"map(granted)", 1, 1, false, s_map_granted, "returns the device address"},
    {"map(granted keeps the mapping)", 0, 0, false, s_map_granted_keeps_mapping, nullptr},
    {"map(stale borrow)",
     WASMOS_ERR_DMA_DENY,
     WASMOS_ERR_DMA_DENY,
     false,
     s_map_stale_borrow,
     nullptr},
    {"sync(no dma.buffer capability)",
     WASMOS_ERR_DMA_DENY,
     WASMOS_ERR_DMA_DENY,
     false,
     s_sync_no_capability,
     "WARP skipped the gate entirely"},
    {"sync(no capability touches nothing)",
     0,
     0,
     false,
     s_sync_no_capability_does_nothing,
     nullptr},
    {"sync(negative borrow)",
     WASMOS_ERR_DMA_INVALID,
     WASMOS_ERR_DMA_INVALID,
     false,
     s_sync_negative_borrow,
     nullptr},
    {"sync(negative offset)",
     WASMOS_ERR_DMA_INVALID,
     WASMOS_ERR_DMA_INVALID,
     false,
     s_sync_negative_offset,
     "WARP validated only the borrow handle"},
    {"sync(zero length)",
     WASMOS_ERR_DMA_INVALID,
     WASMOS_ERR_DMA_INVALID,
     false,
     s_sync_zero_length,
     nullptr},
    {"sync(unknown sync op)",
     WASMOS_ERR_DMA_INVALID,
     WASMOS_ERR_DMA_INVALID,
     false,
     s_sync_bad_op,
     "WARP accepted any opcode because it ignores the direction"},
    {"sync(granted)", WASMOS_ERR_NONE, WASMOS_ERR_NONE, false, s_sync_granted, nullptr},
    {"unmap(no dma.buffer capability)",
     WASMOS_ERR_DMA_DENY,
     WASMOS_ERR_DMA_DENY,
     false,
     s_unmap_no_capability,
     "WARP skipped the gate entirely"},
    {"unmap(no capability touches nothing)",
     0,
     0,
     false,
     s_unmap_no_capability_does_nothing,
     nullptr},
    {"unmap(negative borrow)",
     WASMOS_ERR_DMA_INVALID,
     WASMOS_ERR_DMA_INVALID,
     false,
     s_unmap_negative_borrow,
     nullptr},
    {"unmap(granted)", WASMOS_ERR_NONE, WASMOS_ERR_NONE, false, s_unmap_granted, nullptr},
};

/* -------------------------------------------------------------------- main */

/* Runs every scenario twice -- once per runtime, each after its own reset() --
 * and makes three assertions per row: the wasm3 value, the WARP value, and the
 * relationship between them. Returns 0 when g_failures is zero, 1 otherwise. */
int main(void) {
    reset();

    int divergences = 0;
    /* Randomized order: these scenarios share the capability table and the
     * stub's counters through reset(), so one leaving state behind must not be
     * able to make the next pass. Replay a failure with WASMOS_TEST_SEED. */
    const int scenario_count = (int)(sizeof(k_scenarios) / sizeof(k_scenarios[0]));
    int order[WASMOS_TEST_MAX_CASES];
    const uint64_t seed = wasmos_test_shuffle(order, scenario_count);

    for (int i = 0; i < scenario_count; ++i) {
        const Scenario& sc = k_scenarios[order[i]];
        reset();
        int32_t w3 = sc.run(k_wasm3);
        reset();
        int32_t wp = sc.run(k_warp);

        g_checks += 2;
        if (w3 != sc.expect_wasm3) {
            g_failures++;
            printf("  [FAIL] wasm3 %s: expected %d, got %d\n", sc.what, sc.expect_wasm3, w3);
        }
        if (wp != sc.expect_warp) {
            g_failures++;
            printf("  [FAIL] WARP  %s: expected %d, got %d\n", sc.what, sc.expect_warp, wp);
        }

        g_checks++;
        if (sc.divergent) {
            divergences++;
            /* Asserted to STILL differ, so unifying them fails here and forces
               this row to be reclassified. */
            if (w3 == wp) {
                g_failures++;
                printf(
                    "  [FAIL] %s no longer diverges (both %d) -- update the table\n", sc.what, w3);
            }
        } else if (w3 != wp) {
            g_failures++;
            printf("  [FAIL] parity %s: wasm3=%d WARP=%d\n", sc.what, w3, wp);
        }
    }
    printf("  ... %d of %d scenarios differ between the runtimes\n", divergences, scenario_count);

    printf("test_hostcall_dma: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}
