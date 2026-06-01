## Filesystem Stack

This document describes the filesystem stack: the `fs_manager` VFS router,
the backend registration model, the client state allocator, the FS IPC opcode
table, the `fs_fat` and `fs_init` backends, and the buffer borrow semantics
used for data transfers.

**Sources**: `src/services/fs_manager/`,
`src/services/fs_fat/`,
`src/services/fs_init/`,
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
       ├──► fs_fat    ← FAT12/16/32 on a block device
       └──► fs_init   ← read-only in-memory initramfs
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

Up to 8 backends can be registered simultaneously. Each backend is identified
by a `mount_name` prefix (e.g. `"/boot"`, `"/user"`, `"/init"`). Path
routing is a prefix match: the request is forwarded to the backend whose
`mount_name` is the longest matching prefix of the requested path.

Backends register by sending `FS_IPC_READY (0x404)` to `fs_manager` with
their mount name encoded in the message arguments via
`wasmos_sys_ipc_pack_name16`.

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
| `FS_IPC_READ_APP`  | 0x413 | Read an application blob (used by PM at spawn) |
| `FS_IPC_READ_PATH` | 0x414 | Read a file by absolute path in one shot       |

#### Responses (backend → fs_manager → client)

| Opcode          | Value | Meaning                                     |
|-----------------|-------|---------------------------------------------|
| `FS_IPC_RESP`   | 0x480 | Success reply (carries result data in args) |
| `FS_IPC_STREAM` | 0x481 | Streaming data chunk (multi-message read)   |
| `FS_IPC_ERROR`  | 0x4FF | Error reply (arg0 = errno-style code)       |

---

### Buffer Borrow Semantics

Large data transfers (file reads, application blob loads) use the kernel's
DMA-buffer borrow mechanism rather than packing data into IPC message
arguments:

1. The client allocates or designates a shared buffer region.
2. The client sends a request with a buffer descriptor (address, length) in
   the IPC args.
3. The backend writes directly into the client's buffer via the borrow handle.
4. The response message carries the number of bytes written; no copy through
   `fs_manager` is needed.

`FS_IPC_STREAM (0x481)` is used for transfers that exceed a single message:
the backend sends multiple stream messages followed by a final `FS_IPC_RESP`
to indicate end-of-data.

For `FS_IPC_READ_APP` (used by the process manager during spawn), the process
manager provides a pre-allocated spawn buffer and the filesystem backend fills
it directly.

---

### `fs_fat` Backend

**Source**: `src/services/fs_fat/`

Implements FAT12/16/32 on a block device. The block device endpoint is
provided at spawn time via the device manager's block-device registration
mechanism.

- Supports `OPEN`, `READ`, `SEEK`, `CLOSE`, `STAT`, `READDIR`.
- Write operations (`WRITE`, `MKDIR`, `UNLINK`, `RMDIR`) are implemented for
  the writable `/user` mount; the `/boot` mount is treated as read-only by
  convention.
- Registers with `fs_manager` by sending `FS_IPC_READY (0x404)` with its
  mount name once the FAT superblock has been read and validated.

---

### `fs_init` Backend

**Source**: `src/services/fs_init/`

A read-only in-memory filesystem used for early boot content before the FAT
volume is available. The initramfs image is embedded in the kernel ELF or
provided via a known physical address from the bootloader.

- Handles `OPEN`, `READ`, `SEEK`, `STAT`, `READDIR`, `CLOSE` only.
- Write operations return `FS_IPC_ERROR` with `EROFS`.
- Registers as the `"/init"` mount.

---

### Path Normalization

`fs_manager` normalizes client paths before forwarding:

- Strips the mount-name prefix from the path before sending to the backend
  (the backend sees a root-relative path).
- Resolves `.` and `..` components.
- Applies the per-client working directory (set via `FS_IPC_CHDIR`) to
  relative paths.

---

### Structural Invariants

1. **Backend registration is dynamic.** Backends send `FS_IPC_READY` when
   ready; `fs_manager` does not start until at least one backend is
   registered. The `"/init"` backend registers first and enables early
   rule loading by the device manager.

2. **Path routing is prefix-longest-match.** If `/boot/system` and `/boot`
   are both registered, a path under `/boot/system` goes to the more-specific
   backend.

3. **`FS_IPC_READ_APP` bypasses normal handle state.** It performs a
   single-shot read of an entire application blob into a caller-supplied
   buffer. The process manager uses this opcode exclusively during spawn; it
   does not open a handle and does not update per-client state.

4. **`fs_manager` is the sole client-facing endpoint.** Services never talk
   directly to `fs_fat` or `fs_init`. All FS traffic goes through
   `fs_manager`, which provides uniform path normalization and backend
   multiplexing regardless of which physical volume holds the file.

5. **Stream messages carry partial data; RESP signals end.** A client reading
   a large file will receive zero or more `FS_IPC_STREAM (0x481)` messages
   followed by exactly one `FS_IPC_RESP (0x480)`. The client must buffer all
   stream chunks before the final response arrives.
