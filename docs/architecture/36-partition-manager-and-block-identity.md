## Partition Manager and Block Device Identity

> **Documentation status: Mixed reference and proposal.** §1 (block identity) is
> implemented and is the current baseline. §2 (the partition manager) and §3
> (mount policy) are design proposals; no partition table is parsed yet.
> It describes the descriptor-based block identity model, the partition manager
> service that publishes partitions as block devices, and the mount policy that
> follows from both.

**Sources this proposal changes**: `src/services/device_manager/device_manager.c`,
`src/services/device_manager/device_manager_rules.c`, `src/drivers/ata/ata.c`,
`src/drivers/virtio_blk/virtio_blk.zig`, `src/drivers/fs_fat/`,
`src/utils/blkinfo/blkinfo.c`, `abi/opcodes.yaml`, `abi/constants.yaml`,
`abi/errors.yaml`, `scripts/initfs.toml`, the devmgr rule files.

**Related**: `architecture/16-device-manager-and-bus-enumeration.md` (rule
engine, block registry, spawn state machine),
`architecture/18-filesystem-stack.md` (mount namespace, FAT backend),
`architecture/15-drivers-and-services.md` (driver spawn and class discovery).

---

### Why

Three decisions that belong to the disk are currently written into rule files,
and one of them is written into a filesystem driver.

A block rule today names four things at once:

```
SUBSYSTEM=="block", DRIVER=="ata", ATTR{unit}=="0", ENV{MOUNT}="/boot", RUN+="system/drivers/fs_fat.wap"
```

which disk, which filesystem driver, where it mounts — and, by omission, which
*region* of the disk. The region is decided somewhere else entirely:
`fat_try_parse_mbr` in `src/drivers/fs_fat/fat_geom.c` reads LBA 0, and if it is
not a BPB, locates *the first FAT partition* and mounts that. A second
filesystem would need its own copy of that logic, and "the first FAT partition"
is not necessarily the partition the rule meant.

That probe is live on every boot: QEMU's vvfat (`-drive
format=raw,file=fat:rw:${BUILD_DIR}/esp`) presents a classic MBR, not a raw FAT
volume and not a GPT:

```
offset 0x1B8:  fa fd 1a be          MBR disk signature 0xBE1AFDFA
offset 0x1BE:  80 01 01 00 06 …     bootable, type 0x06 (FAT16), start LBA 63
offset 0x1FE:  55 aa
```

Both `/boot` and `/user` are that shape. UEFI does not force GPT here — OVMF
boots the removable-media path `\EFI\BOOT\BOOTX64.EFI` off an MBR FAT partition
— and vvfat has no GPT mode, so this is not a setting that can be changed.

The rule file must therefore keep working for MBR volumes while a disk that
carries a real GPT describes its own mounts.

---

### Model

Three pieces, each independently landable.

1. **Block identity becomes a descriptor.** A block device is described by a
   struct delivered over a transfer buffer, not by fields packed into IPC
   arguments and a class-instance integer.
2. **A partition manager service** parses partition tables and publishes each
   partition as a block device in its own right, proxying block I/O with an LBA
   offset and a bounds clamp.
3. **Mount policy reads the descriptor.** Rules match partition attributes;
   a GPT partition supplies its own mount path.

---

### 1. Block Identity — implemented

#### The problem with the packed instance

The `block` service class instance is documented as `(backend << 8) | unit`
(`abi/constants.yaml`). Addressing a partition needs a third field, and the next
attribute after that needs a fourth. `DEVMGR_PUBLISH_BLOCK_DEVICE` has the same
shape: unit, present and active bits, and backend spread across four `int32`
arguments, with `sector_count` occupying one of them — which caps a reportable
disk at 2 TiB.

The rule matcher inherits that ceiling. `device_manager_rules.c` parses
`ATTR{unit}` as a `u8` and class/subclass as `u8` hex with `MATCH_ANY_U8` /
`MATCH_ANY_U16` sentinels, because the matcher's vocabulary cannot exceed what
the publish encoding carries.

#### The descriptor

```c
typedef struct {
    uint32_t version;
    uint32_t backend;          /* BLOCK_BACKEND_*                          */
    uint32_t unit;
    uint32_t partition;        /* 0 = whole disk, else partition-table slot */
    uint32_t scheme;           /* PARTITION_SCHEME_NONE / _MBR / _GPT       */
    uint32_t fs_type;          /* FS_TYPE_*, from the superblock probe      */
    uint32_t sector_bytes;
    uint32_t flags;            /* present, active_service, read_only        */
    uint64_t lba_start;        /* absolute, on the underlying disk          */
    uint64_t lba_count;
    uint8_t  type_guid[16];    /* GPT partition type; zero under MBR        */
    uint8_t  part_guid[16];    /* PARTUUID; zero under MBR                  */
    uint8_t  mbr_type;         /* MBR type byte; zero under GPT             */
    uint8_t  reserved[7];      /* pads the fixed head to 88 bytes           */
    char     label[144];       /* PARTLABEL, UTF-8 from UTF-16LE            */
    char     canonical_id[64]; /* block:ata:0, block:virtio-blk:40          */
} wasmos_block_descriptor_t;
```

The padding is load-bearing. The C declaration is `packed` and the Zig mirror in
`src/libc/zig/driver.zig` is a plain `extern struct`; the two describe the same
bytes only because every field sits at its natural alignment and the total is a
multiple of 8, so neither compiler inserts anything. Without the pad they differ
by four bytes at the tail — a disagreement no field-by-field review would show.
Both sides assert the size and the individual offsets at compile time, and
`tests/unit/test_block_descriptor.c` pins the same numbers from the C side.

`lba_start` and `lba_count` are 64-bit because the descriptor is a byte payload
in a transfer buffer, not a set of IPC arguments — the "no 64-bit value crosses
a boundary" rule constrains message arguments, which this is not.

This does **not** lift the transfer limit: `BLOCK_IPC_READ_REQ` arg1 is still a
32-bit LBA, so the I/O path keeps its 2 TiB addressing ceiling (ATA is on
`lba28` and stops at 128 GiB regardless). A `TODO` at the opcode records that;
widening it is a separate change.

#### Identity and the class instance

The canonical id string is the identity, and the **publishing backend** assigns
it — `block:ata:0`, `block:virtio-blk:40`. Partitions extend it with a `p<slot>`
suffix (`block:virtio-blk:40p1`), using `p` for the same reason Linux writes
`nvme0n1p1` rather than `sda1`: our device names end in a digit, so `ata01` would
be ambiguous.

The device manager used to synthesize these ids itself, spelling ATA disks
`block:pci:BB:DD.FF:ata<unit>` at the storage controller's address. That form is
gone, for a reason worth recording: the ATA driver declares `[[regions]]`, so it
is spawned by module index with no startup arguments and never learns its own bus
address. The device manager knew one only because it had matched the rule, and
attaching it to disks it had not probed is exactly what put the ATA controller's
location on virtio disks that were nowhere near it. A consumer inventing an
identity for a device it never probed is the failure mode, not the address
format.

The cost is that two IDE controllers still collide, both calling their disks 0
and 1. Separating them needs the bus address in the id, which needs the driver to
be told its own — today an either/or with its declared I/O windows (see the
spawn-argument `TODO` in `device_manager.c`).

The class instance becomes an **opaque 32-bit fingerprint** of that string,
carrying no decodable meaning:

- FNV-1a 32 over the canonical id, not SHA-256. The fingerprint is an index key,
  not a security primitive, and FNV-1a is ten lines in both C and Zig — which
  matters because the ATA driver (C), the virtio-blk driver (Zig) and the
  partition manager (Zig) must all compute the same value for the same string.
  `hash_id` keeps its SHA-256 hex form for display.
- A client that knows the id it wants computes the fingerprint locally and
  matches without a round trip. A client that is enumerating asks each provider
  for its descriptor.
- A collision surfaces as a refused registration, because
  `service_class_registry_add` already refuses a second owner for a live
  `(class, instance)`. Loud, not silent aliasing.

#### Identity ladder

For rule matching, strongest to weakest:

| Rank | Attribute            | Available under | Survives                        |
|------|----------------------|-----------------|---------------------------------|
| 1    | `partuuid`           | GPT             | reordering, re-cabling, relabel |
| 2    | `partlabel`          | GPT             | reordering, re-cabling          |
| 3    | `<disksig>-nn`       | MBR             | reordering                      |
| 4    | positional `ata0p1`  | all             | nothing but a stable topology   |

Rank 3 is degenerate in this environment: vvfat hardcodes the disk signature
`0xBE1AFDFA`, identical for every directory it serves, so both `/boot` and
`/user` would report `be1afdfa-01`. Under vvfat, rank 4 is the only identity
that distinguishes them. It is still reported, because it is meaningful on a
physical MBR disk.

#### Wire changes

- `BLOCK_IPC_IDENTIFY_REQ` gains `arg1 = buffer_id`: the descriptor is written
  into a buffer the **caller** acquired, lent to the backend with WRITE, and
  releases afterwards. `IDENTIFY_RESP` reports only `arg1 = bytes written`.
  This is the ownership model of `12-dma-transfers.md` — the client holds a
  transfer buffer's lifecycle and the server is a transient grantee — and it is
  not optional: a server that lends its own buffer can never free it, because
  release is owner-only, no hostcall transfers ownership, and nothing tells a
  server when a client has finished reading. It also means the descriptor is
  written fresh per call rather than read out of a snapshot the backend must
  never rewrite while lent.
- `BLOCK_IPC_IDENTIFY_REQ` arg0 became the device's **class instance** rather
  than a unit. This followed from making the instance opaque: several instances
  may share one endpoint (an ATA controller registers one per drive), and the
  instance is the only name a client that found the provider by class actually
  holds. The backend computed those fingerprints itself and maps one back to its
  own device; arg0 = 0 means "the only device you serve".
- `DEVMGR_PUBLISH_BLOCK_DEVICE` carries a descriptor in a buffer the **driver**
  owns and lends READ to device-manager. Same rule, other direction: the driver
  is the client of device-manager there. It is held for the process lifetime
  because a publish is fire-and-forget and carries no acknowledgement that would
  say when the record has been consumed.
- The device's canonical id reaches the filesystem driver as `id=` in its
  startup arguments, so the string has exactly one producer. A driver that
  rebuilt it from `driver=` and `unit=` would be a second place free to disagree
  with the publisher, and a disagreement leaves the filesystem waiting forever on
  an instance nobody registered.
- A borrow is held per **context**, not per endpoint: the kernel resolves the
  grantee endpoint to its owning process and allows one active borrow per object
  per process. This is why a server-lent buffer needs per-client bookkeeping that
  cannot be made correct, and why the client-owned shape above needs none —
  `18-filesystem-stack.md` already records the same trap for the FS path
  ("re-granting per chunk would hit `ALREADY_BORROWED`"). `09-process-and-ipc.md`
  additionally notes that the one-borrow-per-context restriction is a known
  limitation to remove, not the architectural rule.
- `DEVMGR_QUERY_BLOCK_MOUNT_REQ` survives for now, still keyed on unit alone and
  still matching `0xFF` wildcards. It is retired in §3, when the mount name moves
  into the startup arguments and removes the `IDENTIFY` + query round-trip pair
  in `fs_fat.c`.

---

### 2. The Partition Manager

A single system-wide service, written in Zig, spawned once by a rule that names
no disk:

```
SUBSYSTEM=="block", RUN+="system/drivers/partition_manager.wap"
```

It subscribes to the `block` service class, and for each disk that appears:
reads the table, and either publishes partitions or gets out of the way.

#### Endpoint fan-out

The service holds one endpoint per relationship, not one endpoint total. Two
independent reasons force this, and one budget bounds it:

- **Downstream, one endpoint per disk.** `ata_assign_unit_for_source` resolves
  the unit from the *source endpoint* and `BLOCK_IPC_READ_REQ` carries no unit
  field, so a client that has claimed unit 0 can never address unit 1 on the
  same connection. Distinct endpoints make one process look like distinct
  clients, so ATA needs no change. virtio-blk has no claim map and is
  indifferent.
- **Upstream, one endpoint per published partition.** A block request carries no
  partition field either, so the endpoint it arrives on *is* the partition
  selector. The alternative — adding a partition argument to the block
  opcodes — would change the contract for every backend to serve one client.
- **Budget.** `IPC_ENDPOINT_PER_CONTEXT_MAX` is 64. Allowing 4 disks × 8
  partitions plus a command, reply and notify endpoint is 39. The caps are
  explicit constants and exceeding one is a logged refusal, not a silent drop.

`ipc_select_add` watches the whole set in one loop.

#### Probe chain

Per disk, in order:

1. **GPT** — LBA 1 begins `EFI PART`, the header CRC32 validates over
   `header_size` bytes with the CRC field zeroed, and the entry-array CRC32
   validates over `num_entries × entry_size` bytes. On primary-header failure,
   retry at `alternate_lba` (the backup header at the last LBA) before giving
   up; GPT's redundancy is the reason to prefer it and skipping it wastes that.
2. **MBR** — LBA 0 ends `0x55AA`. Four 16-byte entries at 0x1BE, slots 1–4, type
   byte 0 meaning empty. A `0xEE` protective entry means the disk claims GPT: if
   step 1 already failed, refuse the disk rather than treating the protective
   entry as a real partition. Extended partitions (`0x05`/`0x0F`) are not
   followed; that is a logged `TODO`, not a silent skip.
3. **No table** — publish nothing. The raw disk's own class instance already
   exists and the existing rule path still reaches it. This is what makes
   partition tables optional and keeps a raw-superblock disk working untouched.

The entry array can reach 128 × 128 bytes = 32 sectors, which does not fit the
8 KiB block buffer. CRC32 accumulates across chunks, so the array is streamed:
the CRC is computed over the whole array while only the first
`PARTMGR_MAX_PARTITIONS` usable entries are retained. Validation is never
narrowed to the part that happened to fit.

Partitions are numbered by **table slot, not discovery order**. A GPT entry
array routinely has gaps, so a disk with entries in slots 1 and 3 yields `p1` and
`p3` with no `p2`, and deleting a neighbour never renumbers the survivors. This
is the same derived-not-allocated invariant `abi/constants.yaml` argues for one
level up.

Sector size is fixed at 512 with a `TODO` for 4Kn. Consumers read
`sector_bytes` from the descriptor rather than assuming, so lifting it later does
not become a second flag day.

#### Filesystem probe

After locating a partition, the manager reads its first sector and matches a
declarative table — offset, magic bytes, resulting `fs_type` — so adding WFS
later is a row rather than new logic:

| `fs_type`         | Test                                                        |
|-------------------|-------------------------------------------------------------|
| `FS_TYPE_FAT`     | `0x55AA` at 510, jump-boot `0xEB`/`0xE9`, plausible BPB      |
| `FS_TYPE_WFS`     | WFS superblock magic at offset 0 (when the driver lands)      |
| `FS_TYPE_UNKNOWN` | no match                                                     |

The type GUID is reported alongside rather than instead: the pair is what
carries meaning. The ESP GUID `C12A7328-…` implies FAT, but Microsoft Basic Data
`EBD0A0A2-…` — what a plain FAT partition usually carries — implies nothing, so
neither signal alone is sufficient and a rule may require both.

#### Proxy behaviour

For each partition endpoint the manager serves the full block contract:

- `BLOCK_IPC_IDENTIFY_REQ` — answered from its own descriptor. Not forwarded;
  the reported capacity is the *partition's*, not the disk's.
- `BLOCK_IPC_READ_REQ` / `WRITE_REQ` — arg1 becomes `lba + lba_start`; arg0 (the
  client's block-buffer physical address) passes through untouched, so the disk
  driver still lands bytes directly in the filesystem driver's buffer. The
  proxy costs one IPC round trip, not a copy.
- `BLOCK_IPC_READ_ZC_REQ` — `xfer_buffer_reborrow` sub-grants the client's
  borrow to the downstream driver's endpoint (rights ⊆ its own), the request is
  forwarded with the downstream borrow id and the buffer id unchanged, and
  `xfer_buffer_unborrow` releases it on completion, cascade-revoking the chain.
  Zero-copy survives the hop.
- **Bounds clamp** — any request whose `[lba, lba + count)` leaves
  `[0, lba_count)` is refused with a packed error. This is the containment the
  design exists for: a filesystem driver reaches its partition and nothing else.

In-flight requests live in a fixed-size table mapping
`(upstream endpoint, upstream request id)` to the downstream request id. A full
table is answered with a packed busy error, never dropped — a dropped reply
strands a peer that has no timeout.

The manager is an **async service** (`entry = "async_initialize"`), not a
drain-and-select loop: the runner polls only when the root task parks, so there
is no coroutine per request.

#### Zig surface

`src/libc/zig/driver.zig` already covers `ipc_create_endpoint`, the select set,
transfer-buffer acquire/read/write/borrow/release, and `registerService` with a
class and instance. Three additions are needed, all ports of existing
header-only C in `src/libc/include/wasmos/ipc.h`:

- `lookupClass` — `SVC_IPC_LOOKUP_CLASS_REQ`, class name in, entry array out
  over a transfer buffer.
- `subscribeClass` — `SVC_IPC_SUBSCRIBE_CLASS_REQ`, so a disk appearing later
  is picked up without polling.
- `reborrow` / `unborrow` wrappers over the generated externs, which exist in
  `abi/generated/zig/wasmos_imports.zig` but have no ergonomic form.

Build wiring follows `src/drivers/virtio_blk/CMakeLists.txt`: `wasmos_add_zig_wasm_app`
with the coroutine runtime, `service_async_entry_wasm.c` and the generated ABI
staged flat. The manifest needs `svc.class` (to claim `block` instances) and no
hardware capabilities at all — the manager touches no device, only IPC. It ships
as a bootstrap entry in `scripts/initfs.toml`, because it sits between the disk
driver and the `/boot` filesystem driver and therefore must exist before `/boot`
mounts.

---

### 3. Mount Policy

Rules match descriptor fields, so a matchable attribute is a struct field plus a
parser row rather than an ABI change:

```
SUBSYSTEM=="partition", ATTR{partlabel}=="user",                        RUN+="system/drivers/fs_fat.wap"
SUBSYSTEM=="partition", ATTR{partuuid}=="a1b2…",   ENV{MOUNT}="/data",  RUN+="system/drivers/fs_fat.wap"
SUBSYSTEM=="partition", ATTR{type}=="C12A7328-…", ATTR{fstype}=="fat", ENV{MOUNT}="/boot", RUN+="system/drivers/fs_fat.wap"
SUBSYSTEM=="partition", ATTR{name}=="ata0p1",      ENV{MOUNT}="/boot",  RUN+="system/drivers/fs_fat.wap"
```

A GPT partition whose label is a path supplies its own mount point, and
`ENV{MOUNT}` is the fallback for tables that carry no label — which is every MBR
table. `ATTR{scheme}=="none"` makes "this disk has no table" an explicitly
matchable state instead of a fallback branch in code.

Three details fixed at design time because they are silent-failure shaped:

- **GUID byte order.** GPT stores type and partition GUIDs mixed-endian — the
  first three fields little-endian, Microsoft style — so the on-disk bytes are
  not textual order. The descriptor keeps raw bytes exactly as read and the rule
  *parser* converts canonical text into the same layout at load time. Matching is
  then a 16-byte `memcmp`. Getting this wrong yields rules that parse cleanly
  and never match.
- **Operators.** Exact compare and `!=` only. No globbing: labels and UUIDs are
  exact things, an omitted attribute already means "don't care", and a pattern
  engine is easy to add later and hard to remove.
- **Capacity.** `DEVMGR_RULE_TEXT_CAP` is 4096 with per-kind caps of 8. A GUID
  is 36 characters, so lines roughly double and the per-rule struct grows from a
  handful of bytes to over 100. Both caps are raised deliberately, because the
  failure mode is a silently truncated rule file.

The filesystem driver receives `driver=`, `unit=`, `mount=` and its window in
startup arguments and mounts at its partition's LBA 0. `fat_try_parse_mbr` and
the MBR branch of the FAT mount coroutine are deleted.

---

### Implementation Phases

Each phase lands on its own and only the third changes boot behaviour.

#### Phase 1 — Descriptor and opaque instance (done)

No partitions involved; behaviour-neutral. The descriptor, the fingerprint, the
`partition_scheme` / `fs_type` / `block_descriptor` constant groups and the
`block_dev` descriptor error codes are in place, and both block backends publish
and serve descriptors. `DEVMGR_QUERY_BLOCK_MOUNT_REQ` was NOT retired here: the
mount name still travels that way until §3 moves it into the startup arguments.

Two things the plan did not anticipate, both recorded above: `IDENTIFY_REQ` arg0
had to become the class instance rather than a unit, and a descriptor borrow is
per-context rather than per-endpoint.

#### Phase 2 — The partition manager

- `src/drivers/partition_manager/` — Zig service, `linker.metadata`,
  `CMakeLists.txt`; bootstrap entry in `scripts/initfs.toml`.
- Pure Zig table parser: GPT with CRC32 and backup fallback, MBR, UTF-16LE →
  UTF-8, mixed-endian GUID handling.
- Filesystem probe table.
- Endpoint fan-out, proxy with offset, clamp and reborrow.
- `driver.zig`: `lookupClass`, `subscribeClass`, reborrow/unborrow wrappers.
- Spawn rule added; no partition rule matches yet, so nothing mounts differently.

**Done when** partitions appear in `blkinfo` and the device-manager log on a GPT
disk, and every existing mount is unchanged.

#### Phase 3 — Mount policy

- `device_manager_rules.c`: `SUBSYSTEM=="partition"`, string attributes, GUID
  text parsing, raised caps.
- `device_manager.c`: partition rules drive the filesystem spawn; mount name and
  window passed as startup arguments.
- `fs_fat`: accept the window, drop `fat_try_parse_mbr`, drop the mount-name
  query round trip.
- Rule files rewritten: no `DRIVER==`/`ATTR{unit}==` for filesystems.
- Optionally convert `/user` from vvfat to a real GPT image — `userfs/` holds
  only `.gitkeep` and no test reads it from the host, so nothing depends on
  live-directory semantics. This is the cheapest way to get a genuine GPT onto
  the boot path rather than only into a test fixture.

**Done when** a GPT partition mounts at the path its label names, with no rule
naming a disk.

---

### Testing

Per `skills/wasmos-regression-test` and `skills/wasmos-integration-test`.

**Host unit** (`tests/unit/`, registered in the unit-test target):

- GPT: valid header; bad header CRC; bad entry-array CRC; backup-header
  fallback; `header_size` outside `[92, sector_size]`; zero-type entries
  skipped; slot gaps preserved; entry count above the cap still CRC-validated
  over the full array.
- MBR: valid table; missing `0x55AA`; protective `0xEE` with and without a valid
  GPT; empty slots.
- GUID text ↔ mixed-endian bytes round trip; UTF-16LE label → UTF-8.
- Filesystem probe table against real and near-miss boot sectors.
- FNV-1a fingerprint stability and cross-language agreement.
- Rule parsing: string attributes, GUID normalisation, `!=`, cap overflow.
- Descriptor encode/decode round trip.

The parser is a pure Zig module with `test` blocks, run via `zig test` in the
unit-test target. CI installs Zig for both workflows, so this is covered there;
a local machine without Zig loses that coverage, and the target is gated the same
way the virtio-blk build already is.

**Integration** (`tests/`, each file assigned to exactly one battery in
`tests/batteries.json`):

- A hand-built GPT image attached as virtio-blk — written in Python as
  `test_virtio_blk.py` already builds its fixture, no external tooling — assert
  partitions published with the right ranges, labels and PARTUUIDs, and a
  filesystem spawned against the right window.
- The MBR path: `/boot` still mounts through the partition manager.
- The no-table path: a raw-superblock disk still mounts by the existing rule.
- Bounds clamp: a request past the partition end is refused, not passed through.

---

### Open Items

- The 32-bit LBA in the block transfer opcodes caps addressing at 2 TiB. The
  descriptor reports capacity honestly; the I/O path does not yet reach it.
- Extended MBR partitions are not followed.
- 4Kn sector sizes are not supported; `sector_bytes` exists so consumers do not
  need changing when they are.
- Whether the ESP itself should stop being vvfat. It would remove MBR support
  entirely, at the cost of the instant-rebuild ergonomics that `fat:rw:` gives
  across every QEMU target.
