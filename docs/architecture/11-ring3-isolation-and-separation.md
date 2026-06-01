## Ring3 Isolation and Kernel/User Separation

This document describes the x86_64 privilege separation model, memory isolation
enforcement, syscall interface, user-origin fault containment, capability system,
IPC authentication, and the ring3 validation gates.

---

### Threat Model

The baseline threat is a compromised or malformed user-space process (WASM or
native payload) that:
- crashes or hangs the kernel
- reads/writes/executes kernel memory
- spoofs privileged IPC or control-plane operations
- abuses shared-memory paths for cross-process access

The target property is containment: the offending process is terminated; the
kernel and all other processes continue.

---

### Virtual Address Layout

| Range                                                               | Owner  | Notes                                            |
|---------------------------------------------------------------------|--------|--------------------------------------------------|
| `0x0000000000000000–0x0000007FFFFFFFFF`                             | none   | Below user range; pml4[0] stripped in user roots |
| `0x0000008000000000–0x000000FFFFFFFFFF` (`USER_VA_MIN–USER_VA_MAX`) | user   | pml4[1] (`USER_PML4_INDEX=1`)                    |
| `0x0000008100000000+` (`MM_USER_STACK_BASE`)                        | user   | User stack allocation base                       |
| `0xFFFFFFFF80000000+` (`KERNEL_HIGHER_HALF_BASE`)                   | kernel | pml4[511], kernel text/data/heap                 |

Sources: `src/kernel/paging.c` (`USER_PML4_INDEX=1`),
`src/kernel/arch/x86_64/cpu_x86_64.c` (`USER_VA_MIN/MAX`),
`src/kernel/memory.c` (`MM_USER_STACK_BASE`),
`src/kernel/include/paging.h` (`KERNEL_HIGHER_HALF_BASE`).

---

### GDT and Segment Selectors

Source: `src/kernel/arch/x86_64/cpu_x86_64.c`

The GDT has 7 entries:

| Index | Selector | Use                     |
|-------|----------|-------------------------|
| 0     | 0x00     | Null                    |
| 1     | 0x08     | Kernel CS               |
| 2     | 0x10     | Kernel DS               |
| 3     | 0x18     | User CS base            |
| 4     | 0x20     | User DS base            |
| 5–6   | 0x28     | TSS (64-bit, two slots) |

At ring3 entry the CPU uses `0x1B` (0x18 | RPL=3) as CS and `0x23` (0x20 | RPL=3)
as DS/SS. `(cs & 0x3u) == 0x3u` is the kernel's user-mode origin check.

#### TSS and Interrupt Stacks

- `g_rsp0_stack`: 16 KiB (`IRQ0_IST_STACK_SIZE = 16384u`) — RSP0 for general
  privilege-level changes at interrupt/trap entry
- `g_irq0_ist_stack`: 16 KiB, pre-filled with `0xCC`, guarded by canary at
  bottom — IST1 for timer and critical interrupt vectors
- `x86_cpu_set_kernel_stack(rsp0)` updates `g_tss.rsp0` on context switch

---

### User Entry Setup

`process_set_user_entry(pid, rip, user_rsp)` in `src/kernel/process.c`:

1. `paging_strip_low_slot_in_root(user_root)` — removes the identity-map slot
   (pml4[0]) from the process's page-table root
2. `paging_verify_user_root_no_low_slot(user_root, 1)` — verifies the strip
   succeeded and no unexpected kernel mappings are present in the user root
3. Sets: `ctx.cs = USER_CS_SELECTOR` (0x1B), `ctx.ss = USER_DS_SELECTOR` (0x23),
   `ctx.rflags = 0x200` (IF=1, all else clear), `ctx.user_rsp = user_rsp`
4. Copies `ctx` to the main thread; the thread's `root_table` is set to the
   process's verified user root

Ring3 code runs at CPL=3 with an address space that has pml4[0]=0, pml4[1]=user
mappings, and pml4[511]=kernel (accessible only from CPL=0 since kernel page
tables do not carry `PT_FLAG_USER`).

#### Low-Slot Policy

`IDENTITY_PD_COUNT = 0` in `src/kernel/paging.c`. New user roots are created
with pml4[0]=0 unconditionally. The kernel root retains a bootstrap low slot for
the initial CR3 handoff but this is stripped before any process goes to ring3.

---

### Page Table Flag Model

Source: `src/kernel/paging.c`, `src/kernel/arch/x86_64/cpu_x86_64.c`

| Flag              | Bit    | Meaning                             |
|-------------------|--------|-------------------------------------|
| `PT_FLAG_PRESENT` | bit 0  | Page is mapped                      |
| `PT_FLAG_WRITE`   | bit 1  | Writable                            |
| `PT_FLAG_USER`    | bit 2  | Accessible at CPL=3                 |
| `PT_FLAG_NX`      | bit 63 | Execute-disable (requires EFER.NXE) |

NX (`IA32_EFER_NXE = 1<<11`) is enabled at boot. User mappings that require
`MEM_REGION_FLAG_USER` must be explicitly flagged; the page-map path rejects
user-slot addresses without `MEM_REGION_FLAG_USER` and kernel-slot addresses
with it.

#### Memory Region Flags

Source: `src/kernel/include/memory.h`

| Flag                    | Value | Meaning                            |
|-------------------------|-------|------------------------------------|
| `MEM_REGION_FLAG_READ`  | 1<<0  | Readable                           |
| `MEM_REGION_FLAG_WRITE` | 1<<1  | Writable                           |
| `MEM_REGION_FLAG_EXEC`  | 1<<2  | Executable                         |
| `MEM_REGION_FLAG_USER`  | 1<<3  | Must be in user VA range (pml4[1]) |

---

### Syscall Interface

Source: `src/kernel/include/syscall.h`, `src/kernel/syscall.c`

Syscalls use `INT 0x80` (vector `X86_VECTOR_SYSCALL = 0x80`). RAX holds the
syscall number; arguments are in RDI, RSI, RDX (following the x86-64 SysV
ABI convention for the first three arguments). Return value in RAX.

| Number | Name                           | Arguments                                        |
|--------|--------------------------------|--------------------------------------------------|
| 0      | `WASMOS_SYSCALL_NOP`           | —                                                |
| 1      | `WASMOS_SYSCALL_GETPID`        | → RAX = current PID                              |
| 2      | `WASMOS_SYSCALL_EXIT`          | RDI = i32 exit_status                            |
| 3      | `WASMOS_SYSCALL_YIELD`         | —                                                |
| 4      | `WASMOS_SYSCALL_WAIT`          | —                                                |
| 5      | `WASMOS_SYSCALL_IPC_NOTIFY`    | RDI = endpoint, RSI = msg_type, RDX/R8/R9 = args |
| 6      | `WASMOS_SYSCALL_IPC_CALL`      | RDI = endpoint, RSI = request_id, RDX = msg_type |
| 7      | `WASMOS_SYSCALL_GETTID`        | → RAX = current TID                              |
| 8      | `WASMOS_SYSCALL_THREAD_YIELD`  | —                                                |
| 9      | `WASMOS_SYSCALL_THREAD_EXIT`   | RDI = i32 exit_status                            |
| 10     | `WASMOS_SYSCALL_THREAD_CREATE` | RDI = entry_rip, RSI = user_stack_top            |
| 11     | `WASMOS_SYSCALL_THREAD_JOIN`   | RDI = tid                                        |
| 12     | `WASMOS_SYSCALL_THREAD_DETACH` | RDI = tid                                        |
| 13     | `WASMOS_SYSCALL_NOTIFY_READY`  | —                                                |

#### Argument Validation

- `syscall_arg_u32(raw, out)`: rejects if `raw >> 32 != 0` (no implicit truncation)
- `syscall_arg_i32(raw, out)`: rejects if `raw != sign_extended(int32_t(raw))`
- Applies to `WASMOS_SYSCALL_EXIT` and `WASMOS_SYSCALL_THREAD_EXIT`

#### IPC Call Pending Queue

Per-process pending-reply queue: `SYSCALL_IPC_PENDING_DEPTH = 8` entries.
Out-of-order replies are retained until `syscall_ipc_pending_take_request`
matches them by `request_id`. On queue overflow the oldest entry is dropped.

Reply authentication (`syscall_ipc_reply_authentic`): validates that
`resp.source == expected_source_endpoint` and that `ipc_endpoint_owner(resp.source)`
equals `expected_owner_context`. Mismatched source or owner → reply rejected.

---

### User-Origin Fault Containment

Source: `src/kernel/arch/x86_64/cpu_x86_64.c`

`x86_user_exception_handler(vector, frame)` is called for all exceptions when
`(cs & 0x3u) == 0x3u` (CPL=3 at fault time).

**Termination policy**: `process_set_exit_status(proc, -11)` then
`process_yield(PROCESS_RUN_EXITED)`. The faulting process is terminated; other
processes continue unaffected.

Covered exception vectors:

| Vector     | Name                | Fault class handled                   |
|------------|---------------------|---------------------------------------|
| `#DE` (0)  | Divide error        | terminate with reason marker          |
| `#DB` (1)  | Debug               | terminate with reason marker          |
| `#BP` (3)  | Breakpoint          | terminate                             |
| `#OF` (4)  | Overflow            | mapped to vector 13 in current probes |
| `#UD` (6)  | Invalid opcode      | terminate with reason marker          |
| `#NM` (6)  | FPU not available   | terminate (probe: mapped to #UD)      |
| `#GP` (13) | General protection  | terminate with reason marker          |
| `#SS` (13) | Stack-segment fault | terminate (probe: mapped to #GP)      |
| `#PF` (14) | Page fault          | classified by `pf_classify_reason`    |
| `#AC` (17) | Alignment check     | terminate (probe: mapped to #UD)      |

#### Page Fault Classification

`pf_classify_reason(error_code, cr2, from_user)` → `pf_reason_t`:

| Reason                      | Condition                                                |
|-----------------------------|----------------------------------------------------------|
| `PF_REASON_USER_TO_KERNEL`  | from_user=1 AND cr2 outside `[USER_VA_MIN, USER_VA_MAX)` |
| `PF_REASON_UNMAPPED`        | present bit = 0                                          |
| `PF_REASON_EXEC_VIOLATION`  | instruction fetch bit = 1                                |
| `PF_REASON_WRITE_VIOLATION` | write bit = 1                                            |
| `PF_REASON_PROTECTION`      | present=1, not instruction, not write                    |

`USER_VA_MIN = 0x0000008000000000`, `USER_VA_MAX = 0x0000010000000000`. A
user-space attempt to access a kernel address produces `USER_TO_KERNEL`.

---

### Capability System

Source: `src/kernel/include/capability.h`, `src/kernel/capability.c`

Five capability kinds, stored as a bitmask per context:

| Kind                  | Enum value | Bit | Grants                                    |
|-----------------------|------------|-----|-------------------------------------------|
| `CAP_IO_PORT`         | 0          | 0   | IN/OUT port access within declared range  |
| `CAP_IRQ_ROUTE`       | 1          | 1   | IRQ routing to a process                  |
| `CAP_MMIO_MAP`        | 2          | 2   | MMIO physical-memory mapping              |
| `CAP_DMA_BUFFER`      | 3          | 3   | DMA buffer allocation within window       |
| `CAP_SYSTEM_CONTROL`  | 4          | 4   | Kernel control-plane access               |

`CAP_ALL_MASK = (1<<5) - 1 = 0x1F`. The kernel context (`context_id=0`) is
initialized with `CAP_ALL_MASK`. User processes start with no capabilities.

Capabilities are granted by name at spawn time via `capability_grant_name`.
The allowlist is the six strings in `make_wasmos_app` (`ipc.basic`, `io.port`,
`irq.route`, `mmio.map`, `dma.buffer`, `system.control`). Packed apps carrying
unknown names are rejected at pack time before the kernel ever sees them.

DMA capability additionally carries a spawn profile: `CAPABILITY_DMA_WINDOW_LIMIT=16`
physical address windows, direction flags, and a `dma_max_bytes` ceiling.

---

### IPC Control-Plane Isolation

Source: `src/kernel/syscall.c`

- **Endpoint ownership**: `syscall_ipc_call_kernel_endpoint_allowed` controls
  which kernel endpoints a ring3 process may send to via `WASMOS_SYSCALL_IPC_CALL`.
  The default deny list covers process-manager and system-control endpoints.
- **Request correlation**: every `IPC_CALL` carries a `request_id`; the kernel
  side matches replies by this ID and by source-endpoint owner. Stale or future
  IDs that do not match a live pending slot are dropped.
- **Source spoof prevention**: `syscall_ipc_reply_authentic` rejects replies
  where `resp.source` is either `IPC_ENDPOINT_NONE`, mismatched from the
  expected endpoint, or owned by a different context than expected.
- **Arg-width check**: endpoint arguments submitted as 64-bit values that exceed
  a valid 32-bit representation are rejected before use.

---

### Shared Memory Isolation

Source: `src/kernel/include/memory.h`, `src/kernel/memory.c`

Shared regions are `owner_context_id`-bound at creation. Access requires:
- owner context, or
- kernel supervisor (context_id=0), or
- an explicit grant via `mm_shared_grant`

`mm_shared_grant(owner_context_id, id, target_context_id)` and
`mm_shared_revoke(owner_context_id, id, target_context_id)` enforce ownership
at grant/revoke time. `mm_shared_retain/release` maintain a reference count.

Misuse checks (tested in `kernel_ring3_smoke_runtime.c`):
- forged IDs from the wrong context
- wrong-owner operations
- stale revoke / double-revoke
- retain/release balance

---

### Ring3 Validation Gates

#### `run-qemu-ring3-test` / `strict-ring3`

Runs `scripts/qemu_ring3_probe_runtime.py` (via `cmake --build build --target
strict-ring3`). Expected passing markers (in serial output):

**Syscall / basic isolation:**
- `[test] ring3 syscall ok`
- `[test] ring3 yield syscall ok`
- `[test] ring3 native abi ok`
- `[test] ring3 native gettid ok`

**IPC enforcement:**
- `[test] ring3 ipc syscall ok`
- `[test] ring3 ipc syscall deny ok`
- `[test] ring3 ipc syscall control deny ok`
- `[test] ring3 ipc syscall arg width deny ok`
- `[test] ring3 ipc call ok`
- `[test] ring3 ipc call deny ok`
- `[test] ring3 ipc call perm deny ok`
- `[test] ring3 ipc call control deny ok`
- `[test] ring3 ipc call control endpoint deny ok`
- `[test] ring3 ipc call correlate ok`
- `[test] ring3 ipc call err rdx zero ok`
- `[test] ring3 ipc call source auth ok`
- `[test] ring3 ipc call stale id deny ok`
- `[test] ring3 ipc call out-of-order retain ok`
- `[test] ring3 ipc call spoof invalid source deny ok`
- `[test] ring3 ipc owner+sender stress ok`

**Fault containment:**
- `[test] ring3 fault isolate ok`
- `[test] ring3 fault write reason ok`
- `[test] ring3 fault exec reason ok`
- `[test] ring3 fault ud reason ok`
- `[test] ring3 fault de reason ok`
- `[test] ring3 fault db reason ok`
- `[test] ring3 fault gp reason ok`
- `[test] ring3 fault of reason ok`
- `[test] ring3 fault nm reason ok`
- `[test] ring3 fault ss reason ok`
- `[test] ring3 fault ac reason ok`

**Shared memory:**
- `[test] ring3 shmem owner deny ok`
- `[test] ring3 shmem grant allow ok`
- `[test] ring3 shmem misuse matrix ok`

**Threading:**
- `[test] ring3 thread create syscall ok`
- `[test] ring3 thread yield syscall ok`
- `[test] ring3 thread exit syscall ok`
- `[test] ring3 thread join syscall ok`
- `[test] ring3 thread join self deny ok`
- `[test] ring3 thread join helper ok`
- `[test] ring3 thread detach syscall ok`
- `[test] ring3 thread detach invalid deny ok`
- `[test] ring3 thread detach helper ok`
- `[test] ring3 thread detach join deny ok`

**Liveness / stress:**
- `[test] ring3 preempt stress ok`
- `[test] ring3 containment liveness ok`
- `[test] ring3 mixed stress ok`

#### `run-qemu-ring3-fault-storm-test`

Runs `scripts/qemu_ring3_fault_storm_test.py` (up to 3 retries). Expected:
- `[test] ring3 watchdog clean ok`
- `[test] sched progress ok`

Forbidden: `[watchdog] trap frame invalid cs=`

---

### Structural Invariants

1. **pml4[0] is zero in all user roots.** `IDENTITY_PD_COUNT=0` is compile-time
   constant. `process_set_user_entry` strips and verifies before first ring3
   entry. A kernel bootstrap low slot exists but is never inherited by user roots.

2. **`PT_FLAG_USER` is required for user-slot mappings.** The page-map path
   rejects user-slot addresses without the flag and kernel-slot addresses with it.
   No silent compatibility bridging.

3. **Fault termination status is always -11.** All covered user-origin exception
   vectors call `process_set_exit_status(proc, -11)`. The status is deterministic
   and distinguishable from normal process exits.

4. **Capabilities are granted fail-closed at pack time.** `make_wasmos_app`
   rejects unknown capability names. The kernel never receives an unrecognized
   capability name from a WASMOS-APP container.

5. **IPC reply authentication checks both source endpoint and owner context.**
   A process that can forge the source endpoint ID but not the owner context
   still fails `syscall_ipc_reply_authentic`.

6. **Shared-memory grants are owner-only.** `mm_shared_grant` and
   `mm_shared_revoke` verify `owner_context_id` matches the region's recorded
   owner. Foreign context IDs are rejected unconditionally.

---

### Deferred Hardening

The following are intentionally left out of current scope:

- Full hostcall split-view/coherence-bridge cleanup (tracked with
  `TODO` comments in `wasm3_link.c`)
- Process-local exception handling (non-kernel fault policy beyond -11
  termination — `TODO(ring3-phase5)` in `cpu_x86_64.c`)
- Bootstrap low slot removal from kernel root
  (`TODO(ring3-phase3)` in `paging.c`)
- Expanded adversarial IPC coverage beyond current probes
