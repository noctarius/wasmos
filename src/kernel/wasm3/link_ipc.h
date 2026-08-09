/* link_ipc.h - Internal seam between link.c and link_ipc.c.
 *
 * The IPC host functions live in their own translation unit so they can be
 * driven by a host test: link.c pulls in io.h (x86 port-I/O inline asm) and two
 * dozen other kernel headers, so it does not compile off x86 at all, and the
 * shims inside it are unreachable to a test however simple they are.
 *
 * The per-pid side tables stay in link.c, which owns their lifecycle
 * (initialisation and teardown alongside every other per-process slot), and are
 * reached from here through the accessors below.
 */
#ifndef WASMOS_WASM3_LINK_IPC_H
#define WASMOS_WASM3_LINK_IPC_H

#include <stdint.h>

#include "ipc.h"
#include "wasm3.h" /* IM3Runtime / IM3ImportContext / m3ApiRawFunction */

/* Last message received by a process, for the field-accessor host call. */
typedef struct {
    uint32_t pid;
    uint8_t valid;
    ipc_message_t message;
} wasm_ipc_last_slot_t;

/* Context of the peer that sent the in-flight FS request, tracked so FS
 * replies can be attributed without trusting the guest. */
typedef struct {
    uint32_t pid;
    uint8_t valid;
    uint32_t peer_context_id;
} wasm_fs_peer_slot_t;

/* Owned by link.c. Return the calling process's slot, allocating one on first
 * use; 0 if pid is 0 or the table is exhausted. */
wasm_ipc_last_slot_t* wasm_ipc_slot_for_pid(uint32_t pid);
wasm_fs_peer_slot_t* wasm_fs_peer_slot_for_pid(uint32_t pid);

/* Owned by link.c. 0 and *out_context_id set, or -1 if there is no current
 * process. */
int current_process_context(uint32_t* out_context_id);

/*
 * The IPC host functions themselves. m3ApiRawFunction expands to the wasm3
 * raw-call signature: arguments and the return value are slots in _sp, with no
 * dependency on a live runtime or instance. link.c's generated link table
 * (wasmos_link_wasm3.inc) references them by name, and a host test can call
 * them directly with a stack array.
 */
m3ApiRawFunction(wasmos_ipc_create_endpoint);
m3ApiRawFunction(wasmos_ipc_endpoint_owner);
m3ApiRawFunction(wasmos_ipc_send);
m3ApiRawFunction(wasmos_ipc_select_one);
m3ApiRawFunction(wasmos_ipc_drain);
m3ApiRawFunction(wasmos_ipc_notify);
m3ApiRawFunction(wasmos_ipc_last_field);
m3ApiRawFunction(wasmos_sys_select_create);
m3ApiRawFunction(wasmos_sys_select_add);
m3ApiRawFunction(wasmos_sys_select_wait);
m3ApiRawFunction(wasmos_sys_select_wait_timeout);
m3ApiRawFunction(wasmos_sys_select_destroy);

#endif /* WASMOS_WASM3_LINK_IPC_H */
