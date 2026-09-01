## Filesystem Stack

> **Documentation status: Mixed reference and proposal.** VFS routing and the
> existing initfs/FAT backends are implemented; filesystem expansion is future
> work.

This document describes the filesystem stack: the `fs_manager` VFS router,
the backend registration model, the client state allocator, the FS IPC opcode
table, the `fs_fat`, `fs_init` and `fs_tmpfs` backends, and the transfer-buffer
borrow semantics used for data transfers.

**Sources**: `src/services/fs_manager/`,
`src/drivers/fs_fat/`,
`src/drivers/fs_init/`,
`src/drivers/fs_tmpfs/`,
`src/kernel/include/wasmos_ipc.h`

---

### Overview

The filesystem stack is a three-layer hierarchy:

```
WASM service (client)
       │  FS IPC (0x400–0x4FF)
       ▼
  fs_manager  ← VFS router; multiplexes by path prefix
       │  FS IPC (forwarded)
       ├──► fs_fat    ← FAT12/16 (FAT32 detected but read/write unimplemented)
       ├──► fs_wfs    ← WFS, journalled, read-write
       ├──► fs_init   ← read-only in-memory initramfs
       └──► fs_tmpfs  ← read-write in-memory filesystem
```

All inter-layer communication uses the same FS IPC opcode set. `fs_manager`
acts as a transparent proxy: it receives a request from a client, determines
which backend owns the path, forwards the request, and relays the reply.

---

### `fs_manager` — VFS Router

**Source**: `src/services/fs_manager/`

#### Backend Registry

```c
#define FS_BACKEND_CAP 8

typedef struct {
    int32_t  in_use;
    int32_t  slot;
    int32_t  endpoint;
    char     mount_name[16];
} fs_backend_t;
```

Up to 8 backends can be registered simultaneously. Each is identified by its
`mount_path`, an absolute canonical path (`"/"`, `"/boot"`, `"/mnt/usb"`)
normalized by `fsmgr_mount_path_from_reported` from whatever the backend
reported. Routing forwards a request to the backend whose `mount_path` is the
longest whole-segment prefix of it.

A mount POINT is an ordinary directory: `fs_manager` creates it in the filesystem
that covers it when the mount registers (`fsmgr_ensure_mount_points`), walking
the ancestors so a mount at `/mnt/usb` gets `/mnt` too. That is what lets
`readdir(/)` be a plain forwarded readdir — the mount names in it are entries the
root filesystem holds, not names `fs_manager` appended to the reply. It runs after
every registration and is idempotent, because a volume can mount before the root
filesystem does and its point has to appear once the root arrives.

#### Removing a Mount

`FSMGR_IPC_UNMOUNT_REQ` removes one mount, named by the absolute PATH it
occupies — which is what the mount table reports and the only handle a client
has on it. The path travels in a transfer buffer the CLIENT owns
(`arg0` = length, `arg2` = buffer, `arg3` = the client's grant), because a mount
path is not bounded by what an argument word carries.

The refusals define the operation:

- Nothing mounted at that path is `WASMOS_ERR_FS_NO_BACKEND`. A path that merely
  EXISTS inside some other mount is a directory, not a mount.
- `WASMOS_ERR_FS_MOUNT_BUSY` while something still stands in the mount: a deeper
  mount inside it, or an open file on it. `/` is normally unremovable for the
  first reason rather than as a special case, since every other mount is inside
  it. This is a statement about the namespace, not a shortage — it clears when
  whoever is standing there leaves, and is not retryable. A client whose working
  directory is under the mount is NOT counted; see `docs/TASKS.md` for why, and
  what has to exist first.

The backend is quiesced before it is dropped — `WASMOS_IPC_SHUTDOWN_REQ` with
`WASMOS_SHUTDOWN_REASON_UNMOUNT`, the same sequence machine shutdown uses — so a
filesystem with dirty state writes it while it still has a block device. A
backend that fails to answer is dropped anyway: the mount is going regardless,
and leaving it in the table would make an unresponsive backend permanent.

The mount POINT is left in place. It is a directory in the covering filesystem
and belongs to that filesystem, so what becomes visible again is whatever the
covering filesystem holds there — the other half of shadowing.

Establishing a mount is not a request. A filesystem is placed by whoever spawns
its driver, which passes `mount=<path>` as a startup argument; the mount then
appears through the class event below. `mount` and `umount` are utilities under
`/system/utils`, not shell built-ins, so the table always comes from the service
that owns it.

Backends do NOT push a registration. `fs_manager` SUBSCRIBES to the `fs.backend`
service class and enumerates it, then PULLS each provider's identity with
`FSMGR_IPC_BACKEND_INFO_REQ` (see Backend Identity below), so the backend set is
rebuilt from the registry on every (re)start and a backend that registers later
arrives as a class event. `FS_IPC_READY (0x404)` survives only as a liveness
question a backend answers.

#### Client State

```c
#define FS_CLIENT_CHUNK_CAP 32

typedef struct {
    /* keyed by context_id */
    ...
} fs_client_state_t;
```

Up to 32 concurrent client contexts. The client state tracks open file
handles and the current working directory for each calling process. Open
handles are forwarded to the appropriate backend; `fs_manager` stores only
the mapping from client handle to backend endpoint and backend-side handle.

The working directory is a full canonical VFS path (`/`, `/wfs`, `/wfs/docs`) and
`fs_manager` is its sole authority: every client path is resolved against it
before routing, and `FS_IPC_CHDIR` reports the resulting path back to the client
so no second copy of it exists. A spawned process inherits its spawner's path by
copy (`FSMGR_IPC_CLONE_CWD`), which is what makes a relative name mean the same
directory in a child as in its parent. A client whose state names no backend is
at the VFS root; that case is answered by `fs_manager` itself and is never
routed to a guessed backend.

---

### FS IPC Opcode Table

All filesystem operations use opcodes in the range `0x400–0x4FF`.

#### Requests (client → fs_manager → backend)

| Opcode             | Value | Operation                                      |
|--------------------|-------|------------------------------------------------|
| `FS_IPC_OPEN`      | 0x400 | Open a file by path                            |
| `FS_IPC_READ`      | 0x401 | Read bytes from an open file                   |
| `FS_IPC_CLOSE`     | 0x402 | Close an open file handle                      |
| `FS_IPC_STAT`      | 0x403 | Get file metadata (size, type, timestamps)     |
| `FS_IPC_READY`     | 0x404 | Backend registration (backend → fs_manager)    |
| `FS_IPC_SEEK`      | 0x405 | Set file position                              |
| `FS_IPC_WRITE`     | 0x406 | Write bytes to an open file                    |
| `FS_IPC_UNLINK`    | 0x407 | Remove a file                                  |
| `FS_IPC_MKDIR`     | 0x408 | Create a directory                             |
| `FS_IPC_RMDIR`     | 0x409 | Remove a directory                             |
| `FS_IPC_READDIR`   | 0x410 | Read directory entries                         |
| `FS_IPC_CHDIR`     | 0x412 | Change working directory                       |
| `FS_IPC_READ_APP`  | 0x413 | Retired; range sentinel only (see below)       |
| `FS_IPC_READ_PATH` | 0x414 | Read a file by absolute path in one shot       |

Mount-table operations are answered by `fs_manager` itself and never reach a
backend, so they carry their own response opcodes rather than `FS_IPC_RESP`:

| Opcode                     | Value | Operation                             |
|----------------------------|-------|---------------------------------------|
| `FSMGR_IPC_QUERY_MOUNTS`   | 0x422 | Report the mount table into a buffer  |
| `FSMGR_IPC_UNMOUNT`        | 0x423 | Remove the mount at an absolute path  |

#### Responses (backend → fs_manager → client)

| Opcode          | Value | Meaning                                     |
|-----------------|-------|---------------------------------------------|
| `FS_IPC_RESP`   | 0x480 | Success reply (carries result data in args) |
| `FS_IPC_STREAM` | 0x481 | Streaming data chunk (multi-message read)   |
| `FS_IPC_ERROR`  | 0x4FF | Error reply (arg0 = errno-style code)       |

---

### Transfer-Buffer Borrow Semantics (owner-push)

Large data transfers (file reads/writes, spawn blob loads) move through the
transfer-buffer **object/owner/borrow** model — the full contract (roles,
grantor/borrower/mapper, lifecycle, ABI) is in
[Transfer Buffers & DMA](12-dma-transfers.md); this section is just how the FS
stack uses it.

The **client owns** the buffer (it holds the lifecycle — it reads the result out
after the call returns). fs-manager is only a **transient borrower** that
reborrows to the backend; the backend writes/reads the client's buffer directly.

For a file read/write (`open`/`read`/`write`/`stat`/`unlink`/…), the client:

1. `acquire`s a per-operation buffer (`buffer_id`); for writes it fills it.
2. `borrow`s the fs.vfs endpoint R|W → `b1` (grants fs-manager); one grant is
   reused across a chunk loop (re-granting per chunk would hit `ALREADY_BORROWED`).
3. Sends the request with **`arg2 = buffer_id`, `arg3 = b1`** (the FS op's other
   args are unchanged, e.g. `fd`/`len`/`path_len`).
4. fs-manager `reborrow`s `b1` to the routed backend → `b2`, forwards
   `arg2 = buffer_id`, `arg3 = b2`; the backend reads/writes the client buffer by
   `buffer_id`. fs-manager `unborrow`s `b2` (its own reborrow) after the reply.
5. The client reads any result out of its own buffer and `release`s it — the
   release cascade-revokes `b1` (fs-manager never unborrows the client's grant).

No copy through `fs_manager` occurs — the backend writes straight into the
client's buffer. `FS_IPC_READ_PATH` (spawn/one-shot read) works the same way,
with the kernel PM as the owning client; PM grants fs-manager via the kernel core
API and its `release` cascades the grant. For requests with no payload (seek,
close) `arg2`/`arg3` carry their normal op args and no borrow is taken.

`FS_IPC_CHDIR` carries its target as a path in the client's buffer, like `OPEN`:
`arg0` = length, `arg2` = buffer id, `arg3` = the grant, a zero length naming the
VFS root. Depth and component length are therefore bounded by the buffer, not by
what fits in four request arguments — a directory name up to a backend's own
maximum (255 bytes for WFS) and an arbitrarily deep path are both expressible.
The reply carries the resolved working directory back in the same buffer, with
its length in `arg1` (`arg0` stays the operation status).

Path-only requests where the *kernel* reads the caller's buffer directly (spawn
paths, service-register descriptors) need no grant at all — the kernel resolves
the caller-owned object by ownership (`pm_foreign_xfer_ptr`).

`FS_IPC_STREAM (0x481)` is used for transfers that exceed a single message
(directory listings): the backend sends stream messages then a final
`FS_IPC_RESP`. (`FS_IPC_READ_APP` is retired along with spawn-by-name.)

---

### `fs_fat` Backend

**Source**: `src/drivers/fs_fat/`

Implements FAT12/16 on a block device. FAT32 is detected at mount but its
cluster read/write is unimplemented (`fat_fatent_read`/`fat_fatent_write`
return `FS_ERR_CORRUPT`). The block device endpoint is provided at spawn time
via the device manager's block-device registration mechanism.

- Supports `OPEN`, `READ`, `SEEK`, `CLOSE`, `STAT`, `READDIR`.
- Write operations (`WRITE`, `MKDIR`, `UNLINK`, `RMDIR`) are implemented for
  the writable `/user` mount; the `/boot` mount is treated as read-only by
  convention.
- Registers with `fs_manager` by sending `FS_IPC_READY (0x404)` with its
  mount name once the FAT superblock has been read and validated.

---

### `fs_tmpfs` Backend

**Source**: `src/drivers/fs_tmpfs/` (Zig)

A read-write filesystem held entirely in the backend's own linear memory: a fixed
node table (192 entries) over a block pool that GROWS. Contents are not
persisted.

The pool takes 32 KiB chunks from `memory.grow`, so an instance storing nothing
costs a table of null pointers rather than its whole capacity — which matters
because the mount is per-instance and a system may run several. A chunk is never
moved and never freed, so a block INDEX is stable for the life of the process;
chains and the free scan address blocks by index, never by pointer, and each
chunk carries the chain and allocation metadata for its own blocks so the
bookkeeping grows with the pool. Measured: 16 KiB of static data, an 8 MiB
ceiling, and growth verified under both wasm3 and WARP.

The manifest's `max_memory` MUST exceed `initial_memory`. A module declaring
`min == max` cannot grow at all — `memory.grow` is refused — which is a silent
capacity ceiling rather than an error.

It is a GENERAL in-memory filesystem, not a root-specific one. The mount comes
from the `mount=` startup argument and defaults to `/`, so a second instance
mounted at `/tmp` is another process with another argument and its own separate
contents. Every identity is therefore DERIVED from the mount path rather than
fixed: the `fs.backend` class instance is an FNV-1a fingerprint of it, so two
instances at different paths cannot claim one registry address, and two asked for
the SAME path are refused the claim rather than answering for each other. The
refusal is currently a HANG rather than an error, because the process manager
answers a refused claim with no reply at all (see `docs/TASKS.md`).

- Implements the whole opcode set except the retired `READ_APP`: `OPEN` (with
  `O_CREAT`/`O_TRUNC`/`O_APPEND`), `READ`, `WRITE`, `SEEK`, `CLOSE`, `STAT`,
  `READDIR`, `CHDIR`, `MKDIR`, `RMDIR`, `UNLINK`, `RENAME`.
- Names are case-SENSITIVE, as in WFS; FAT is the outlier. `NAME_MAX` is 255,
  matching `WFS_NAME_MAX` and FAT's long-name limit, so a filename valid on
  another mount is creatable here — the name lives in the node record, so the
  parity costs 255 bytes per node (a 50 KiB table) whether names are long or not.
- A node with a descriptor open on it, or one a connection stands in, is refused
  rather than freed: a descriptor holds a node index, so removing it underneath
  one would leave that descriptor addressing a slot the next create reuses.
- Runs as an async service (`async_initialize`). Every operation completes in
  memory with no downstream call, so the root task parks forever on a future
  nothing resolves and the runner's poll is what an idle instance sleeps in.
- The namespace and storage core is a separate module, `fs_tmpfs_store.zig`,
  which depends on nothing in the guest environment except a source of pool
  memory. That source is a seam (`chunk_source`): the driver points it at the
  `memory.grow` arena and `tests/unit/test_fs_tmpfs_store.zig` points it at a
  static array, which is what makes path resolution, chain walking, truncation
  and the namespace rules testable on the host. The IPC layer above it is not
  host-testable and is covered only by a running guest.

**Placed by rule.** A `SUBSYSTEM=="boot"` device-manager rule may carry
`ENV{MOUNT}`, delivered to the spawned process as a `mount=` startup argument.
That is what a filesystem with no backing device needs: it names no device, so
nothing about it implies where it belongs. Two instances are placed this way and
need no disk between them:

- `/home/user` — a mount at DEPTH. `fs_manager` creates `/home` and then
  `/home/user` as ordinary directories in the root filesystem while walking the
  mount path.
- `/wfs/nested` — a mount INSIDE another mount. Its point is created in the WFS
  volume rather than in the root filesystem, which is the other branch of
  `fsmgr_ensure_mount_points`, and it SHADOWS the file the volume already holds
  there (`scripts/wfs/nested/covered.txt`): a path under the point reaches the
  tmpfs, so the covered file is neither listed nor readable while the mount
  stands. Demonstrated, and verified against a control run with the mount
  disabled where the file IS listed. Nothing unmounts, so contents REAPPEARING on
  unmount remains construction rather than a tested property.

**Why the VFS root wants one.** A mount point has to be a directory somewhere,
and nothing holds one while `/` has no filesystem — which is why routing matches
a mount's first segment and a mount can only exist at the top level. The kernel's
init sequence therefore spawns an instance for `/` between `fs-manager` and
`fs-init`, before any volume mounts.

An instance mounted at `/` is what `ls /`, `cd /` and every path naming no other
mount are served by. `tests/test_vfs_root_mount.py` pins that end to end: the
root is reported as `fs-tmpfs`, the mount points are directories in it, `ls` and
`cd` agree about what exists, and a path under a mount still reaches that mount
rather than the root covering it.

### `fs_init` Backend

**Source**: `src/drivers/fs_init/`

A read-only in-memory filesystem used for early boot content before the FAT
volume is available. The initramfs image is embedded in the kernel ELF or
provided via a known physical address from the bootloader.

- Handles `OPEN`, `READ`, `CLOSE`, `READDIR`, `CHDIR`, `READY` only (no `SEEK`,
  no `STAT`).
- Unhandled/write opcodes return `FS_IPC_ERROR` (arg0 = -1).
- Registers as the `"/init"` mount.

---

### Path Normalization

`fs_manager` normalizes client paths before forwarding:

- Joins a relative path onto the client's working directory, so every routed
  path is absolute (`fsmgr_cwd_join`, unit-tested on the host).
- Resolves `.` and `..` components and collapses redundant slashes. `..` cannot
  escape the VFS root.
- Strips the mount-name prefix from the path before sending to the backend
  (the backend sees a root-relative path).
- Refuses rather than truncates when a result does not fit: a shortened path
  names a different file, which the caller would then open unknowingly.
- Routes the resulting absolute path to a mount, matched on its FIRST SEGMENT
  (`route_absolute_path`). A path that names no mount is NOT SERVED: the caller
  reports `WASMOS_ERR_FS_NOT_FOUND`.

  **There is no fallback backend, deliberately.** Every mount is named, so a path
  matching none names nothing. Serving such a path from the boot volume made
  `/system/utils/ip` resolve as a second name for `/boot/system/utils/ip`, and the
  alias appeared in no listing — `ls /` enumerates mounts, `/system` was not among
  them, and `cd /system` succeeded anyway. Nothing in the system emitted such a
  path: the CLI's `PATH` is `/boot/apps:/boot/system/services:/boot/system/drivers:/boot/system/utils`,
  the device-manager rule roots are `/init/…` and `/boot/…`, and a full boot never
  reached the fallback. Its only population was paths typed by hand.

  A second fallback of the same shape, keyed on the CLIENT rather than the path,
  had already shipped and hidden broken working-directory inheritance for as long
  as there was a single non-root mount: a relative name typed in `/wfs` was handed
  to the FAT driver, which answered NOT_FOUND, and the driver holding the file was
  never asked (`resolve_backend_for_state`).

  The distinction that carries the weight is ABSOLUTE vs relative, not routed vs
  unrouted. Routing is reached only after a name has been joined onto the client's
  working directory, so "no mount matched" is a statement about the PATH and never
  about the client.

  Once a root filesystem is mounted, every absolute path routes somewhere and
  "not served" is reachable only on a system with nothing at `/`. The root is not
  a fallback: it is an ordinary mount that happens to prefix every path, which is
  why it cannot reintroduce the aliasing above — a path served by the root
  filesystem is a path IN the root filesystem, and appears in its listing.

### Backend Identity

A backend reports two independent things in `FSMGR_IPC_BACKEND_INFO_RESP`, and
conflating them mislabels every mount:

- `kind` (`arg0`) is `FSMGR_BACKEND_BLOCK` or `FSMGR_BACKEND_PSEUDO`. It separates a
  backend served from a block device from one served from anything else
  (initfs today, a devfs or sysfs later), and carries **no filesystem
  identity** — every block-backed backend reports the same value whatever it
  mounts.
- `fs_type` (`arg1`) is the `FS_TYPE_*` the backend serves, and is the only field
  that answers "which filesystem". `FS_TYPE_UNKNOWN` means the backend named
  nothing, and is reported as such rather than guessed at.

A backend whose reported mount name is EMPTY once its leading `/` is stripped is
not registered. Such a backend names a mount PATH (`/`) rather than a mount name,
and while routing matches a first segment there is no name for it to match;
registering it anyway gave it a slot, kept it out of routing, and printed a bare
`/` entry into the root listing.

`mount` names a filesystem from `fs_type` alone (`fsmgr_backend_fs_name`, one
lookup row per type). Deriving it from `kind` reports every mounted volume as
FAT.

A backend that sits on no block device names itself the same way: initfs reports
`FS_TYPE_INITFS` and tmpfs reports `FS_TYPE_TMPFS`, so `FS_TYPE_*` is the single
namespace for "which filesystem"
rather than a probe-result enum with pseudo-filesystems handled beside it. A
future devfs or sysfs is therefore a value in `abi/constants.yaml` plus a lookup
row, and costs no branch at any call site. Such a value is meaningless in a
`SUBSYSTEM=="volume"` rule -- a volume is a formatted block device, so a rule
spelling one could never match, and the rule parser does not accept one.

A path-less request (`READDIR`) is preceded by a `CHDIR` re-asserting the
requesting client's directory, because a backend holds one current directory per
`fs_manager` connection and cannot tell two clients apart. `fs_manager` uses its
own transfer buffer for that, since such a request supplies none.

---

### Structural Invariants

1. **Backend discovery is dynamic and PULL-based.** `fs_manager` subscribes to
   the `fs.backend` class and enumerates it, then pulls each provider's identity.
   It registers its own name and signals readiness BEFORE discovery, because the
   providers are peers that may still be starting and blocking readiness on them
   would deadlock the boot sequence.

2. **Path routing is longest-prefix over mount PATHS, on whole segments**
   (`fsmgr_route_path_for_mounts`). A mount is an absolute canonical path, the
   owner of a request is the longest such path prefixing it, and a match must end
   at a segment boundary so `/wfs` does not own `/wfsx`. `/` prefixes everything,
   so the root filesystem is the owner of last resort rather than a case of its
   own, and a mount at `/mnt/usb` needs no special handling — it simply outranks
   `/mnt`.

3. **`FS_IPC_READ_APP` is retired.** PM spawn now reads app blobs via
   `FS_IPC_READ_PATH` (see `process_manager_spawn.c`); `FS_IPC_READ_APP_REQ`
   (0x413) survives only as a range sentinel and has no live handler.

4. **`fs_manager` is the sole client-facing endpoint.** Services never talk
   directly to `fs_fat` or `fs_init`. All FS traffic goes through
   `fs_manager`, which provides uniform path normalization and backend
   multiplexing regardless of which physical volume holds the file.

5. **Stream messages carry partial data; RESP signals end.** A client reading
   a large file will receive zero or more `FS_IPC_STREAM (0x481)` messages
   followed by exactly one `FS_IPC_RESP (0x480)`. The client must buffer all
   stream chunks before the final response arrives.
