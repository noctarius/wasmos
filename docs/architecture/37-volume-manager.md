## Volume Manager

> **Documentation status: Proposal.** Nothing here is implemented. The block
> layer below it exists (`architecture/36-partition-manager-and-block-identity.md`);
> the filesystem recognisers and the `volume` class this describes do not.

**Sources this proposal changes**: `src/drivers/partition_manager/`,
`src/services/device_manager/device_manager_rules.c`, `src/drivers/fs_fat/`,
`abi/constants.yaml`, `abi/opcodes.yaml`, the devmgr rule files.

(Planned new components: a WFS filesystem driver and fsck/mkfs utilities; their exact paths are part of the implementation work.)

**Related**: `architecture/36-partition-manager-and-block-identity.md` (block
descriptors, the partition manager), `architecture/18-filesystem-stack.md`
(mount namespace), `architecture/16-device-manager-and-bus-enumeration.md` (rule
engine and spawn).

---

## 1. What a volume is

A **volume** is a thing with a filesystem on it. A raw formatted disk is one; a
formatted partition is one; several partitions welded together would be one.
Above this layer, partition-ness is not visible.

The block layer answers "what storage exists". It does not answer "what can be
mounted", and those differ in both directions: a partition table entry may hold
no filesystem, and a disk with no partition table may hold one. The `block` class
therefore cannot be what mount policy matches on without that policy naming
disks and units, which is what it is trying to stop doing.

The volume manager owns the gap. It consumes the `block` class and publishes a
`volume` class whose instances carry what a mount decision actually needs:

```
volume {
  fs_type          FS_TYPE_*, from a recogniser
  label            the volume's own name, when its format carries one
  uuid             the volume's identity, when its format carries one
  backing device   the `block` class instance underneath
  lba_start        window within that device
  lba_count
  claimed          a filesystem service holds it
}
```

## 2. Why not the layers that already exist

**Not the partition manager.** Its job is partition tables. Filesystem
recognition placed there makes every new filesystem edit a component that should
know only about GPT and MBR, and it still cannot describe a disk with no table,
because the partition manager publishes nothing for one. A recogniser written
there does work -- it is a dozen lines and the component already reads raw
sectors -- which is what makes it the tempting wrong place.

**Not the filesystem drivers.** They are spawned BY the rule that needs the
answer, so asking a filesystem to identify a volume requires it already running.
HelenOS can ask (`uspace/srv/volsrv/part.c` loops filesystem servers through
`vfs_fsprobe`, and `fat_fsprobe` in `uspace/srv/fs/fat/fat_ops.c` opens the
volume for real) only because its servers are resident. Ours are not.

**Not a mount utility.** `fs_type` is an INPUT to choosing a mount, not an
output of one: the rule engine matches `ATTR{fstype}` to decide which filesystem
driver to spawn. A probe that ran at mount time would produce the answer after
the decision it exists to inform. Neither Linux nor HelenOS probes at mount.

## 3. Recognisers

A **recogniser** identifies a format without that format's driver being present.
It reads a bounded prefix of a volume and answers `FS_TYPE_*`, a label and a
uuid; it does not mount, and it is not authoritative. The filesystem driver does
the real parse and is what refuses a volume a recogniser misread.

That split is what both reference systems use, from opposite directions:

- **Linux** keeps recognition in a library, `libblkid`, with `superblocks/` (one
  file per filesystem) beside `partitions/` (one per table format) — two
  concerns, one library. udev runs it at device-publish time as a builtin, and
  the resulting `ID_FS_TYPE` / `ID_FS_LABEL` / `ID_FS_UUID` are what
  `/dev/disk/by-label` and mount units match on. `mount -t auto` is a second
  consumer of the same library, not where the knowledge lives.
- **Windows** keeps a filesystem recogniser as a stub driver separate from the
  filesystem itself, so an unloaded filesystem can still be identified and then
  demand-loaded. Above it `volmgr` presents partitions and whole disks alike as
  volume objects, and `mountmgr` owns the namespace.

We follow Linux's shape, because a library needs no process to be resident and
our filesystem drivers are not.

Recogniser order is precedence and must be explicit. Specific formats come
before permissive ones — exFAT before FAT, as HelenOS's `fstab[]` orders them —
because a permissive recogniser that runs first claims volumes it should not.

## 4. Mount policy

With volumes published, a rule names what a volume IS rather than where it sits:

```
SUBSYSTEM=="volume", ATTR{fstype}=="wfs",  ENV{MOUNT}="/wfs"
SUBSYSTEM=="volume", ATTR{label}=="user",  ENV{MOUNT}="/user"
```

Neither names a disk, a unit, a backend or a table slot. Moving the image to
another controller, or putting it in a partition, does not change the rule. This
is the end state `architecture/36-partition-manager-and-block-identity.md` §3
was reaching for; it stops short because matching on the partition still
requires the volume to BE a partition.

## 5. Exclusivity

The volume manager is the natural owner of "this volume is in use", because it
is the one component that knows both the volume and who mounted it. Two things
need it and neither has it today:

- `fsck.wfs` must refuse a mounted volume. Checking one a filesystem is writing
  reports damage that is only a race. The block layer used to refuse a second
  client per unit, which made this true by accident; that arbitration was
  removed, correctly, because a request now names its own target.
- `mkfs.wfs` must refuse one for the same reason, more sharply.

`claimed` on the volume, set when a filesystem service mounts it, is the flag
both consult.

It RECORDS a claim; it does not enforce one. The volume manager is not in the
I/O path — unlike the partition manager, which proxies every transfer and can
therefore clamp it, a volume names a device a client then talks to directly. A
tool that does not ask is not stopped. That is the same cooperative arrangement
the mount namespace already runs on, and it is worth stating rather than
implying, because "the volume manager owns exclusivity" reads as enforcement.

Enforcement would mean the volume manager standing in the data path for every
volume, which costs a hop on every filesystem read to prevent a mistake only
`fsck` and `mkfs` can make. The claim is checked where the damage would be done,
not where the I/O is.

## 6. Discovery

The volume manager SUBSCRIBES to the `block` class; it does not enumerate it.

The distinction is not stylistic. `partition_manager.zig`'s `probeAll` calls
`lookupClass` exactly once at bring-up, so a disk whose driver registers
afterwards is never probed and its partitions never appear — a live defect
recorded in `TASKS.md`, invisible today only because the partition manager is
spawned after storage is online. A volume manager built the same way inherits it
exactly, and a second component with the same gap is how a gap stops looking like
one. `subscribeClass` exists (`src/libc/zig/driver.zig`); the partition manager
should switch from one-shot `lookupClass` enumeration to `subscribeClass`-driven
class events, rather than the tree carrying two answers to "a device appeared".

What arrives on that subscription is whole disks AND partitions, in one stream,
because the partition manager publishes each partition INTO the `block` class.
A subscriber must expect both. It does not, however, risk consuming its own
output: volumes are published to a different class.

### The whole-disk volume that should not exist

A disk carrying a partition table must not also be published as a volume. If it
were, `/boot` would appear twice — once as the partition holding it, once as the
disk beneath — and a rule matching on `fstype` would match whichever arrived
first. This is the same failure the `SUBSYSTEM` split fixed one layer down
(`architecture/36-partition-manager-and-block-identity.md` §3), reappearing
because the volume layer flattens exactly the distinction that split
introduced.

Recognition alone does not settle it. A recogniser reading LBA 0 of a partitioned
disk finds a table, matches nothing, and yields `FS_TYPE_UNKNOWN` — which is a
publishable volume, not a suppression, since `ATTR{fstype}=="unknown"` is a
legitimate matcher a rule may use to select a volume no shipped filesystem
claims. "No superblock matched" and "this is not a volume" are different answers
and must not collapse into one.

The suppressing signal is a partition table, which is why `libblkid` carries
`partitions/` beside `superblocks/` rather than only the latter. Two ways to get
it, and the choice is a real one:

- **The volume manager parses tables too**, reusing `partition_table.zig`. Honest
  and self-contained, but it makes two components readers of the same on-disk
  structure, which is precisely what
  `architecture/36-partition-manager-and-block-identity.md` centralised.
- **The block descriptor carries it.** Cheaper, and wrong today: `scheme` is
  always `PARTITION_SCHEME_NONE` on a whole disk, because the DISK DRIVER
  publishes that record and disk drivers read no tables
  (`ata.c`, `virtio_blk.zig`). Only a partition's descriptor carries a real
  scheme, set by the partition manager. Making this work means the partition
  manager amending the disk's record after parsing — a republish of a device it
  does not own.

The second is preferable if the republish is acceptable, because it keeps table
parsing in one component. Either way the rule is the same and belongs in the
document: **a volume is published for a device that holds a filesystem, and a
device holding a partition table holds partitions instead.**

## 7. Recognition in practice

A recogniser needs to READ, which makes the volume manager a block client with
the same obligations as any other.

- One transfer buffer for the prefix, acquired by the volume manager and lent to
  each backend, because the CLIENT of a request owns the buffer and the server is
  a transient grantee (`architecture/12-dma-transfers.md`).
- Grants are per PROCESS, not per endpoint or per device. An ATA controller
  serving two drives answers both on one endpoint, so one grant covers them and a
  second attempt is refused as `ALREADY_BORROWED`. Deduplicate by endpoint before
  lending, as `probeAll` does.
- A partition device is addressed from ITS OWN LBA 0. The partition manager
  rebases every forwarded transfer onto the window, so a descriptor's `lba_start`
  says where the volume SITS and is never an address a client sends. A recogniser
  reading `lba_start` reads past the superblock it was looking for.

The bounded prefix should be one figure stated once, not per recogniser: it sizes
the buffer, and a recogniser that wants more than the volume manager read cannot
have it without changing that figure deliberately.

### Where the library lives

`src/drivers/partition_manager/partition_table.zig` is the precedent and the
shape to copy: a pure module with no `std`, no host calls and no globals, taking
bytes and returning a verdict, with `test` blocks run under `zig test` in the
unit-test target. Purity is what makes exhaustive host testing possible — the
partition parser has 27 cases against synthetic tables — and a recogniser is the
same kind of function.

One file per format, mirroring `libblkid`'s `superblocks/`. Precedence is a
single ordered table in the volume manager, not a property each recogniser
asserts about itself, so the order is readable in one place.

## 8. The `volume` class and its descriptor

The `volume` sketch in §1 is a set of fields; what crosses IPC must be a defined
struct in a client-owned transfer buffer, per `skills/wasmos-shared-primitives`.
The conventions are already set by `wasmos_block_descriptor_t`
(`src/drivers/include/wasmos_driver_abi.h`): packed, `version` first and
validated by every reader, layout pinned by `_Static_assert` in C and comptime
asserts in the Zig mirror, and delivered as `arg0 = buffer_id`,
`arg1 = byte_offset`, `arg2 = size`.

The class instance must be **derived, not allocated**: an FNV-1a fingerprint of
a canonical volume id, exactly as the `block` instance is a fingerprint of
`block:ata:0p1`. A counter would renumber volumes between boots, which is the
property the whole change exists to remove.

A publish needs a BORROW and one slot per volume at its own offset. The partition
manager shipped without either and every publish was refused at the read while
class registration still succeeded — so `blkinfo` listed partitions, reads
worked, and the failure looked cosmetic while every partition rule addressed an
empty registry.

## 9. What this displaces

`wasmos_block_descriptor_t.fs_type` is set to `FS_TYPE_UNKNOWN` by all three
publishers and by nothing else, while `ATTR{fstype}` in the rule engine matches
against it. The matcher is live, the field is not.

That is a reporting surface waiting for a recogniser, and it is also a field in
the wrong struct: `fs_type` on a BLOCK device says a disk driver probed a
superblock, which none does and none should. Once volumes carry it, the block
descriptor's copy should be retired rather than left as a field that is
permanently one value — a field nothing sets is one a later reader will trust.
`ATTR{fstype}` then means what it says, on the subsystem that can answer it.

## 10. Constraints inherited from the block layer

**`/boot` cannot be a volume, yet.** The mount-policy examples in §4 do not cover
it, and it is the case that stopped
`architecture/36-partition-manager-and-block-identity.md` §3 short. The partition
manager is spawned from the BOOT rules, which cannot load until `/boot` is
mounted, so `/boot` cannot mount from anything the partition manager published —
and a volume manager consuming the `block` class sits one layer further from the
bootstrap than that. `/boot` keeps a whole-disk rule and `fat_try_parse_mbr`
until a table reader runs from initfs.

**Two volumes on one disk collide on `fs.backend`.**
`FSMGR_BACKEND_INSTANCE(kind, unit)` packs `(kind, unit)`, and a partition
reports its disk's unit, so the second filesystem to register is refused. It is
latent today because no shipped configuration mounts two volumes from one disk;
a volume manager whose whole purpose is to multiply mountable things makes it
routine. The block fingerprint is the identity that fixes it, and this is the
last packed class instance left.

## 11. Boundaries

The volume manager does NOT mount. It publishes volumes; the device manager's
rules choose what mounts on them, and the filesystem drivers do the mounting.
Keeping the decision in the rule engine is what leaves mount policy in one place
rather than splitting it between a rule file and a service.

Spanning several partitions into one volume — the striped and mirrored sets a
Windows dynamic disk carries — is deliberately out of scope for the first
version. The abstraction admits it later; nothing in the tree needs it now, and
a volume that maps one window of one device is what makes the raw-disk and
partition cases uniform.
