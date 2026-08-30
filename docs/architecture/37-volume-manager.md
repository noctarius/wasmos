## Volume Manager

> **Documentation status: Reference.** Every section is implemented and is the
> current baseline: the recognisers, the `volume` class and its descriptor, the
> suppression rule, exclusivity, and mount policy on volumes all run on every
> boot — `/wfs` and `/vwfs` both mount from `SUBSYSTEM=="volume"` on fstype and
> uuid, naming no disk, unit, backend, partition or transport, and `fs_wfs`
> claims the volume it mounts while `fsck.wfs` refuses a claimed one.

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

**Implemented.** The first of those is the live `/wfs` rule, qualified by uuid as
§4.1 describes. It replaced `SUBSYSTEM=="block", DRIVER=="ata", ATTR{unit}=="2"`,
which is the shape this layer exists to retire — and the WFS volume is precisely
the case nothing else reaches, since it has no partition table for a
`SUBSYSTEM=="partition"` rule to match. `/vwfs` followed, replacing
`DRIVER=="virtio-blk", ATTR{unit}=="48"`.

### 4.1 Identity: `ATTR{uuid}`

`ATTR{fstype}` alone selects a KIND of volume, and a rule fires once. Two volumes
of one format therefore make the rule's target depend on which was recognised
first. `ATTR{uuid}` is what removes that: it is the volume's own identity, and it
travels with the volume across disks and transports.

Both live WFS rules are written this way, differing only in identity:

```
SUBSYSTEM=="volume", ATTR{fstype}=="wfs", ATTR{uuid}=="5746532d-7465-4573-742d-766f6c756d65", ENV{MOUNT}="/wfs"
SUBSYSTEM=="volume", ATTR{fstype}=="wfs", ATTR{uuid}=="5746532d-7669-7274-696f-2d766f6c3031", ENV{MOUNT}="/vwfs"
```

Nothing in the second says virtio. That pair is what shows a transport is not part
of a volume's identity: swap the two images between controllers and each still
mounts at its own path.

**A volume uuid is not a GPT GUID.** It is whatever bytes the FILESYSTEM stores,
and the matcher takes them in on-disk order — WFS's sixteen opaque bytes as
`mkfs_wfs` writes and prints them, FAT's four-byte volume serial as the boot
sector stores it. `ATTR{partuuid}` and `ATTR{type}`, which match GPT's own
identifiers, keep GPT's mixed-endian field order; the two parsers are separate
because the byte orders genuinely differ. Reading a volume uuid through GUID order
reverses its first eight bytes, and the rule then parses cleanly and never
matches.

The width is the format's, one to sixteen bytes, zero-padded for comparison — so a
FAT serial is spelled at its own width (`ATTR{uuid}=="1a2b3c4d"`) rather than
padded out to a GUID by hand. There is no separate `ATTR{serial}`: a rule asks
"which volume", and every format answers with whatever identity it has.

Hyphens are presentation and may fall anywhere; case is ignored. The volume
manager logs each volume's uuid in the spelling a rule takes, because nothing else
on the system reports one — a FAT serial otherwise has to be read out of the boot
sector by hand. Note that DOS-lineage tools DISPLAY a FAT serial reversed
(`8D93-D649` for the bytes `49 d6 93 8d`), so the value to paste is the one in our
own log, not the one another system prints.

Identity is only as strong as the format makes it. A FAT serial is 32 bits with no
uniqueness guarantee, and `scripts/make_gpt_image.py` derives it from the label, so
it carries no more information than `ATTR{label}` does. Prefer a label where one
exists and the uuid where it does not — which is WFS's case, since WFS has no
label at all.

### 4.2 Labels

`ATTR{label}` is the FILESYSTEM's label and is deliberately not `ATTR{partlabel}`.
They differ in practice, not just in principle: the ESP carries no partition
label at all while its FAT boot sector says `QEMU VVFAT`, and
`scripts/make_gpt_image.py` writes the GPT name `user` beside a FAT volume label
`USER`. Matching the wrong one finds nothing, so the rule engine keeps them as
separate matchers and REFUSES a rule that uses one on the other's subsystem —
a rule that can never fire is rejected at load rather than dying silently.

**The empty value means opposite things for the two, and that is not an
inconsistency.** `ATTR{label}==""` selects the volumes whose filesystem label is
blank: a FAT volume may genuinely be named "", and the descriptor separates that
from a format carrying no label at all (`VOLUME_DESCRIPTOR_FLAG_HAS_LABEL`), so
the matcher has something to select. `ATTR{partlabel}==""` selects nothing and is
refused at load: an MBR partition has no name concept and a GPT entry may simply
be unnamed, and neither is "a partition named the empty string".
`ATTR{name}==""` is refused for the same reason, a canonical id never being
empty.

A matcher's presence is therefore tracked by a flag where the empty value is
meaningful, and by the first byte where it is not — `has_label` exists,
`has_partlabel` does not need to.

The filesystem driver is told about the BACKING BLOCK DEVICE, not the volume: a
driver mounts a block device, and the volume is what selected which one. All
three facts it receives — canonical id, backend and unit — come from the backing
record. Passing the volume's own zeroed unit instead let `fs_wfs` mount and then
fail to register, because `fs.backend` packs `(kind, unit)`.

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

**Implemented,** for `fs_wfs` and `fsck.wfs`. The driver sends
`VOLUME_IPC_CLAIM_REQ` the moment the mount completes — not once it is ready,
because the mount itself replays a journal and that window is exactly when a
check would see a race — and releases at the start of shutdown, before the clean
mark is written, since a claim that outlives its holder makes the volume
permanently unrecheckable. The claim is fire-and-forget: blocking a mount on an
advisory record would let it stall a boot.

Both sides DERIVE the volume's class instance rather than exchanging it. A
volume's canonical id is `volume:` prefixed to its backing device's, so a driver
holding `id=block:ata:2` and a tool given `block:ata:2` reach the same
fingerprint independently, and no message carries a second spelling that could
disagree with the publisher.

`fsck.wfs` distinguishes three answers, not two: claimed, not claimed, and
**cannot tell** — no volume manager running, or no volume covering that device.
The third is refused rather than waved through, because a check that proceeded
because it could not find an owner is exactly as dangerous as one that ignored
the owner it found. `--force` overrides any of them and says so in its output,
since findings taken past a live claim may be races rather than damage.

There is no `mkfs.wfs` in the guest — `mkfs_wfs` is a host tool that writes an
image file — so the sharper half of the requirement has no call site yet. A guest
formatter must consult the same flag when one exists.

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

The distinction is not stylistic, and the partition manager is the worked
example: it enumerated the `block` class exactly once at bring-up, so a disk
whose driver registered afterwards was never probed and its partitions never
appeared. A GPT-partitioned virtio-blk disk reproduced it on the shipped
configuration, because virtio-blk finishes negotiating its PCI device well after
the partition manager reports ready. That is fixed —
`architecture/36-partition-manager-and-block-identity.md` §2, "Discovery" — and
the volume manager copies its shape rather than reinventing one:

- Subscribe FIRST, enumerate second, so nothing falls between the two, and drop
  an arrival naming something already known.
- Drop an arrival on the subscriber's OWN endpoint. The registry notifies every
  subscriber including the registrant, so a component that publishes into a
  class it subscribes to sees its own output come back.
- Queue the arrival in the message handler and do the work elsewhere, if the
  work blocks. A handler that blocks stalls the loop it was dispatched from.

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
`partitions/` beside `superblocks/` rather than only the latter.

**Implemented: the volume manager detects the table in the same read.**
`partition_table.detectScheme` runs over the prefix already fetched for the
recognisers — an MBR is at LBA 0 and a GPT header at LBA 1, both inside it — so
suppression costs no extra I/O, no protocol and no ordering assumption. It reuses
`partition_table.zig` rather than re-reading the signatures, so the objection
that this makes two components readers of the same structure applies to the I/O
and not to the parsing: there is still exactly one parser, and a second
implementation could disagree with the one the partition manager acts on.

An earlier revision of this section preferred a second option — the block
descriptor carrying `scheme` — and that option does not work. A consumer of the
`block` class obtains descriptors by sending `BLOCK_IPC_IDENTIFY_REQ` to the
provider, which for a whole disk is `ata.c` or `virtio_blk.zig`; those read no
tables and report `PARTITION_SCHEME_NONE` forever. The device manager's registry
does upsert by `canonical_id`, so the partition manager could amend the record
there — but that registry has no query opcode (`DEVMGR_QUERY_BLOCK_MOUNT_REQ` was
retired), so the amendment never reaches a class client. Recorded because the
reasoning looks sound until the retrieval path is followed.

The rule either way: **a volume is published for a device that holds a
filesystem, and a device holding a partition table holds partitions instead.**

Note what does NOT suppress. A recogniser finding nothing yields
`FS_TYPE_UNKNOWN`, and that publishes: an unrecognised format reads exactly like
a blank disk, both are legitimate to select with `ATTR{fstype}=="unknown"`, and a
`mkfs` needs to see a device it can format. Only a table suppresses.

## 7. Recognition in practice

A recogniser needs to READ, which makes the volume manager a block client with
the same obligations as any other.

- One transfer buffer for the prefix, acquired by the volume manager and lent to
  each backend, because the CLIENT of a request owns the buffer and the server is
  a transient grantee (`architecture/12-dma-transfers.md`).
- Grants are per PROCESS, not per endpoint or per device. An ATA controller
  serving two drives answers both on one endpoint, so one grant covers them and a
  second attempt is refused as `ALREADY_BORROWED`. Deduplicate by endpoint before
  lending, as `grantBackend` does — and keep the set for the life of the process,
  because a disk arriving later may be served by a backend already lent to.
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

**`/boot` keeps a whole-disk rule, and everything the volume rule needs works.**
Both managers are initfs payloads, so a volume for the ESP exists before `/boot`
is mounted, and `ATTR{boot}=="1"` selects the volume the FIRMWARE loaded this
system from. Nothing on an ESP can supply that identity — its filesystem is
ordinary FAT, its label is firmware-specific (`QEMU VVFAT` here), and an MBR gives
it no partition label and no PARTUUID — so it comes from the bootloader, which
reads the MEDIA/HARDDRIVE node of its own device path into `boot_info`; the kernel
publishes the LBA range as the `boot.partition` kernel-environment variable, and
the device manager marks the volume whose backing partition covers it. Swapping
the rule selects the ESP on the first try.

What stops the swap is a YIELD-SPIN one layer away. A volume rule mounts `/boot`
later than a block rule, and the kernel's broker self-test spins on
`PROCESS_RUN_YIELDED` until `font-service` is ready — a service that starts from
`/boot`. The boot then never finishes. See `docs/TASKS.md`; `fat_try_parse_mbr`
stays until the rule does.

Attempting the swap did find and fix a real defect beneath it, because `/boot`
would be the first mount to serve FILE reads from a partition: the partition
manager passed a client's `dst_borrow_id` through to the disk backend, and a
borrow is held per CONTEXT, so that id named a grant between the client and the
proxy and resolved to nothing for the disk. It REBORROWS now —
`xfer_buffer_reborrow` mints the backend a handle within the proxy's own rights,
narrowed to the transfer's direction, and the chain cascade-revokes. The mount
path was never affected because it reads into the caller's own block buffer, which
is why a partition could mount and then fail on its first file read.

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
