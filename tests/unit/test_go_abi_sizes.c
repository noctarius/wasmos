/* test_go_abi_sizes.c - pins the wasm32 struct sizes the Go binding hardcodes.
 *
 * src/libc/go/coroutine.go models every shared C record as an opaque
 * [N]uint32 blob, because Go cannot express the C layout directly. Nothing in
 * Go checks those counts, and C writes through pointers to them: a blob that is
 * too small is silent heap corruption in a Go guest, not a compile error.
 *
 * The Rust and Zig bindings declare real fields and have their layout computed
 * by the compiler, so they cannot drift this way. Go can, and did -- Coroutine
 * was declared [13]uint32 against a 56-byte C struct, so every Go coroutine had
 * its last field written four bytes past the end of the allocation.
 *
 * Compiled for wasm32 (the only target these sizes describe) and never linked;
 * the assertions are the whole test. If one fires, update the matching
 * declaration in src/libc/go/coroutine.go to the size named in the message.
 */
#include "wasmos/coroutine_wasm.h"
#include "wasmos/libsys.h"

/* Assert that a C record occupies exactly `words` 32-bit words, naming the Go
 * declaration that has to match in the failure message. 4 is sizeof(uint32) --
 * the Go side's storage unit -- not the pointer width, so the relation holds
 * independently of the ABI's pointer size; the wasm32 assertion at the bottom is
 * what pins the sizeof() side.
 *
 * Every count below is the array length written in src/libc/go/coroutine.go
 * (`type Coroutine struct{ storage [14]uint32 }` and its neighbours), and every
 * type is the C record that Go blob stands in for, declared in
 * wasmos/coroutine_wasm.h or wasmos/libsys.h. Neither number is a chosen bound:
 * they are the same object seen from two languages, so a failure means one side
 * changed and the other did not. */
#define WASMOS_GO_BLOB_IS(type, words)                                                             \
    _Static_assert(sizeof(type) == (words) * 4u,                                                   \
                   #type " size changed; src/libc/go/coroutine.go declares [" #words "]uint32")

WASMOS_GO_BLOB_IS(wasmos_wasm_runtime_t, 6);
WASMOS_GO_BLOB_IS(wasmos_future_t, 6);
WASMOS_GO_BLOB_IS(wasmos_promise_t, 1);
WASMOS_GO_BLOB_IS(wasmos_wasm_coroutine_t, 14);
WASMOS_GO_BLOB_IS(wasmos_future_continuation_t, 15);
WASMOS_GO_BLOB_IS(wasmos_future_group_t, 14);
WASMOS_GO_BLOB_IS(wasmos_sys_event_loop_t, 136);
WASMOS_GO_BLOB_IS(wasmos_sys_wasm_ipc_future_t, 20);
WASMOS_GO_BLOB_IS(wasmos_sys_wasm_fs_request_t, 20);
WASMOS_GO_BLOB_IS(wasmos_sys_wasm_fs_operation_t, 24);

/* wasm32 is the only ABI these describe; a host build would assert nothing
 * useful because the pointer width differs. */
_Static_assert(sizeof(void*) == 4u, "test_go_abi_sizes must be built for wasm32");
