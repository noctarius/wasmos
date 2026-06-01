## DMA Transfer Support

This document describes the DMA transfer model: its constants, data structures,
capability enforcement, hostcall API, kernel buffer-layer implementation, and
the ATA storage integration that exercises the full lifecycle.

---

### Design Principles

- **Deny by default.** No DMA operation succeeds without explicit
  `CAP_DMA_BUFFER` capability and a matching window profile.
- **Reuse borrow objects.** DMA state is attached to existing PM buffer-borrow
  slots. No separate DMA region allocator is introduced.
- **Least privilege.** Each grant is bounded by direction flags, maximum byte
  count, and an approved physical-address window list.
- **Deterministic teardown.** On driver crash or kill, all mapped DMA borrows
  are force-unmapped by the process exit path.
- **No IOMMU required.** The current baseline uses physical addresses directly.
  A future IOMMU backend would replace `out_device_addr` with an IOVA while
  keeping the driver-facing hostcall ABI unchanged.
  TODO: introduce IOVA domain model when VT-d/AMD-Vi support is added.

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
WASMOS_BUFFER_KIND_FS    = 1
WASMOS_BUFFER_GRANT_READ  = 0x1
WASMOS_BUFFER_GRANT_WRITE = 0x2
```

Kernel-side buffer kind constants, defined in `src/kernel/include/process_manager.h`:

```c
PM_BUFFER_KIND_FILESYSTEM = 1u
PM_BUFFER_KIND_FRAMEBUFFER = 2u
PM_BUFFER_BORROW_READ  = 0x1u
PM_BUFFER_BORROW_WRITE = 0x2u
PM_FS_BUFFER_SIZE = 2 MiB (2u * 1024u * 1024u)
```

Limits, defined in `src/kernel/include/process_manager_internal.h` and
`src/kernel/include/capability.h`:

```c
PM_DMA_WINDOW_LIMIT        = 16u   // per spawn profile
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

#### `pm_fs_buffer_slot_t` (`process_manager_buffers.c`)

Per-context buffer slot tracking active borrow and any attached DMA mapping:

```c
typedef struct {
    uint8_t  in_use;
    uint32_t context_id;
    uint64_t buffer_phys;              // physical base of the slot's buffer
    uint8_t  borrow_active;            // nonzero when a borrow is in progress
    uint8_t  borrow_flags;             // PM_BUFFER_BORROW_READ / WRITE
    uint32_t borrow_source_context_id; // context that issued the borrow
    uint8_t  dma_mapped;               // nonzero when dma_map_borrow has succeeded
    uint32_t dma_direction_flags;
    uint32_t dma_offset;
    uint32_t dma_length;
} pm_fs_buffer_slot_t;
```

Two separate lists exist: `g_pm_fs_slots` (filesystem kind) and
`g_pm_fb_slots` (framebuffer kind). Each list is a chunked array of 16
initial entries.

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
`src/kernel/wasm3_link.c`. All three functions are Wasm imports under
the `"wasmos"` module namespace.

#### `wasmos_dma_map_borrow`

```c
// api.h declaration
extern int32_t wasmos_dma_map_borrow(
    int32_t borrow_kind,
    int32_t source_endpoint,
    int32_t offset,
    int32_t length,
    int32_t direction_flags)
    WASMOS_WASM_IMPORT("wasmos", "dma_map_borrow");  // wasm3 sig: "i(iiiii)"
```

Returns the physical device address (a non-negative `int32_t`) on
success, or a `WASMOS_DMA_STATUS_*` error on failure.

Validation sequence (in order):

1. `kind >= 0`, `offset >= 0`, `length > 0`, `direction_flags > 0` →
   `WASMOS_DMA_STATUS_INVALID` if any fail.
2. Caller has `CAP_DMA_BUFFER` capability →
   `WASMOS_DMA_STATUS_DENY` if absent.
3. Resolved endpoint owner is not 0 and not the calling context →
   `WASMOS_DMA_STATUS_INVALID`.
4. Endpoint owner matches `borrow_source_context` on the slot →
   `WASMOS_DMA_STATUS_INVALID` (cross-context borrow forbidden).
5. Borrow direction flags: `WASMOS_DMA_DIR_TO_DEVICE` requires
   `PM_BUFFER_BORROW_READ` on the slot; `WASMOS_DMA_DIR_FROM_DEVICE`
   requires `PM_BUFFER_BORROW_WRITE` →
   `WASMOS_DMA_STATUS_DENY` on mismatch.
6. `capability_dma_direction_allowed` → `WASMOS_DMA_STATUS_DENY`.
7. `length <= dma_max_bytes` → `WASMOS_DMA_STATUS_RANGE`.
8. `process_manager_buffer_dma_map` (slot state/range validation + phys
   address computation) → `WASMOS_DMA_STATUS_UNAVAILABLE` on failure.
9. `capability_dma_range_allowed(ctx, device_addr, length)` → `WASMOS_DMA_STATUS_RANGE`.
10. `device_addr <= 0x7FFFFFFF` (must fit in positive signed 32-bit) →
    `WASMOS_DMA_STATUS_RANGE`.

#### `wasmos_dma_sync_borrow`

```c
extern int32_t wasmos_dma_sync_borrow(
    int32_t borrow_kind,
    int32_t offset,
    int32_t length,
    int32_t sync_op)
    WASMOS_WASM_IMPORT("wasmos", "dma_sync_borrow");  // wasm3 sig: "i(iiii)"
```

Valid `sync_op` values: `WASMOS_DMA_SYNC_TO_DEVICE`, `FROM_DEVICE`,
`BIDIR`. Returns `WASMOS_DMA_STATUS_OK` or an error code.

The kernel calls `process_manager_buffer_dma_sync`. On x86, cache
maintenance is a no-op; the call still enforces that a mapping is
active and that the requested range falls within `dma_length`.

#### `wasmos_dma_unmap_borrow`

```c
extern int32_t wasmos_dma_unmap_borrow(
    int32_t borrow_kind,
    int32_t source_endpoint)
    WASMOS_WASM_IMPORT("wasmos", "dma_unmap_borrow");  // wasm3 sig: "i(ii)"
```

Returns `WASMOS_DMA_STATUS_OK` or an error code. Clears `dma_mapped`
and all DMA metadata on the slot without releasing the borrow itself.

---

### Kernel PM Buffer DMA Layer

Implemented in `src/kernel/process_manager_buffers.c`.

#### `process_manager_buffer_dma_map`

```c
int process_manager_buffer_dma_map(
    uint32_t kind,
    uint32_t borrower_context_id,
    uint32_t source_context_id,
    uint32_t offset,
    uint32_t length,
    uint32_t direction_flags,
    uint64_t *out_device_addr);
```

Preconditions (returns -1 on any failure):
- slot found for `(kind, borrower_context_id)`, borrow is active
- `borrow_source_context_id == source_context_id`
- slot not already mapped (`dma_mapped == 0`)
- `offset + length <= buffer_size(kind)` (FS: 2 MiB; FB: framebuffer_size from boot-info)

On success: sets `dma_mapped = 1`, records direction/offset/length, and
computes `*out_device_addr = buffer_phys + offset`.

#### `process_manager_buffer_dma_sync`

Validates the slot is mapped and the requested offset+length falls
within the originally mapped range. The sync operation itself is a
no-op on x86 (no explicit cache flush required).

#### `process_manager_buffer_dma_unmap`

Validates slot is mapped and `borrow_source_context_id` matches.
Clears `dma_mapped`, `dma_direction_flags`, `dma_offset`, `dma_length`.
Does not release the borrow; the caller must call `wasmos_buffer_release`
separately.

---

### DMA Lifecycle State Machine

A buffer slot progresses through these states:

```
IDLE ──[wasmos_buffer_borrow]──► BORROWED
  └────────────────────────────────────────────────────────┐
BORROWED ──[wasmos_dma_map_borrow]──► DMA_MAPPED           │ 
  │                                                        │
DMA_MAPPED ──[wasmos_dma_sync_borrow]──► DMA_MAPPED        │
  │          (TO_DEVICE before hardware, FROM_DEVICE after)│
  │                                                        │
DMA_MAPPED ──[wasmos_dma_unmap_borrow]──► BORROWED         │
  │                                                        │
BORROWED ──[wasmos_buffer_release]──────────────────────► IDLE
```

Constraints:
- `buffer_release` while `dma_mapped` is set fails with an error.
- On process exit, the cleanup path force-unmaps all DMA-mapped slots
  before releasing the borrow records.
- A new spawn receives new context/slot state with no residual mappings
  from a previous driver instance.

---

### ATA Storage Integration

The ATA driver (`src/drivers/ata/ata.c`) exercises the full DMA lifecycle
on every read and write request.

#### Helper Functions

**`ata_dma_prepare(source_endpoint, offset, length, direction_flags, *out_device_addr)`**

```
wasmos_buffer_borrow(WASMOS_BUFFER_KIND_FS, source_endpoint,
    direction == FROM_DEVICE ? GRANT_WRITE : GRANT_READ)
→ wasmos_dma_map_borrow(WASMOS_BUFFER_KIND_FS, source_endpoint,
    offset, length, direction_flags)
→ [if TO_DEVICE] wasmos_dma_sync_borrow(WASMOS_BUFFER_KIND_FS,
    offset, length, WASMOS_DMA_SYNC_TO_DEVICE)
```

Returns `WASMOS_DMA_STATUS_OK` on success. On any failure, releases
the buffer borrow before returning the error code.

**`ata_dma_finish(source_endpoint, offset, length, direction_flags)`**

```
[if FROM_DEVICE] wasmos_dma_sync_borrow(WASMOS_BUFFER_KIND_FS,
    offset, length, WASMOS_DMA_SYNC_FROM_DEVICE)
→ wasmos_dma_unmap_borrow(WASMOS_BUFFER_KIND_FS, source_endpoint)
→ wasmos_buffer_release(WASMOS_BUFFER_KIND_FS)
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

2. **One mapping per slot at a time.** `process_manager_buffer_dma_map`
   rejects a second map call while `dma_mapped` is set. The driver must
   call `dma_unmap_borrow` before re-mapping the same borrow.

3. **Window check is at map time, not sync time.** The physical address
   range is validated against `capability_dma_range_allowed` once during
   `dma_map_borrow`. Subsequent sync calls verify only that the range
   falls within the already-mapped `dma_length`.

4. **Sync is a no-op on x86.** `process_manager_buffer_dma_sync` enforces
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
   (`PM_BUFFER_KIND_FRAMEBUFFER = 2`) has PM-layer DMA map/unmap support
   wired through the same `pm_fs_buffer_slot_t` path, but no driver
   currently calls the DMA hostcalls for framebuffer transfers.
