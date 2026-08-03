## Transfer Buffers & DMA

> **Documentation status: Mixed reference and proposal.** Borrow-based DMA and
> driver-owned pinned regions are implemented; IOMMU, non-coherent cache, and
> additional device-DMA work remain deferred.

This document is the authoritative contract for the transfer-buffer subsystem:
the object/owner/borrow capability model, the roles (grantor, borrower/grantee,
mapper), the stateless id-based ABI, the lifecycle rules, and the DMA transfer
model layered on top. The kernel core lives in
`src/kernel/xfer_buffer/xfer_buffer.c` (`src/kernel/include/xfer_buffer.h`); the
wasm ABI is wired in `src/kernel/wasm3/link.c` and `src/kernel/warp/link.cpp`.

---

### The object / owner / borrow model (READ THIS FIRST)

A **transfer buffer is a first-class kernel object** with a stable `buffer_id`.
Userspace holds ids like file descriptors and passes them back over IPC; the
kernel keeps **no per-context "current buffer" state** — every call names its
object/handle by id and is re-resolved. There is exactly one model; the old
"one borrow slot per (kind, context)" singleton is gone.

**Roles** (a single context can be several of these at once, for different objects):

- **Owner** — the context that `acquire`d the object. It holds the object's
  *lifecycle*: it alone `release`s it. Ownership is the answer to "who knows when
  this buffer is truly done?" — for FS reads that's the **application** (it reads
  the result out after the syscall returns), so the app owns.
- **Grantor / lender** — the context that created a particular *(re)borrow*. For a
  top-level `borrow`, the grantor is the owner. For a `reborrow`, the grantor is
  the upstream borrower. **The grantor is the only context that may `unborrow`
  that (re)borrow.**
- **Borrower / grantee** — the context a (re)borrow grants access *to*. It may
  read/write the object (subject to its rights) and may itself `reborrow`
  downstream, but it **never** unborrows a grant it did not create.
- **Mapper** — a driver context that maps a DMA window on a borrow it holds. See
  DMA below; the mapper is always a borrower with `CAP_DMA_BUFFER`.

**The one rule that generates everything else:** *whoever creates a thing is the
only one who tears it down; teardown cascades downstream, never upstream.*

- Owner creates the object → owner `release`s it. `release` is a **transient
  teardown**: it cascade-revokes every outstanding borrow/reborrow (and clears
  their DMA) in one sweep. The owner need not wait for borrowers to unborrow.
- Grantor creates a (re)borrow → grantor `unborrow`s it (authorized by
  `xfer_buffer_get_lent`, which checks the caller is the borrow's *lender*). An
  `unborrow` cascade-revokes only the reborrows **downstream** of it — never the
  grant it came from.
- Mapper creates a DMA window → mapper `dma_unmap`s it (authorized by
  `xfer_buffer_get_borrowed`, which checks the caller is the *borrower*).

**Rights** are `BUFFER_BORROW_READ (0x1)` / `BUFFER_BORROW_WRITE (0x2)`. A
`reborrow`'s rights must be a subset of the upstream borrow's
(`RIGHTS_AMPLIFICATION` otherwise). A borrower holds at most one active borrow
per object (`ALREADY_BORROWED` otherwise) — so a relay that reborrows to the
same backend across many requests must unborrow each reborrow before the next.

**Kinds:** `BUFFER_KIND_TRANSFER (1)` is generic transferable bulk data;
`BUFFER_KIND_FRAMEBUFFER (2)` is local-only backing (owner-self-borrow only, not
transferable, not reborrowable).

---

### Design Principles

- **Owning a transfer buffer is fd-like; DMA is privileged.** Any real process
  may `acquire`/`borrow`/`reborrow`/`release`/`unborrow` a transfer buffer (moving
  IPC payloads is ordinary). **DMA** — mapping a buffer for a device — is the only
  privileged step and is gated by `CAP_DMA_BUFFER` (`require_dma_capability` in
  the shim). Because only a capability-holding driver can `dma_map`, DMA lives
  **only in drivers**, never in the app or the fs-manager.
- **DMA window ≠ buffer lifecycle.** A DMA mapping is a *transient, operational*
  attachment scoped to a single device access: the driver maps it right before
  programming the device and unmaps it right after the transfer, inside its
  handling of one request. It is the mapper's concern, not the owner's/lender's.
  A borrow **cannot be unborrowed while its DMA is still mapped** — the mapper
  unmaps first.
- **Least privilege / deny by default (DMA).** Each DMA grant is bounded by
  direction flags, a maximum byte count, and an approved physical-address window
  list; no DMA without `CAP_DMA_BUFFER` and a matching window.
- **Deterministic teardown.** On process exit, `xfer_buffer_drop_context`
  force-revokes every borrow the context issued or holds (and their DMA), and
  destroys objects it owned.
- **No IOMMU required.** The baseline uses physical addresses directly; a future
  IOMMU backend would return an IOVA from the DMA-map path while keeping the
  driver-facing ABI unchanged.
  TODO: introduce IOVA domain model when VT-d/AMD-Vi support is added.

---

### Stateless id-based ABI (wasm imports, `wasmos` module)

Declared in `src/libc/include/wasmos/api.h`. All return `>= 0` on success
(buffer_id / borrow_id / device address / 0) and a negative
`xfer_buffer_status_t` on failure.

```c
xfer_buffer_acquire(min_size)                 -> buffer_id   // OWNER creates an object
xfer_buffer_borrow(grantee_endpoint, buffer_id, flags) -> borrow_id
                                              // OWNER grants the context owning grantee_endpoint
xfer_buffer_reborrow(grantee_endpoint, borrow_id, flags) -> borrow_id
                                              // a BORROWER sub-grants (rights ⊆ its own)
xfer_buffer_read(buffer_id, ptr, len, off)    // owner OR any grantee with READ
xfer_buffer_write(buffer_id, ptr, len, off)   // owner OR any grantee with WRITE
xfer_buffer_unborrow(borrow_id)               // the (re)borrow's GRANTOR only (get_lent)
xfer_buffer_release(buffer_id)                // OWNER only; cascade-revokes all borrows
// DMA (driver-only, CAP_DMA_BUFFER):
dma_map_borrow(borrow_id, off, len, dir) -> device_addr   // MAPPER (borrower) maps
dma_sync_borrow(borrow_id, off, len, sync_op)
dma_unmap_borrow(borrow_id)                               // MAPPER unmaps its own window
```

The generic `buffer_*` variants take a leading `kind` arg; the `xfer_buffer_*`
forms above are `BUFFER_KIND_TRANSFER` shorthands. Kernel authority resolvers:
`xfer_buffer_get_owned` (owner), `xfer_buffer_get_lent` (grantor/lender — used by
`unborrow`), `xfer_buffer_get_borrowed` (borrower — used by DMA), `describe`
(owner-or-borrower, used by read/write).

### Usage examples

**FS read/write relay (app → fs-manager → backend).** The app owns and holds the
lifecycle; fs-manager is a transient borrower that reborrows to the backend.

```
app         acquire(sz)=buf ; borrow(fs.vfs_ep, buf, R|W)=b1
            send FS_IPC_READ_REQ(fd, len, arg2=buf, arg3=b1)   // reuse buf+b1 across chunks
fs-manager  reborrow(backend_ep, b1, R|W)=b2                    // grantor of b2
            forward to backend (arg2=buf, arg3=b2)
backend     read/write(buf, ...)                                // grantee; no borrow, no unborrow
fs-manager  unborrow(b2)                                        // its own reborrow, per request
app         (repeat...) release(buf)                            // cascade-revokes b1 (and any b2)
```

Rules exercised: app owns + reuses one grant (never re-grants per chunk — that
would hit `ALREADY_BORROWED`); fs-manager only unborrows the reborrow it created;
the app's `release` is the whole-tree teardown; the backend never unborrows.

**Path ops (open/stat/unlink/…), owner + kernel-read.** For spawn paths and
service-register descriptors the *kernel* (PM) reads the caller's buffer directly
by ownership (`pm_foreign_xfer_ptr`) — no grant needed. The owner just
`acquire` → write path → send `buffer_id` → `release`.

**Disk DMA (driver, transient window).** Only the driver may map:

```
backend(fat_fs/ata)  b2 already granted (R|W) by fs-manager
                     dma_map_borrow(b2, off, len, dir)=device_addr   // CAP_DMA_BUFFER
                     program device ; wait for completion
                     dma_sync_borrow / dma_unmap_borrow(b2)          // mapper unmaps its window
                     reply
fs-manager           unborrow(b2)                                    // DMA already gone
```

A borrow can't be unborrowed while its DMA is mapped, so the mapper must unmap
before its lender tears the borrow down.

---

### Constants

Defined in `src/drivers/include/wasmos_driver_abi.h`:

```c
/* Direction flags (bitfield) */
WASMOS_DMA_DIR_TO_DEVICE   = 1 << 0   // driver → device
WASMOS_DMA_DIR_FROM_DEVICE = 1 << 1   // device → driver
WASMOS_DMA_DIR_BIDIR       = 3        // both directions

/* Status codes returned by DMA hostcalls */
WASMOS_DMA_STATUS_OK          =  0
WASMOS_DMA_STATUS_DENY        = -1   // capability check failed
WASMOS_DMA_STATUS_INVALID     = -2   // bad argument / wrong state
WASMOS_DMA_STATUS_RANGE       = -3   // out-of-window or oversize
WASMOS_DMA_STATUS_UNAVAILABLE = -4   // no active borrow or not mapped

/* Sync operations */
WASMOS_DMA_SYNC_TO_DEVICE   = 1
WASMOS_DMA_SYNC_FROM_DEVICE = 2
WASMOS_DMA_SYNC_BIDIR       = 3

/* IPC opcodes (PROC namespace, 0x230–0x2BF) */
PROC_IPC_DMA_MAP_BORROW_REQ   = 0x230
PROC_IPC_DMA_SYNC_BORROW_REQ  = 0x231
PROC_IPC_DMA_UNMAP_BORROW_REQ = 0x232
PROC_IPC_DMA_BORROW_RESP      = 0x2B0
PROC_IPC_DMA_BORROW_ERROR     = 0x2BF
```

Buffer kind and grant constants, defined in `src/libc/include/wasmos/api.h`:

```c
WASMOS_BUFFER_KIND_XFER   = 1
WASMOS_BUFFER_GRANT_READ  = 0x1
WASMOS_BUFFER_GRANT_WRITE = 0x2
```

Architectural buffer-kind constants:

```c
TRANSFER_BUFFER_KIND    = 1u
FRAMEBUFFER_BUFFER_KIND = 2u
BUFFER_BORROW_READ      = 0x1u
BUFFER_BORROW_WRITE     = 0x2u
PM_FS_BUFFER_SIZE       = 2 MiB (2u * 1024u * 1024u)
```

Limits, defined in `src/kernel/include/process_manager_internal.h` and
`src/kernel/include/capability.h`:

```c
PM_DMA_WINDOW_LIMIT         = 16u   // per spawn profile
CAPABILITY_DMA_WINDOW_LIMIT = 16   // per capability context
```

---

### Data Structures

#### `wasmos_dma_window_t` (`wasmos_driver_abi.h`)

A single approved physical-address range:

```c
typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
} wasmos_dma_window_t;
```

#### `wasmos_spawn_dma_caps_t` / `wasmos_spawn_caps_v2_t` (`wasmos_driver_abi.h`)

Spawn-time capability payload carrying DMA grants:

```c
typedef struct __attribute__((packed)) {
    uint32_t direction_flags;
    uint32_t max_bytes;
    uint32_t window_count;
    uint32_t reserved0;
} wasmos_spawn_dma_caps_t;

typedef struct __attribute__((packed)) {
    uint32_t cap_flags;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint16_t irq_mask;
    uint16_t reserved0;
    wasmos_spawn_dma_caps_t dma;
    wasmos_dma_window_t windows[];   // window_count entries follow inline
} wasmos_spawn_caps_v2_t;

#define WASMOS_SPAWN_CAPS_V2_SIZE(window_count) \
    (sizeof(wasmos_spawn_caps_v2_t) + (window_count) * sizeof(wasmos_dma_window_t))
```

#### `pm_spawn_caps_t` (`process_manager_internal.h`)

Kernel's parsed representation of a driver's spawn capability profile:

```c
typedef struct {
    uint8_t  valid;
    uint32_t cap_flags;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint16_t irq_mask;
    uint32_t dma_direction_flags;
    uint32_t dma_max_bytes;
    uint32_t dma_window_count;
    wasmos_dma_window_t dma_windows[PM_DMA_WINDOW_LIMIT];
} pm_spawn_caps_t;
```

#### xfer-buffer Registry Objects

The xfer-buffer registry (`src/kernel/xfer_buffer/xfer_buffer.c`) is a single,
spinlock-guarded (`g_xfer_lock`) store of buffer objects. There is no
per-context "slot" and no one-borrow-per-context limit; objects and borrows are
independent handles keyed by stateless ids. The relevant descriptors
(`src/kernel/include/xfer_buffer.h`) are:

```c
typedef struct { ... } xfer_buffer_t;             // kind, buffer_id, size_bytes
typedef struct { ... } xfer_buffer_owner_t;       // owner_context_id + buffer
typedef struct { ... } xfer_buffer_borrow_t;      // lender, borrower, flags, borrow_id
typedef struct { ... } xfer_buffer_dma_mapping_t; // offset, length, direction, dev addr, active
```

Each object is owned by one context (identified by `buffer_id`); borrow edges
form a directed acyclic graph (a borrower may `reborrow` downstream). DMA state
lives on an `xfer_buffer_dma_mapping_t` attached to an owner or a borrow, not on
a context slot.

---

### Capability Enforcement

Defined in `src/kernel/include/capability.h` and
`src/kernel/capability.c`. The kernel's per-context DMA state is:

```c
// within capability_context_state_t
uint32_t dma_direction_flags;
uint32_t dma_max_bytes;
uint32_t dma_window_count;
wasmos_dma_window_t dma_windows[CAPABILITY_DMA_WINDOW_LIMIT];
```

Three capability checks are performed at every DMA map call:

| Check     | Function                                                 | Logic                                                                      |
|-----------|----------------------------------------------------------|----------------------------------------------------------------------------|
| Direction | `capability_dma_direction_allowed(ctx, direction_flags)` | `(ctx->dma_direction_flags & direction_flags) == direction_flags`          |
| Window    | `capability_dma_range_allowed(ctx, base, length)`        | `base >= w.base && base+length <= w.base+w.length` for at least one window |
| Size      | `capability_dma_max_bytes(ctx)`                          | `length <= ctx->dma_max_bytes`                                             |

The capability kind `CAP_DMA_BUFFER = 3` must be present in the calling
context's capability flags (`capability_has`). Any missing capability
returns `WASMOS_DMA_STATUS_DENY`.

---

### Hostcall API

Declared in `src/libc/include/wasmos/api.h`; implemented in
`src/kernel/wasm3/link.c`. All three functions are Wasm imports under
the `"wasmos"` module namespace.

#### `wasmos_dma_map_borrow`

```c
// api.h declaration — id-based: the MAPPER names its own borrow by borrow_id.
extern int32_t wasmos_dma_map_borrow(
    int32_t borrow_id,
    int32_t offset,
    int32_t length,
    int32_t direction_flags)
    WASMOS_WASM_IMPORT("wasmos", "dma_map_borrow");  // wasm3 sig: "i(iiii)"
```

Returns the physical device address (a non-negative `int32_t`) on
success, or a `WASMOS_DMA_STATUS_*` error on failure.

Validation sequence (in order):

1. `borrow_id > 0`, `offset >= 0`, `length > 0`, `direction_flags > 0` →
   `WASMOS_DMA_STATUS_INVALID` if any fail.
2. Caller has `CAP_DMA_BUFFER` capability (`require_dma_capability`) →
   `WASMOS_DMA_STATUS_DENY` if absent.
3. `xfer_buffer_get_borrowed(borrow_id, caller_ctx, &borrow, &mapping)` — the
   caller must be the borrow's **borrower** (mapper) → `DENY` otherwise.
4. `capability_dma_direction_allowed` (direction ⊆ the borrow's rights via
   `dma_map_borrow`) → `WASMOS_DMA_STATUS_DENY`.
5. `length <= dma_max_bytes` → `WASMOS_DMA_STATUS_RANGE`.
6. `xfer_buffer_dma_map_borrow` (range validation + phys computation) →
   `WASMOS_DMA_STATUS_DENY` on failure.
7. `capability_dma_range_allowed(ctx, device_addr, length)` → `WASMOS_DMA_STATUS_RANGE`.
8. `device_addr <= 0x7FFFFFFF` (must fit in positive signed 32-bit) →
   `WASMOS_DMA_STATUS_RANGE`.

#### `wasmos_dma_sync_borrow`

```c
extern int32_t wasmos_dma_sync_borrow(
    int32_t borrow_id,
    int32_t offset,
    int32_t length,
    int32_t sync_op)
    WASMOS_WASM_IMPORT("wasmos", "dma_sync_borrow");  // wasm3 sig: "i(iiii)"
```

Valid `sync_op` values: `WASMOS_DMA_SYNC_TO_DEVICE`, `FROM_DEVICE`,
`BIDIR`. Returns `WASMOS_DMA_STATUS_OK` or an error code.

The kernel calls `xfer_buffer_dma_sync`. On x86, cache
maintenance is a no-op; the call still enforces that a mapping is
active and that the requested range falls within `dma_length`.

#### `wasmos_dma_unmap_borrow`

```c
extern int32_t wasmos_dma_unmap_borrow(int32_t borrow_id)
    WASMOS_WASM_IMPORT("wasmos", "dma_unmap_borrow");  // wasm3 sig: "i(i)"
```

Returns `WASMOS_DMA_STATUS_OK` or an error code. Clears all DMA metadata on the
borrow's mapping without releasing the borrow itself.

---

### Kernel xfer-buffer DMA Layer

Implemented in `src/kernel/xfer_buffer/xfer_buffer.c`; declared in
`src/kernel/include/xfer_buffer.h`. All calls run under the registry
spinlock (`g_xfer_lock`).

#### `xfer_buffer_dma_map_owned` / `xfer_buffer_dma_map_borrow`

```c
int xfer_buffer_dma_map_owned(const xfer_buffer_owner_t *owner,  ...,
                              xfer_buffer_dma_mapping_t *out_mapping);
int xfer_buffer_dma_map_borrow(const xfer_buffer_borrow_t *borrow, ...,
                               xfer_buffer_dma_mapping_t *out_mapping);
```

DMA is attached to a resolved handle — an owner (owner-initiated) or a borrow
(borrower-initiated, resolved by `borrow_id` via `xfer_buffer_get_borrowed`) —
not to a `(kind, context)` slot. Preconditions: the handle is valid, no mapping
is already active on it, and `offset + length <= object size`
(page-rounded at acquire; framebuffer size from boot-info). On success the
mapping records direction/offset/length and the device address
(`object phys + offset`).

#### `xfer_buffer_dma_sync`

Validates the mapping is active and the requested offset+length falls within the
originally mapped range. The sync operation itself is a no-op on x86 (no
explicit cache flush required).

#### `xfer_buffer_dma_unmap`

Clears the DMA metadata on the mapping. It does not release the borrow or
object; those are released via `xfer_buffer_unborrow` / `xfer_buffer_release`.
Releasing an object or unborrowing a handle with a still-active mapping
cascade-revokes the mapping.

---

### DMA Lifecycle State Machine

A borrow held by the mapper progresses through these DMA states (the borrow
itself is created owner-side via `xfer_buffer_borrow`, which hands the mapper a
`borrow_id`):

```
BORROWED ──[wasmos_dma_map_borrow]──► DMA_MAPPED
  │                                       │
  │   DMA_MAPPED ──[wasmos_dma_sync_borrow]──► DMA_MAPPED
  │              (TO_DEVICE before hardware, FROM_DEVICE after)
  │                                       │
  │   DMA_MAPPED ──[wasmos_dma_unmap_borrow]──► BORROWED
  │
BORROWED ──[owner xfer_buffer_release / mapper xfer_buffer_unborrow]──► gone
```

Constraints:
- Releasing an object or unborrowing a handle with an active mapping
  cascade-revokes the mapping (see the DMA layer above).
- On process exit, `xfer_buffer_drop_context` force-revokes all of the
  context's mappings, borrows, and owned objects.
- A new spawn receives fresh registry state with no residual mappings
  from a previous driver instance.

---

### Driver-Owned DMA Regions

The borrow-based model above maps a *peer's transient* buffer. Some devices
need the opposite: memory the driver **owns and pins for its whole lifetime**,
which the device DMAs to/from continuously. The canonical case is **virtqueue
rings** (see [Networking](22-networking-virtio-net-and-stack.md)), but the same
primitive generalizes to block-DMA staging and framebuffer/scanout regions.

This is implemented as the `region_alloc` hostcall in both runtimes
(`warp_region_alloc` in `src/kernel/warp/link.cpp`,
`wasmos_region_alloc` in `src/kernel/wasm3/link.c`). WARP performs a real
linear-memory page remap onto the allocated physical run; wasm3 allocates the
same low-physical run, maps it into the process's linear-memory VA window, and
reclaims those driver-owned regions on process reap. In both cases the
allocation is gated by `CAP_DMA_BUFFER` plus the caller's approved DMA window.
The remaining design context below is retained as rationale.

Such memory cannot come from the driver's WASM linear memory: linmem pages are
not guaranteed physically contiguous, the WARP linmem base is not page-aligned,
and it can relocate on growth. It must instead be a **kernel-reserved physical
region**. The building blocks already exist and only need composing into one
hostcall:

- `pfa_alloc_pages_below(pages, limit)` (`src/kernel/physmem.c`) — returns a
  **contiguous, page-aligned** physical run below a ceiling (use the low-2GB
  limit so the legacy virtqueue PFN register and the signed-32-bit
  `device_addr` contract hold).
- `pfa_pin_pages(base, pages)` (`src/kernel/memory.c`) — pins the run so it is
  never reused or relocated.
- `wasmos_phys_map(phys, size, wasm_offset)` (`src/kernel/wasm3/link.c`,
  `src/kernel/warp/link.cpp`) — maps the region into the driver's linear memory
  so it can read/write descriptors.

The surface is a thin allocator that composes these under the `CAP_DMA_BUFFER`
gate plus the driver's approved DMA window, returning both a linmem pointer and
a stable physical address:

```c
/* returns the wasm linmem offset of the mapped region (>= 0), or a negative
 * WASMOS_DMA_STATUS_* ; writes the u64 physical base to *out_phys. */
int32_t wasmos_region_alloc(int32_t pages, int32_t cache_policy, uint64_t *out_phys);
```

The mapping is a real page remap (via the same pinned-base linmem window
machinery as `shmem_map_auto`, factored into `warp_linmem_place_phys`), so the
driver's writes land in the exact physical pages the device DMAs from — not a
copy. The backing run is `pfa_alloc_pages_below(pages, 2 GiB)` + `pfa_pin_pages`,
and `capability_dma_range_allowed` is enforced on the allocation just like on
borrow mappings.

Design notes:

- **Region object, not just a buffer.** The handle should carry
  `{ backing: alloc | mmio, phys, pages, pinned, cache_policy, dma_capable,
  shareable }` so the same abstraction covers allocated DMA regions,
  fixed-MMIO ranges (framebuffer, mapped via `phys_map` on a firmware-given
  address rather than `pfa_alloc`), and shared regions.
- **Cache policy is a first-class attribute.** DMA rings on x86 are cached and
  hardware-coherent (write-back, only compiler/memory barriers needed);
  framebuffers want write-combining. A single allocator must set the correct
  PAT/PTE attributes per region.
- **`block_buffer_phys` is the existing precedent** for handing a driver a
  fixed low-physical DMA buffer (`src/kernel/wasm3/link.c`); the region
  allocator generalizes it rather than replacing the borrow path.
- **Keep the guards.** `capability_dma_range_allowed` and the low-2GB clamp
  must apply to allocated regions exactly as they do to borrow mappings — a
  general "give me physical memory" primitive without capability gating is a
  DMA-anywhere hole.
- **Scope.** The current `region_alloc` is a one-shot pinned reservation (no
  free/reuse), sufficient for a fixed set of devices; a real region lifecycle
  (free, refcount, revoke) and write-combining cache policy are follow-ons.
  `virtio-net` uses `region_alloc` for virtqueue rings and packet pools (see
  Networking Phase 1). Region free/revoke and write-combining PAT support
  remain future work.

---

### ATA Storage Integration

> **Currently deferred/aspirational.** The borrow-based DMA fast-path below is
> disabled pending the owner-push migration: `ata_dma_prepare()` returns
> `WASMOS_DMA_STATUS_DENY` unconditionally (`src/drivers/ata/ata.c`), so every
> ATA read and write is presently PIO (matching invariant 5, "PIO fallback is
> unconditional"). The lifecycle described here is the intended shape once the
> block IPC protocol carries the buffer grant.

The ATA driver (`src/drivers/ata/ata.c`) is written around the full DMA
lifecycle on every read and write request.

#### Helper Functions

**`ata_dma_prepare(source_endpoint, offset, length, direction_flags, *out_device_addr)`**

```
wasmos_buffer_borrow(WASMOS_BUFFER_KIND_XFER, source_endpoint,
    direction == FROM_DEVICE ? GRANT_WRITE : GRANT_READ)
→ wasmos_dma_map_borrow(WASMOS_BUFFER_KIND_XFER, source_endpoint,
    offset, length, direction_flags)
→ [if TO_DEVICE] wasmos_dma_sync_borrow(WASMOS_BUFFER_KIND_XFER,
    offset, length, WASMOS_DMA_SYNC_TO_DEVICE)
```

Returns `WASMOS_DMA_STATUS_OK` on success. On any failure, releases
the buffer borrow before returning the error code.

**`ata_dma_finish(source_endpoint, offset, length, direction_flags)`**

```
[if FROM_DEVICE] wasmos_dma_sync_borrow(WASMOS_BUFFER_KIND_XFER,
    offset, length, WASMOS_DMA_SYNC_FROM_DEVICE)
→ wasmos_dma_unmap_borrow(WASMOS_BUFFER_KIND_XFER, source_endpoint)
→ wasmos_buffer_release(WASMOS_BUFFER_KIND_XFER)
```

#### Read Path (`BLOCK_IPC_READ_REQ`)

```
ata_dma_prepare(source, 0, sector_count * 512, WASMOS_DMA_DIR_FROM_DEVICE, &addr)
  → on success: ata_read_lba28()
               ata_dma_finish(..., FROM_DEVICE)
  → on failure: ata_read_lba28()   // PIO fallback, no DMA lifecycle
```

#### Write Path (`BLOCK_IPC_WRITE_REQ`)

```
ata_dma_prepare(source, 0, sector_count * 512, WASMOS_DMA_DIR_TO_DEVICE, &addr)
  → on success: ata_write_lba28()
               ata_dma_finish(..., TO_DEVICE)
  → on failure: ata_write_lba28()  // PIO fallback, no DMA lifecycle
```

#### Observability Markers

One-shot markers are logged at most once per direction per process lifetime:

| Marker                            | Meaning                                        |
|-----------------------------------|------------------------------------------------|
| `[ata] dma read path active`      | First successful DMA lifecycle on a read       |
| `[ata] dma write path active`     | First successful DMA lifecycle on a write      |
| `[ata] dma read fallback rc=<n>`  | First failed DMA prep on a read; PIO proceeds  |
| `[ata] dma write fallback rc=<n>` | First failed DMA prep on a write; PIO proceeds |

The one-shot flags (`g_dma_read_ok_logged`, etc.) prevent log storms on
repeated operations. The `rc` value is the `WASMOS_DMA_STATUS_*` code
from `ata_dma_prepare`.

---

### Structural Invariants

1. **CAP_DMA_BUFFER is the gate.** `wasmos_dma_map_borrow` checks this
   capability before any other state. A driver spawned without it always
   gets `WASMOS_DMA_STATUS_DENY` regardless of borrow state.

2. **One mapping per slot at a time.** `xfer_buffer_dma_map`
   rejects a second map call while `dma_mapped` is set. The driver must
   call `dma_unmap_borrow` before re-mapping the same borrow.

3. **Window check is at map time, not sync time.** The physical address
   range is validated against `capability_dma_range_allowed` once during
   `dma_map_borrow`. Subsequent sync calls verify only that the range
   falls within the already-mapped `dma_length`.

4. **Sync is a no-op on x86.** `xfer_buffer_dma_sync` enforces
   state/range semantics but does not issue cache maintenance instructions.
   This is correct for x86 coherent DMA. Non-coherent architectures
   would need explicit flushes here.
   TODO: add explicit flush hooks when porting to non-coherent targets.

5. **PIO fallback is unconditional.** ATA read/write proceeds via PIO
   whether or not DMA setup succeeded. The DMA lifecycle only adds
   borrow-ownership semantics and observability markers; it does not
   change the actual data-transfer path (hardware still uses PIO registers).
   TODO: integrate real bus-master DMA (PRDT/descriptor) once IOMMU
   or IOVA support is added.

6. **No framebuffer DMA integration.** The framebuffer buffer kind
   (`FRAMEBUFFER_BUFFER_KIND = 2`) has PM-layer DMA map/unmap support
   wired through the same transfer-buffer slot path, but no driver
   currently calls the DMA hostcalls for framebuffer transfers.
