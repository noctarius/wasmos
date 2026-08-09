# Kernel Object Tables

Status: implemented (`src/kernel/idtable.{h,c}`); both IPC tables adopted.

A recurring shape in the kernel: a table of objects that user space refers to by
an opaque id. Endpoints, select sets, futexes, shared-memory regions and
completion ports are all this. The shape is small enough to look like it does not
need a component, which is why it was written out longhand twice before this
document existed — and each hand-written copy got a different subset of it right.

## The five obligations

A table of id-addressed kernel objects owes its callers all of these. Missing any
one has already produced a bug in this tree.

1. **Grow on demand.** A fixed array is a ceiling chosen before the workload was
   known. The select table was 32 slots — 2.8 KB — against a boot that brings up
   roughly twenty processes, each wanting one.
2. **Never hand out a live id.** The id counter is monotonic and wraps. On wrap
   it must skip ids that a live object still holds, or the new object shares an
   id with the old and traffic for one lands on the other. This was a real
   endpoint-table bug.
3. **Never hand out a reserved id.** `0` and the "no object" sentinel must not be
   allocatable, so a caller can use them unambiguously.
4. **Bound each owner.** Without a per-context ceiling, one context takes the
   whole table and every other context is starved: a service that cannot create
   a select set cannot park, and one that cannot create an endpoint cannot be
   reached. The refusal alone is not the property — a global limit gives that.
   The property is that the *neighbour still works*.
5. **Release by owner.** A dying context takes its objects with it, and only its
   own. Anything else leaks the table across process churn.

## The component

`idtable_t` (`src/kernel/include/idtable.h`) implements all five over the
existing `list_t` array-chunk backend. The element type embeds
`idtable_header_t` as its first member:

```c
typedef struct {
    idtable_header_t header;   /* id, owner_context_id, in_use */
    ...                        /* whatever the object is */
} my_object_t;
```

`idtable_alloc` / `idtable_get` / `idtable_free` / `idtable_count_for_owner` /
`idtable_release_owner` are the whole surface. A `per_owner_max` of zero means
unbounded, so a table with no starvation concern does not have to invent a
number. Failures are the generated transport axis (`WASMOS_FULL`, `WASMOS_NOENT`,
`WASMOS_INVAL`), never a bare `-1`.

## What it deliberately does not do

**It holds no lock.** Every function assumes the caller's table lock is held.
This is not an oversight to fix later: the endpoint table's lock order is
table-lock then per-object lock, taken that way so a lookup cannot be raced by a
release, and a lock inside the component would either duplicate that or invert
it. Ownership of the lock stays where the ordering is decided. See
`docs/LOCK_HIERARCHY.md`.

**It does not cache a per-owner count.** `idtable_count_for_owner` walks. That is
O(n) per allocation, which is fine because allocation is rare and a cached count
is one more invariant to maintain across every release path — including the ones
that only run during teardown.

**It does not index by id.** Lookup walks too. An array indexed by id is O(1) but
reintroduces the fixed ceiling, which is obligation 1. Where a caller needs O(1)
on a hot path, it should hold the object pointer rather than re-resolving the id:
`ipc_select_signal` already takes the object, not the id.

## Adopters

| Table                     | State                                                                                                                               |
|---------------------------|-------------------------------------------------------------------------------------------------------------------------------------|
| IPC endpoints (`ipc.c`)   | Adopted. The hard case: a per-object lock, a lock ORDER that matters, and teardown work of its own — all of which stayed in `ipc.c` where they belong. |
| IPC select sets (`ipc.c`) | Adopted, and no longer a fixed array. Ids stopped being array indices, which removed a latent bug: a destroyed slot could be reissued to another context while a waiter still held it. |
| Futex table (`futex.c`)   | Entries live for the system's lifetime; obligations 2–5 do not apply as written.                                                    |

When adding a new id-addressed object table, use this rather than writing the
five out again. When touching one of the hand-written ones, moving it here is
worth more than patching the obligation you came for.
