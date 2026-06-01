## libsys and IPC Service Runtime

This document describes the WASM-side service runtime library (`libsys`): the
event loop, intent tracking, message handler registration, and utility
functions used by WASM drivers and services.

**Sources**: `src/libsys/wasm/include/wasmos/libsys.h`,
`src/libsys/wasm/include/wasmos/libsys_string.h`,
`src/libsys/native/zig/libsys.zig` (native variant)

---

### Purpose

`libsys.h` is a header-only library (all functions are `static inline`). It
provides a structured event loop on top of the raw `wasmos_ipc_send` /
`wasmos_ipc_recv` / `wasmos_ipc_try_recv` hostcalls. All WASM services that
handle more than one IPC opcode use it.

---

### Event Loop

```c
#define WASMOS_SYS_INTENT_MAX  16
#define WASMOS_SYS_HANDLER_MAX 16

typedef struct {
    int32_t receiver_endpoint;
    int32_t next_request_id;
    void (*default_on_message)(void *user, const wasmos_ipc_message_t *msg);
    void *default_user;
    wasmos_sys_intent_t  intents [WASMOS_SYS_INTENT_MAX];   // pending send callbacks
    wasmos_sys_handler_t handlers[WASMOS_SYS_HANDLER_MAX];  // persistent type callbacks
} wasmos_sys_event_loop_t;
```

#### Initialization

`wasmos_sys_event_loop_init(loop, receiver_endpoint, request_id_base)`:
- Sets `loop->receiver_endpoint` and `loop->next_request_id`.
- Zeroes all intent and handler slots.

#### Handler Registration

`wasmos_sys_event_register(loop, msg_type, on_message, user)`:
- Registers a persistent callback for a specific IPC message type.
- At most `WASMOS_SYS_HANDLER_MAX` (16) type-specific handlers per loop.
- If the type is already registered, the callback is replaced in-place.
- Returns -1 if the handler table is full.

`wasmos_sys_event_set_default(loop, on_message, user)`:
- Sets a catch-all handler for message types with no registered handler.

#### Intent Tracking

An *intent* is a one-shot callback tied to a specific `request_id`. It is used
to associate a response from a target service with the action that sent the
request.

`wasmos_sys_intent_send(loop, dest_ep, src_ep, type, arg0–3, on_resolve, user, *out_request_id)`:
- Sends an IPC message with an auto-assigned request ID.
- Registers `on_resolve` to fire when a reply with that `request_id` arrives.
- Returns -1 if the intent table is full (16 in-flight intents maximum).
- The callback fires exactly once; the slot is freed before calling `on_resolve`.

`wasmos_sys_intent_send_with_request_id(...)`:
- Same as above but uses a caller-supplied `request_id`.
- Fails if a pending intent with that ID already exists.

#### Polling

`wasmos_sys_event_loop_poll(loop, budget)`:
- Calls `wasmos_ipc_try_recv` (non-blocking) up to `budget` times.
- For each received message:
  1. Checks the intent table: if `msg.request_id` matches a pending intent,
     fires the one-shot callback and removes the entry.
  2. Otherwise checks the handler table by `msg.type`.
  3. Falls through to the default handler if no type match.
- Returns the number of messages processed.

`budget = 0` is treated as `budget = 1` (always processes at least one
message if available).

---

### Blocking Helpers

`wasmos_sys_ipc_recv_matching(reply_endpoint, request_id, *out_reply)`:
- Loops on `wasmos_ipc_recv` (blocking), discarding messages whose
  `request_id` does not match.
- Returns 0 when the matching reply arrives, -1 on receive error.
- Used for synchronous one-shot requests where out-of-order replies must be
  filtered out without losing them.

---

### Service Lifecycle Utilities

`wasmos_sys_notify_ready(proc_endpoint, source_endpoint)`:
- Sends `PROC_IPC_NOTIFY_READY (0x20C)` to the process manager.
- Called after a service has completed initialization and is ready to serve
  requests.
- Absent this call, the sysinit `start` command blocks indefinitely waiting
  for the service to become ready.

`wasmos_sys_svc_lookup_retry(proc_endpoint, src_endpoint, name, request_id, retries)`:
- Sends `SVC_IPC_LOOKUP_REQ (0x221)` for the named service.
- Retries up to `retries` times with `sched_yield()` between attempts.
- Returns the endpoint ID on success, -1 on timeout.
- Used at service startup to locate dependent services that may not yet be
  registered.

---

### String and Encoding Utilities

`wasmos_sys_ipc_pack_name16(name, out_args[4])`:
- Packs up to 15 characters of a null-terminated string into four `int32_t`
  values (4 bytes per word, little-endian byte order within each word).
- Used to encode service names in IPC messages where the name fits in four
  argument fields.

`wasmos_sys_ipc_unpack_name16(arg0, arg1, arg2, arg3, out, out_len)`:
- Reverses the above packing.

`wasmos_sys_trim_left(s)`, `wasmos_sys_trim_right(s)`:
- In-place / pointer-advance whitespace trimming.
- Used by the rule parser and config readers.

`wasmos_sys_strcpy(dst, dst_cap, src)`:
- Null-terminated bounded string copy.

`wasmos_sha256_hex16_prefix(in, out[17])`:
- Computes a 16-character hex prefix of the SHA-256 of a string.
- Used by the device manager for rule-file fingerprinting.

---

### Multiple Loops per Service

Services with multiple concerns (e.g. device manager) maintain separate
`wasmos_sys_event_loop_t` instances on separate endpoints:

```
device_manager:
  g_dm_ipc_loop       ← main IPC endpoint (commands from other services)
  g_dm_inventory_loop ← inventory endpoint (device records from bus scanners)
  g_dm_query_loop     ← query endpoint (device info requests)
  g_dm_rules_loop     ← rules endpoint (file reads from fs manager)
```

Each loop polls its own endpoint and dispatches to its own set of handlers.
Multiple loops allow the device manager to interleave rule loading with device
arrival events without blocking either path.

---

### Native Variant

`src/libsys/native/zig/libsys.zig` provides equivalent functionality for
native ELF services (compositor, font service). It wraps the native driver
API (`wasmos_driver_api_t`) rather than raw WASM imports, but exposes the
same event loop, intent, and handler patterns.

The Zig native event loop exposes:
- `eventRegister` — registers a handler for a message type
- `eventPoll` / `eventLoop` — drives the polling/blocking loop
- `svcLookupEndpointRetry` — lookup with retry, using the native IPC send path

---

### Usage Pattern

A typical WASM service main loop:

```c
wasmos_sys_event_loop_t g_loop;
wasmos_sys_event_loop_init(&g_loop, g_ep, 1);
wasmos_sys_event_register(&g_loop, MY_IPC_REQ, handle_req, NULL);
wasmos_sys_event_set_default(&g_loop, handle_unknown, NULL);
wasmos_sys_notify_ready(proc_endpoint, g_ep);

for (;;) {
    wasmos_ipc_recv(g_ep);    // block until message
    wasmos_sys_event_loop_poll(&g_loop, 64);  // drain and dispatch
}
```

Intent-based async send:
```c
wasmos_sys_intent_send(&g_loop, dest, src,
    FS_IPC_OPEN_REQ, arg0, arg1, arg2, arg3,
    on_open_response, ctx_ptr, NULL);
// ... poll loop will fire on_open_response when reply arrives
```

---

### Structural Invariants

1. **Intents are one-shot.** The slot is freed before the callback fires.
   A callback that needs to track the response must copy the message or
   manage its own state.

2. **Handlers are registered by type, not by source.** If two services send
   the same message type, both will dispatch to the same handler. Filter by
   `msg.source` inside the handler if needed.

3. **`poll(budget)` is non-blocking.** It uses `ipc_try_recv`. For blocking
   behavior, call `ipc_recv` first (as in the usage pattern above) to ensure
   the loop wakes only when a message is available.

4. **No more than 16 in-flight intents per loop.** Services that issue many
   concurrent requests must either use multiple loops or complete outstanding
   intents before issuing new ones.

5. **`wasmos_sys_notify_ready` must be called exactly once.** The process
   manager's `start` command blocks until notified. Double-calling is harmless
   but sends an extra IPC message.
