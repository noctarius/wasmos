# WARP Ring 3 Implementation Plan

This document is the engineering specification for porting WARP JIT execution
from ring 0 to ring 3.  It is a companion to
`11-ring3-isolation-and-separation.md` (ring 3 kernel model) and should be
read alongside it.

---

## 1  Root-Cause Analysis — Why Ring 3 Is the Right Fix

WARP currently runs entirely in ring 0 with the shared kernel page table.  All
physical frames from `pfa_alloc_pages_above(WASMOS_SHMEM_PHYS_LIMIT=64MB)` are
addressed through the kernel direct map `phys | kHalfBase`.  This creates
three aliasing classes that have all produced real bugs:

| Aliasing class | Symptom |
|---|---|
| Shmem (phys [0,64MB)) vs WARP linear mem (phys [64MB,512MB)) | menu_bar's `ensureLinearSize` memset zeroed gfx-smoke window-2 framebuffer (fixed by partition) |
| WARP linear mem of process A vs WARP linear mem of process B | multiple potential cross-process corruptions |
| WARP JIT code vs WARP linear mem of the **same** process | `commitVirtualMemory` memset zero-filled the calculator's JIT code → #UD panic |

The third class is the **current unfixed crash**.  It occurs because:

1. The calculator's JIT code is at, say, `phys_jit = 0x41dfcb0` (69 MB,
   canonical VA `0xffffffff841dfcb0`).
2. The calculator's WASM linear memory is reallocated to a base, say,
   `phys_lmem = 0x4100000` (65 MB).  The WARP capacity is 2 MB, so the linear
   memory occupies `[0x4100000, 0x4300000)` physically and canonical VA range
   `[0xffffffff84100000, 0xffffffff84300000)`.
3. `phys_jit = 0x41dfcb0` is inside `[0x4100000, 0x4300000)`.
4. When `ensureLinearSize(917KB)` is called for a later shmem probe, it calls
   `commitVirtualMemory(lmem_base + prev_usable, delta)`, which remaps the page
   at `0xffffffff841dfcb0` canonically (no-op in the PTE) then WARP immediately
   calls `std::memset(..., 0, delta)` over that range — zeroing the JIT code.
5. The next execution of the JIT code hits the zeroed page → `#UD`.

**With ring 3**, each WARP process has its own CR3.  JIT code is mapped at a
user VA (e.g. `0x0000008000000000`) and linear memory at a different user VA
(e.g. `0x0000008010000000`).  They never share canonical VA space with anything
else.  `commitVirtualMemory` writes are always confined to the linear memory
region of one process; no other process's JIT code is affected.

---

## 2  Design Overview

```
Ring 0  (compilation, unchanged):
  WasmModule::initFromBytecode → writes JIT x86 code to physically-backed pages
  Physical pages are dual-mapped:
    kHalfBase alias  — kernel RW, used while writing JIT code and for hostcalls
    user-space VA    — CPL=3 R-X, used during execution

Ring 3  (execution, new):
  process_set_user_entry() → JIT entry trampoline at user VA
  JIT code runs at CPL=3
  Hostcall → INT 0x80 → kernel dispatches warp_* C function → IRET to ring 3
  Export return → INT 0x80 WASMOS_SYSCALL_WARP_RETURN → kernel records result
  Fault → x86_user_exception_handler → process_set_exit_status(-11) → killed
```

WARP source (`libs/warp`) is **never modified**.  All changes are in the
kernel wrapper layer.

---

## 3  Memory Layout

### 3.1  Per-process user VA space

The user VA space is `[USER_VA_MIN, USER_VA_MAX)` = `[0x0000008000000000,
0x000000FFFFFFFFFF]` (pml4[1]).

We carve four sub-regions within it (all offsets from `USER_VA_MIN`):

| Offset from USER_VA_MIN | Size    | Content                    | Flags         |
|-------------------------|---------|----------------------------|---------------|
| `+0x0000_0000_0000`     | 512 MB  | JIT executable code        | R-X user      |
| `+0x0020_0000_0000`     | 2 GB    | WASM linear memory         | RW- user      |
| `+0x00A0_0000_0000`     | 4 KB    | Hostcall trampoline page   | R-X user      |
| `+0x00A0_0000_1000`     | 4 KB    | Return trampoline page     | R-X user      |

Define constants in `src/kernel/include/warp_ring3.h` (new file):

```c
#define WARP_R3_JIT_BASE      (USER_VA_MIN + 0x0000000000000ULL) /* 0x8000000000 */
#define WARP_R3_LINMEM_BASE   (USER_VA_MIN + 0x2000000000000ULL) /* 0xa000000000 */
#define WARP_R3_HC_TRAMPOLINE (USER_VA_MIN + 0xa000000000000ULL) /* trampoline page */
#define WARP_R3_RET_TRAMPOLINE (WARP_R3_HC_TRAMPOLINE + 0x1000)
```

### 3.2  Kernel dual-map

Physical pages for JIT code and linear memory are also accessible via the
kernel higher-half direct map (`phys | kHalfBase`) for use during compilation
and from hostcall C functions.  The dual mapping is maintained throughout the
lifetime of the WasmModule.

### 3.3  Physical zone allocation

No change to zone partitioning from the current state:
- Shmem: `pfa_alloc_pages_below(WASMOS_SHMEM_PHYS_LIMIT = 64 MB)`
- JIT code + linear mem: `pfa_alloc_pages_above(WASMOS_SHMEM_PHYS_LIMIT)`

The aliasing bug is eliminated by per-process address spaces, not by further
physical zone sub-division.  (Zone sub-division was the workaround; ring 3 is
the real fix.)

---

## 4  Hostcall Trampoline Page

### 4.1  Stub layout

One 4 KB page holds up to 512 stubs of 8 bytes each.  Each stub:

```asm
; Stub for hostcall ID N  (8 bytes)
    mov  eax, N          ; B8 NN NN NN NN  (5 bytes)
    int  0x80            ; CD 80           (2 bytes)
    ret                  ; C3              (1 byte)
```

The kernel generates these stubs into a kernel-writable scratch page, then
maps the physical page at `WARP_R3_HC_TRAMPOLINE` with R-X + user flags.

`WARP_HC_MAX = 128` (enough for all current wasmos hostcalls; table in
`src/kernel/warp/link.cpp`).  Each entry in `warp_wasmos_symbols_ring3[]`
has `ptr = WARP_R3_HC_TRAMPOLINE + N * 8` for the Nth hostcall.

### 4.2  Return trampoline

A separate 4 KB page at `WARP_R3_RET_TRAMPOLINE` contains:

```asm
warp_r3_ret_trampoline:
    ; RAX = i32 return value from the export function
    ; push RAX, then syscall WARP_RETURN
    push rax
    mov  eax, WASMOS_SYSCALL_WARP_RETURN   ; e.g. 16
    pop  rdi                                ; return value in RDI
    int  0x80
    ; kernel does not return here — process ends or is rescheduled
    ud2
```

The return trampoline is the `user_rsp` target: the JIT entry point is
called with `CALL` semantics so the return address on the ring-3 stack is
`warp_r3_ret_trampoline`.  When the export function returns, the CPU pops
the trampoline address, executes it, and the kernel's WARP_RETURN syscall
handler captures the result.

---

## 5  New Syscalls

Add to `src/kernel/include/syscall.h`:

```c
#define WASMOS_SYSCALL_WARP_HOSTCALL  14   /* RDI=hc_id, RSI=arg0, RDX=arg1, R10=arg2, R8=arg3 */
#define WASMOS_SYSCALL_WARP_RETURN    15   /* RDI=i32 return value */
#define WASMOS_SYSCALL_WARP_LINMEM_BASE 16 /* → RAX = ring3 linmem base VA (for linMem register) */
```

Handler in `src/kernel/syscall.c`:

```c
case WASMOS_SYSCALL_WARP_HOSTCALL: {
    uint32_t hc_id = (uint32_t)frame->rdi;
    // Dispatch table: warp_ring3_hostcall_dispatch[hc_id]
    // Arguments from RSI, RDX, R10, R8 (see warp_r3_hostcall_arg layout)
    // Return value → frame->rax
    frame->rax = warp_ring3_dispatch_hostcall(hc_id, frame);
    break;
}
case WASMOS_SYSCALL_WARP_RETURN: {
    // Record RDI as the export return value in the per-process warp state
    process_t *proc = process_get(process_current_pid());
    proc->warp_r3_return_value = (int32_t)frame->rdi;
    proc->warp_r3_done = 1;
    process_yield(PROCESS_RUN_YIELDED);  // yield; caller thread will unblock
    break;
}
```

### 5.1  Hostcall argument passing

The JIT code calls `trampoline_N(args...)`.  WARP's calling convention for
hostcalls passes arguments left-to-right.  The trampoline does `int 0x80`
with `RDI=hc_id` **and the actual hostcall arguments in RSI, RDX, R10, R8**
(matching x86-64 SysV with the 4th arg in R10 instead of RCX to avoid
conflict with SYSCALL instruction).

The syscall frame provides:
```
frame->rdi = hostcall_id
frame->rsi = arg0  (uint32_t)
frame->rdx = arg1  (uint32_t)
frame->r10 = arg2  (uint32_t)
frame->r8  = arg3  (uint32_t, optional)
```

The existing hostcall C functions (`warp_ipc_send`, `warp_shmem_map_auto`,
etc.) take `(arg0, arg1, ..., void *ctx_)`.  The `ctx_` pointer comes from
the per-process `warp_ctx_for_pid(pid)`.

---

## 6  Execution Entry and Exit

### 6.1  Calling an export function

In `warp_driver_call_entry` (currently calls `mod->callExportedFunctionWithName`):

```c
// New ring-3 entry path:
int warp_r3_call_export(wasm_driver_t *driver, const char *name,
                        uint32_t argc, const uint32_t *argv)
{
    process_t *proc = process_find_by_context(driver->owner_context_id);
    proc->warp_r3_done = 0;
    proc->warp_r3_return_value = 0;

    // Compute ring-3 JIT function address from name
    uint64_t ring3_rip = warp_r3_resolve_export(driver->warp_module, name);
    if (!ring3_rip) return -1;

    // Push args + return trampoline address onto ring-3 stack
    // then set up IRET frame to enter ring 3 at ring3_rip
    warp_r3_setup_call_frame(proc, ring3_rip, argc, argv);

    // Set ring-3 entry point and unpark (or directly IRET if same thread)
    process_set_user_entry_rip_rsp(proc->pid,
                                   ring3_rip,
                                   proc->warp_r3_stack_top);
    process_unpark_pid(proc->pid);

    // Block current (kernel) thread until WASMOS_SYSCALL_WARP_RETURN fires
    while (!proc->warp_r3_done) {
        process_block_on_ipc(PROCESS_BLOCK_IPC);  // or a dedicated wait reason
    }

    return proc->warp_r3_return_value;
}
```

### 6.2  Alternately: helper thread model

Simpler alternative — no new process; use a dedicated kernel helper thread
that enters ring 3 on behalf of the WASM module:

1. Spawn helper thread with `process_spawn_kernel_thread(warp_r3_helper, arg)`
2. The helper thread sets up the IRET frame and enters ring 3
3. When WASMOS_SYSCALL_WARP_RETURN fires, the kernel signals the caller
4. This avoids per-process state for the WARP_RETURN mechanism

The helper-thread model reuses the existing `wasm_driver_t` without needing
per-process field additions.

### 6.3  Fault handling

Ring 3 faults in JIT code go to `x86_user_exception_handler(vector, frame)`,
which calls `process_set_exit_status(proc, -11)` then
`process_yield(PROCESS_RUN_EXITED)`.  The WARP module is cleaned up on
process exit.  The kernel thread that called `warp_r3_call_export` unblocks
and sees the process as dead → returns `-1` (export call failed).

This replaces the current C++ exception / longjmp mechanism.  The only
recoverable traps are ones we explicitly want to recover from (e.g.
`STACKFENCEBREACHED`); everything else terminates the process.

---

## 7  linMem Register Initialization

WARP's JIT loads `linMem` at function entry from basedata:

```
; prologue generated by x86_64_backend.cpp
mov  linMem, [basedata - Basedata::FromEnd::linMemBase]
```

For ring 3, `linMem` must hold the **ring-3 VA** of the WASM linear memory
base, not the kernel higher-half alias.  The basedata (part of the memory
manager's allocation) must be accessible from ring 3 as well.

Approach: dual-map the basedata page(s) at a user VA.  After calling
`WasmModule::setupRuntime()`, patch the `linMemBase` field in the basedata
to point to `WARP_R3_LINMEM_BASE + offset_within_allocation`.

The kernel still accesses linear memory via the kHalfBase alias; only the
linMemBase field in basedata (read by JIT prologues) uses the ring-3 VA.

---

## 8  Symbol Table for Ring 3

New function in `src/kernel/warp/link.cpp`:

```cpp
vb::Span<vb::NativeSymbol const>
warp_wasmos_symbols_ring3(void)
{
    // Same ordering as WASMOS_SYMBOLS(DYNAMIC_LINK) but ptr = trampoline VA
    static vb::NativeSymbol syms[] = {
        { vb::NativeSymbol::Linkage::DYNAMIC, "wasmos", "ipc_create_endpoint",
          vb::function_traits<...>::getSignature(),
          (void*)(WARP_R3_HC_TRAMPOLINE + HC_IPC_CREATE_ENDPOINT * 8) },
        // ... one entry per hostcall ...
    };
    return { syms, sizeof(syms)/sizeof(syms[0]) };
}
```

Passed to `initFromBytecode(bc, warp_wasmos_symbols_ring3(), true)` for ring-3
modules.  `DYNAMIC_LINK` is required (same as AOT) so the linker emits a
relocation table rather than baking kernel addresses into JIT CALL instructions.

---

## 9  Hostcall Dispatch Table

A new file `src/kernel/warp/ring3_dispatch.c` contains the dispatch table:

```c
typedef int32_t (*warp_r3_hc_fn_t)(const syscall_frame_t *frame, void *ctx);

static int32_t r3_dispatch_ipc_send(const syscall_frame_t *f, void *ctx) {
    return (int32_t)warp_ipc_send(f->rsi, f->rdx, f->r10, f->r8, 0, 0, 0, 0, ctx);
}
// ... one per hostcall ...

static const warp_r3_hc_fn_t warp_r3_dispatch[WARP_HC_MAX] = {
    [HC_IPC_CREATE_ENDPOINT] = r3_dispatch_ipc_create_endpoint,
    [HC_IPC_SEND]            = r3_dispatch_ipc_send,
    // ...
};

int32_t warp_ring3_dispatch_hostcall(uint32_t hc_id, const syscall_frame_t *f)
{
    if (hc_id >= WARP_HC_MAX || !warp_r3_dispatch[hc_id]) return -1;
    void *ctx = warp_ctx_for_pid(process_current_pid());
    return warp_r3_dispatch[hc_id](f, ctx);
}
```

Hostcall IDs are assigned as `enum warp_hostcall_id` in `warp_ring3.h` and
match the stub offsets in the trampoline page.

---

## 10  Process State Additions

Add to `process_t` (or a parallel per-PID ring-3 state struct):

```c
typedef struct {
    uint64_t  ring3_jit_base;    /* user VA where JIT code starts */
    uint64_t  ring3_linmem_base; /* user VA where WASM linear memory starts */
    uint64_t  ring3_stack_top;   /* user VA of stack top for ring-3 execution */
    int32_t   return_value;      /* captured by WASMOS_SYSCALL_WARP_RETURN */
    uint8_t   done;              /* set by WASMOS_SYSCALL_WARP_RETURN handler */
} warp_ring3_state_t;
```

Alternatively, store in `wasm_driver_t` alongside `wasm_module`.

---

## 11  Implementation Phases and File Checklist

### Phase 1 — Per-process CR3 + dual-mapped allocations

**New files:**
- `src/kernel/include/warp_ring3.h` — constants, state struct, prototypes
- `src/kernel/warp/ring3_dispatch.c` — hostcall dispatch table
- `src/kernel/warp/ring3_trampolines.c` — trampoline page generation

**Modified files:**
- `src/kernel/warp/mem_utils_kernel.cpp`:
  - `mapRXMemory`: allocate physical, dual-map at kernel alias AND user VA
    (`paging_map_4k` into the current process's `mm_context_t`)
  - `allocVirtualMemory`: same dual-map treatment for linear memory
  - `commitVirtualMemory`: keep but narrow to only remap pages that are
    currently non-canonically mapped (check PTE, skip if already canonical
    and within linear memory physical range)
- `src/kernel/warp/shim.cpp`:
  - `warp_kmalloc` large path: dual-map if the allocation is for this module
- `src/kernel/warp_driver.cpp`:
  - `wasm_driver_start`: create dedicated `mm_context_t` for the module;
    pass ring-3 symbol table; patch basedata linMemBase

### Phase 2 — Syscall additions

**Modified files:**
- `src/kernel/include/syscall.h`: add `WASMOS_SYSCALL_WARP_HOSTCALL`, `_WARP_RETURN`, `_WARP_LINMEM_BASE`
- `src/kernel/syscall.c`: add dispatch cases

### Phase 3 — Ring-3 execution

**Modified files:**
- `src/kernel/warp_driver.cpp`:
  - Replace `mod->callExportedFunctionWithName` with `warp_r3_call_export`
  - Set `k_stack_fence` per-process to ring-3 stack base
  - Remove C++ exception checkpoint (ring-3 faults kill the process cleanly)
- `src/kernel/warp/link.cpp`:
  - Add `warp_wasmos_symbols_ring3()`

### Phase 4 — Testing

- Add `[warp-r3] start`, `[warp-r3] ready` logs analogous to the calculator
- Existing halt test already checks `[calculator] ready` — this becomes the
  ring-3 WARP regression gate
- Add `run-qemu-warp-r3-test` target once initial ring-3 boot is stable

---

## 12  Key Invariants to Preserve

1. **WARP source is never modified.** All bridging is in the kernel wrapper.
2. **AOT modules use the same ring-3 path.** `initFromCompiledBinary` still
   runs in ring 0; only execution moves to ring 3.
3. **Hostcall C functions are unchanged.** The dispatch table calls them with
   the same arguments; only the calling convention (syscall frame) differs.
4. **`warp_ctx_for_pid` still works.** The hostcall context is retrieved
   by PID from the same table; ring 3 execution does not change the PID.
5. **No shmem zone changes.** Shmem stays in [0,64 MB); the ring-3 isolation
   makes the partition fix from the earlier session technically redundant but
   harmless to keep.
6. **`commitVirtualMemory` is a no-op or only touches truly new pages.**
   With per-process address spaces, the memset only affects one process's
   linear memory — but if JIT code is dual-mapped in the SAME process's
   user space at a non-overlapping user VA, there is no conflict.

---

## 13  Known Hard Problems

### 13.1  linMem base patching

WARP's `syncBasedataStart` writes an updated `linMem` value after each
`reallocAlignedMemory`.  It writes the kernel alias.  We need this to instead
write the ring-3 alias.  This requires intercepting `syncBasedataStart` or
patching the basedata field after every reallocation.

Approach: override `commitVirtualMemory` to also patch the basedata field:

```cpp
void commitVirtualMemory(void *ptr, size_t size) {
    // existing canonical remap...
    // then update ring-3 linMem base in basedata
    uint64_t r3_base = kernel_to_ring3_linmem(ptr);
    write_basedata_linmem_base(ctx->module, r3_base);
}
```

### 13.2  Trampoline VA in DYNAMIC_LINK symbol table

WARP's DYNAMIC_LINK symbol resolution (used by `initFromBytecode` for the
ring-3 path) stores `NativeSymbol::ptr` in the link-data section.  The JIT
code at function call time reads this pointer and does a CALL.  If the ptr is
a ring-3 VA (trampoline), the CALL from ring-3 JIT code works.  If the ptr is
a kernel VA (the current setup), the CALL from ring-3 faults immediately.

This is why ring-3 execution **requires** `warp_wasmos_symbols_ring3()` with
trampoline VAs as `ptr` values.

### 13.3  Stack fence

`k_stack_fence = 0xFFFFFFFF00000000` works for ring 0 (always below RSP).
For ring 3, the fence must be `ring3_stack_base` (below which RSP must not
drop).  This requires passing the fence per-process:

```cpp
const uint8_t *fence = (const uint8_t *)proc->warp_r3_stack_base;
mod->callExportedFunctionWithName<1>(fence, name, ...);
```

### 13.4  `mm_copy_to_user` in hostcalls

Some hostcalls copy data to/from the WASM process using kernel pointers.
With ring 3, those accesses still work via the kHalfBase alias of the
physical pages — no change needed as long as the dual-map is maintained.
Only if we drop the kHalfBase alias do we need `mm_copy_to_user`.

---

## 14  Relation to Existing Ring-3 Infrastructure

The existing ring-3 model (doc `11-ring3-isolation-and-separation.md`) already
provides:

- `mm_context_create()` / `mm_context_destroy()` — per-process PML4
- `paging_map_4k_in_root(root, va, pa, flags)` — mapping into a specific CR3
- `process_set_user_entry(pid, rip, rsp)` — ring-3 entry
- `x86_user_exception_handler` — ring-3 fault → terminate
- `INT 0x80` syscall ABI — proven by ring-3 smoke tests
- `MEM_REGION_FLAG_USER` flag checks — user-VA enforcement

The WARP ring-3 work adds a new **execution model** (JIT code at user VA,
hostcalls via INT 0x80) on top of this already-proven infrastructure.  The
first milestone (per-process CR3 with dual-mapped JIT) can be tested using
the existing halt test (`[calculator] ready` gate) without touching the
syscall layer at all.
