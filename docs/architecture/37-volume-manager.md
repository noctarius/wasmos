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
is the end state `architecture/36` §3 was reaching for; it stops short because
matching on the partition still requires the volume to BE a partition.

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

## 6. Boundaries

The volume manager does NOT mount. It publishes volumes; the device manager's
rules choose what mounts on them, and the filesystem drivers do the mounting.
Keeping the decision in the rule engine is what leaves mount policy in one place
rather than splitting it between a rule file and a service.

Spanning several partitions into one volume — the striped and mirrored sets a
Windows dynamic disk carries — is deliberately out of scope for the first
version. The abstraction admits it later; nothing in the tree needs it now, and
a volume that maps one window of one device is what makes the raw-disk and
partition cases uniform.
