---
name: wasmos-shared-primitives
description: Read the ownership/lifetime contract of a shared WASMOS primitive BEFORE calling it. Triggers on the SYMBOL you are about to type, not on the kind of task you are doing. Transfer buffers and DMA borrows - wasmos_xfer_buffer_acquire/borrow/reborrow/unborrow/release/read/write, wasmos_buffer_borrow, wasmos_block_buffer_phys/copy/write, wasmos_dma_map_borrow, wasmos_shmem_map, the bare xfer_buffer_* / buffer_* / dma_*_borrow / shmem_* externs the generated Zig and Rust bindings use, and the Zig wrappers bufferAcquire / bufferBorrow / bufferRead / bufferWrite / bufferRelease / blockBufferPhys. IPC endpoints and select sets - wasmos_ipc_create_endpoint, wasmos_ipc_select_one/add/wait, wasmos_ipc_send/recv/call, and the bare ipc_create_endpoint / ipc_select_* externs. Service classes - wasmos_svc_register_class, wasmos_svc_lookup_class, wasmos_svc_subscribe_class, and the Zig lookupClass / subscribeClass / registerService. Coroutines and futures - wasmos_future_*, wasmos_wasm_coroutine_*, async_initialize, wasmos_sys_wasm_async_run. Kernel object tables - idtable_*. Maps each to the architecture document that OWNS its contract, states the invariants that decide a design, and names the signals that mean you are using one the wrong way round. Use before the first call to any of them, and whenever you are about to add bookkeeping because a primitive keeps returning an error.
---

# WASMOS: Shared Primitive Contracts

## Why this exists

The contract you break is usually documented somewhere you had no reason to
open. An agent reads the architecture document for the subsystem it is
*editing*; a primitive's contract lives with the primitive it belongs to, which
is a different document, owned by a different subsystem, that no file in the
change set points at.

This has already shipped a defect. `BLOCK_IPC_IDENTIFY` was built with the
backend lending its own transfer buffer to every client — the exact inverse of
the documented model — because the implementer read `xfer_buffer.c` instead of
`docs/architecture/12-dma-transfers.md`. The kernel source answered "what
happens", the document answered "who owns it", and only the second question
decides the design.

**The trigger is the symbol you are about to type, not the task you are doing.**

## The rule

Before the first call to any primitive below, read the document that owns it.

- **The document is the authority, not the source.** The implementation tells
  you what the code does today; the contract tells you what a caller is
  promised. They differ exactly where the contract is interesting — a
  restriction the code enforces may be a documented *limitation to remove*
  rather than the rule (`09-process-and-ipc.md` says precisely this about the
  one-borrow-per-context restriction).
- **If the doc and the code disagree, that is a defect** — a `[DOCS]` or `[BUG]`
  entry in `docs/TASKS.md` — not permission to follow the code.
- **Skimming for the API is not reading the contract.** The parts that matter
  are who owns the object, who may free it, what happens on failure, and what
  the caller still owes afterwards.

## Where the contracts live

| Symbol you are about to use | Contract |
|---|---|
| `wasmos_xfer_buffer_*`, `wasmos_buffer_borrow`, `wasmos_block_buffer_*`, `wasmos_dma_*_borrow`, `wasmos_shmem_*` — and the bare `xfer_buffer_*` / `dma_*_borrow` externs in the generated Zig and Rust bindings, and the Zig wrappers `bufferAcquire` / `bufferBorrow` / `bufferRead` / `bufferWrite` / `bufferRelease` | `docs/architecture/12-dma-transfers.md` §"The object / owner / borrow model (READ THIS FIRST)" |
| `wasmos_ipc_create_endpoint`, `wasmos_ipc_select_*`, `wasmos_ipc_send` / `_call`, the bare `ipc_*` externs, endpoint lifetime, message args | `docs/architecture/09-process-and-ipc.md` |
| `wasmos_svc_register_class`, `wasmos_svc_lookup_class`, `wasmos_svc_subscribe_class`, Zig `lookupClass` / `subscribeClass` / `registerService` | `docs/architecture/09-process-and-ipc.md` §"Class-Based Discovery" |
| `wasmos_future_*`, `wasmos_wasm_coroutine_*`, `async_initialize`, `wasmos_sys_wasm_async_run`, event loops | `docs/architecture/32-coroutines-futures-promises.md` |
| `idtable_*` | `docs/architecture/35-kernel-object-tables.md` |
| `ringbuf_*` | `src/drivers/include/wasmos/ringbuf.h` is the authority; `docs/architecture/22-networking-virtio-net-and-stack.md` describes the socket data-plane use |
| Packed error codes, opcodes, generated constants | `docs/architecture/34-abi-idl-and-error-model.md` + the `wasmos-add-*` skills |

One primitive appears under several spellings on purpose. A call site reaches
it as `wasmos_xfer_buffer_borrow` from C, as a bare `xfer_buffer_borrow` extern
from the generated Zig and Rust bindings, and as `bufferBorrow` from the Zig
driver shim — and a trigger that lists only one of the three fires on the
language that happens to have been written first. A new row should carry every
spelling a caller can type.

A primitive missing from this table still has a contract. Find its document
through `docs/ARCHITECTURE.md` before calling it, and add the row.

## Transfer buffers: the invariant that decides designs

Read `12-dma-transfers.md` for the full model. The part that decides how you
shape an exchange, and the part that has already been got wrong:

**The CLIENT of an exchange owns the buffer. The server is a transient
grantee.** (`12-dma-transfers.md`, and `18-filesystem-stack.md:124` spells it
out for the FS path.)

"Client" means the side making the request, and it is a per-exchange role, not a
fixed property of a process:

- A filesystem driver asking a block driver to describe a disk is the client →
  **the filesystem driver** owns that buffer.
- A block driver announcing itself to device-manager is the client of
  device-manager → **the block driver** owns that buffer.

The same process is therefore the owner in one exchange and a grantee in
another. Ask "who initiated this request?", not "who is the driver?".

**A server cannot own a buffer it hands to a client.** This is not a style
preference, it is unimplementable:

- `xfer_buffer_release` is **owner-only**.
- **No hostcall transfers ownership.** The kernel has a set-owner function; it
  is not in the guest ABI.
- **Nothing tells a server when a client has finished reading.** There is no
  unborrow notification.

So a server that lends its own buffer can never free it, never know when it is
safe to reuse it, and must never rewrite it while lent — which also makes
whatever it holds a snapshot rather than live state.

The client-owned shape has none of those problems and needs no bookkeeping:

```c
bid = wasmos_xfer_buffer_acquire(sizeof(record));          /* client owns it   */
wasmos_xfer_buffer_borrow(server_ep, bid, WASMOS_BUFFER_GRANT_WRITE); /* lend it  */
wasmos_ipc_send(server_ep, reply, OP_REQ, id, arg0, bid, 0, 0);
/* ... server writes into it and replies ... */
wasmos_xfer_buffer_read(bid, &record, sizeof(record), 0);
wasmos_xfer_buffer_release(bid);   /* cascade-revokes the server's grant */
```

`release` cascade-revokes every borrow beneath it, so there is nothing to clean
up on any path — including the error paths.

## Signals you are holding a primitive backwards

Stop and re-read the contract when any of these appear. They are not problems to
solve; they are the primitive telling you the design is inverted.

- **You are adding a table, cache, retry or special case because a primitive
  "keeps failing" in a legitimate-looking way.** `ALREADY_BORROWED` from a
  transfer buffer means the buffer is on the wrong side of the exchange. A
  per-client grant table to route around it shipped once and was reverted.
- **You cannot say who frees the object.** If the answer involves "the other
  side will probably be done by then", the ownership is wrong.
- **You need a rule like "must not rewrite this while lent."** A per-request,
  client-owned buffer has no such rule.
- **You are tracking peers by endpoint** where the kernel tracks them by
  context. Several endpoints belong to one process; a table keyed on the wrong
  one disagrees with the kernel for any client holding more than one.

## Checklist before the first call

1. Found the owning document in the table above (or through
   `docs/ARCHITECTURE.md`) and read the ownership/lifetime section.
2. Named the client of the exchange — the side making the request — and given
   it the buffer.
3. Named who releases the object, on the success path and on every error path.
4. Confirmed the contract permits what you are about to do, rather than
   confirming the current implementation happens to allow it.
