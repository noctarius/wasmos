# 28 — Symmetric Multi-Processing (SMP)

## Status

Implemented (Phases 0–9). `WASMOS_SMP=0` is the default; enabling it requires
`WASMOS_IRQ_IOAPIC=1`. All SMP bring-up code is live: MADT discovery,
LAPIC ICR helpers, AP trampoline, `smp_cpus_up`, AP C entry, per-CPU spinlock
state, and ready-queue locking. No behavioral change at `WASMOS_SMP=0`.

---

## Overview

WASMOS follows the Linux model for SMP bring-up: the kernel boots single-core
(BSP only), completes all single-core initialisation, then switches into
multi-core mode by waking Application Processors (APs) one at a time via the
INIT-SIPI-SIPI protocol. APs join the scheduler after the BSP's full init
sequence is complete.

SMP requires I/O APIC mode (`WASMOS_IRQ_IOAPIC`) and is controlled by a
separate build-time Kconfig option:

```
config WASMOS_SMP
    bool "Enable Symmetric Multi-Processing (SMP)"
    depends on WASMOS_IRQ_IOAPIC
    default n
```

When `WASMOS_SMP = 0` (the default), the kernel compiles and runs identically
to the pre-SMP baseline. The per-CPU data structure (`cpu_local_t`) exists in
both configurations; `cpu_local()` returns `&g_cpus[0]` unconditionally when
SMP is disabled.

---

## Steady-State AP Contract

After bring-up each online AP runs the identical scheduler loop as the BSP:

```c
while (1) { process_yield(); }
```

The contract for what an AP may do in steady state:

- **Kernel and ring3 scheduling.** APs share the global ready queue (protected by
  an IRQ-safe spinlock) and may dequeue and dispatch any kernel thread (ring0) or
  user thread (ring3). Dispatch is not restricted to kernel-only: the existing
  context-switch path loads the correct CR3 on each dispatch, so ring3 contexts
  execute normally on any CPU.
- **Shared address space.** All CPUs share the same higher-half kernel PML4.
  User-process mappings are added during spawn but are never removed while
  SMP bring-up is active (removal requires TLB shootdown, which is out of scope
  for this phase — see [MM/TLB Safety Contract](#mmtlb-safety-contract)). This
  makes user address space safe to enter from any AP without additional
  coordination.
- **Per-CPU isolation.** `current_process`, `current_thread`, `sched_ctx`,
  `preempt_disable_count`, and `in_scheduler` live in `cpu_local_t`. Each AP has
  its own copy; there is no cross-CPU scheduler state sharing outside the
  spinlock-protected ready queue.
- **No AP-exclusive device work.** APs do not own interrupts, perform MMIO init,
  or interact with the IOAPIC RTE table. Device IRQs remain BSP-delivered (see
  [Interrupt Affinity and Timer Delivery](#interrupt-affinity-and-timer-delivery)).
- **Per-CPU LAPIC timer.** Each AP runs its own periodic timer for quantum
  accounting. Timer ticks decrement the current thread's `ticks_remaining` on the
  CPU that is running that thread.

This is a **full multi-core kernel + ring3 scheduling model**. Per-CPU run queues
and work stealing are future optimisations.

---

## Kconfig / CMake wiring

`scripts/kconfig_to_cmake.py` maps `WASMOS_SMP` to a CMake bool cache variable
via the existing `BOOL_KEYS` table.

`CMakeLists.txt` translates it to `-DWASMOS_SMP=<0|1>` passed to the kernel
compile flags alongside the other `WASMOS_*` defines.

---

## Per-CPU data structure

### `cpu_local_t`  (`src/kernel/include/arch/x86_64/smp.h`)

```c
#define WASMOS_MAX_CPUS  16

typedef struct cpu_local {
    struct cpu_local    *self;              /* GS:0 — fast self-pointer */
    uint32_t             cpu_id;           /* logical index 0..N-1 */
    uint32_t             apic_id;          /* hardware LAPIC ID from MADT */
    volatile uint8_t     started;          /* AP writes 1 when fully online */

    /* x86 per-CPU tables */
    uint64_t    gdt[GDT_ENTRY_COUNT];
    tss_t       tss;
    uint8_t     ist_stack[IST_STACK_SIZE]   __attribute__((aligned(16)));
    uint8_t     rsp0_stack[RSP0_STACK_SIZE] __attribute__((aligned(16)));

    /* Scheduler state (previously global in process.c) */
    process_t         *current_process;
    thread_t          *current_thread;
    process_context_t  sched_ctx;
    uint32_t           preempt_disable_count;
    volatile uint8_t   in_scheduler;
} cpu_local_t;

extern cpu_local_t  g_cpus[WASMOS_MAX_CPUS];
extern uint32_t     g_cpu_count;   /* CPUs discovered in MADT, minimum 1 */
```

`tss_t` and `GDT_ENTRY_COUNT` are moved from the private section of
`cpu_x86_64.c` into `cpu_x86_64.h` so they can be used in the struct
definition above.

### `cpu_local()` accessor

```c
#if WASMOS_SMP
static inline cpu_local_t *cpu_local(void) {
    cpu_local_t *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));
    return p;
}
#else
static inline cpu_local_t *cpu_local(void) { return &g_cpus[0]; }
#endif
```

GS base is written via MSR `0xC0000101` (`IA32_GS_BASE`) at the end of
`x86_cpu_init()` for the BSP and at the start of `smp_ap_c_entry()` for each
AP. The self-pointer at `cpu_local_t.self` (offset 0) is set to
`&g_cpus[cpu_id]` before GS is loaded, so `mov %%gs:0` immediately returns the
correct pointer.

The IDT is a single shared global loaded by all CPUs (`lidt` in both
`x86_cpu_init()` and `smp_ap_c_entry()`). Each CPU has its own GDT and TSS.

---

## Boot sequence

```
kmain()
  ├─ ... memory, framebuffer, serial ...
  ├─ lapic_init()
  ├─ ioapic_init()          ← also runs MADT type-0 scan (SMP: fills g_cpus[])
  ├─ smp_init()             ← initialises g_cpus[0] (BSP), no-op when SMP=0
  ├─ ... scheduler_init(), drivers, services ...
  └─ smp_cpus_up()          ← sends INIT-SIPI-SIPI per AP, no-op when SMP=0
```

`smp_init()` runs immediately after the interrupt controllers are live. It sets
up `g_cpus[0]` (BSP) and records the AP count but does not wake any APs.

`smp_cpus_up()` runs after the scheduler is fully initialised so that APs can
enter the run loop as soon as they complete their per-CPU setup.

---

## MADT type-0 CPU discovery

`ioapic.c` already walks the MADT for type-1 (I/O APIC) and type-2 (IRQ
override) entries. The `madt_parse()` function gains a type-0 arm:

```c
typedef struct {
    acpi_madt_entry_t hdr;
    uint8_t           processor_uid;
    uint8_t           apic_id;
    uint32_t          flags;        /* bit 0: enabled */
} __attribute__((packed)) acpi_madt_processor_t;
```

For each enabled type-0 entry up to `WASMOS_MAX_CPUS`, `madt_parse()` records
the APIC ID in `g_cpus[g_cpu_count]` and increments `g_cpu_count`. The BSP
entry (APIC ID matching `lapic_read_id()`) is recognised and fills
`g_cpus[0]`; APs fill `g_cpus[1..N-1]`.

When `WASMOS_SMP = 0` the type-0 branch is compiled out and `g_cpu_count`
stays 1.

---

## Interrupt Affinity and Timer Delivery

IRQ routing in IOAPIC mode (the only mode that supports SMP) is BSP-centric for
Phase 0–9. The table below gives the complete picture:

| Source                        | Delivered to          | Notes |
|-------------------------------|-----------------------|-------|
| Device IRQs (IOAPIC RTE 1–15) | BSP (LAPIC 0) only    | All 16 RTEs are programmed with physical destination LAPIC 0. Reprogramming RTE destination for per-CPU affinity is not done in this phase. |
| Scheduler timer ticks          | Every online CPU      | Each AP calls `lapic_timer_init()` in `smp_ap_c_entry()`. Every CPU receives its own periodic LAPIC timer interrupt and runs `timer_handle_irq()` / `process_tick()` independently against its own `cpu_local()`. |
| IRQ0 / vector 32 (timer)      | BSP (LAPIC 0) only    | In IOAPIC mode the LAPIC timer replaces the PIT. The IOAPIC RTE for IRQ0 still points to physical LAPIC 0; APs receive the timer via their *own* LAPIC timer, not the IOAPIC route. Both paths fire vector 32 and reach `x86_timer_irq_handler`. |
| INIT/SIPI IPIs                | APs (targeted)        | Used only during bring-up. No runtime IPIs (remote wakeup, TLB shootdown) are issued in this phase. |

`docs/architecture/05-x86-cpu-architecture.md` describes IOAPIC RTE programming
in detail, including how all RTEs default to fixed delivery to LAPIC 0.

---

## LAPIC ICR helpers

Three new functions in `lapic.c` (guarded by `#if WASMOS_SMP`):

```c
uint32_t lapic_read_id(void);
void     lapic_send_init_ipi(uint32_t apic_id);
void     lapic_send_sipi(uint32_t apic_id, uint8_t vector);
```

`lapic_send_*` writes the destination APIC ID into ICR_HI (`LAPIC_REG + 0x310`)
then the command word into ICR_LO (`LAPIC_REG + 0x300`). Each send busy-waits
on the delivery-status bit (ICR_LO bit 12) before returning.

---

## AP trampoline  (`src/kernel/arch/x86_64/smp_trampoline.S`)

APs reset into 16-bit real mode. The trampoline is a small blob placed at
physical address `0x1000` (vector `0x01` for SIPI). It transitions through
16-bit → 32-bit protected → 64-bit long mode before calling into C.

### Data slots (written by BSP before SIPI)

| Physical address | Size     | Content                                 |
|------------------|----------|-----------------------------------------|
| `0x500`          | 8 bytes  | CR3 — kernel PML4 physical address      |
| `0x508`          | 8 bytes  | 64-bit C entry point (`smp_ap_c_entry`) |
| `0x510`          | 8 bytes  | Initial RSP (top of AP kernel stack)    |
| `0x518`          | 10 bytes | GDT pseudo-descriptor (limit + base)    |

The trampoline is either linked as a raw `.bin` section or embedded via
`.incbin` into a C array in `smp.c` and copied to `0x1000` at runtime. Two
separate 4 KiB PTEs cover the two low pages with distinct permissions:

- `0x1000` — trampoline code page: `MEM_REGION_FLAG_READ | WRITE | EXEC`.
  APs fetch and execute from this page during the 16→64-bit transition.
- `0x0000` — data slot page (holds the `0x500`–`0x518` slot block): `MEM_REGION_FLAG_READ | WRITE`, NX.
  Keeping this page non-executable means AP bring-up does not open execute
  permission on the zero-page.

The `0x1000` page is also reserved from the general page-frame allocator during
`pfa_init()` so shared-memory objects and kernel allocations cannot be placed on
the trampoline page.

### Transition outline

```
[16-bit real mode @ 0x1000]
  enable A20
  load GDT from [0x518]
  set CR0.PE=1
  far jump → [32-bit PM stub]

[32-bit PM stub]
  load CR3 from [0x500]
  enable PAE (CR4.PAE), set EFER.LME
  set CR0.PG=1
  far jump → [64-bit stub]

[64-bit stub]
  load RSP from [0x510]
  call *[0x508]              ; → smp_ap_c_entry(cpu_id)
```

---

## AP C entry  (`smp_ap_c_entry` in `smp.c`)

```c
void smp_ap_c_entry(uint32_t cpu_id)
{
    cpu_local_t *cpu = &g_cpus[cpu_id];

    /* Load this CPU's private GDT and TSS. */
    cpu_gdt_load(cpu);          /* lgdt + ltr */

    /* Set GS base so cpu_local() works from here on. */
    cpu->self = cpu;
    x86_write_msr(IA32_GS_BASE_MSR, (uint64_t)(uintptr_t)cpu);

    /* Shared IDT — same descriptor as BSP. */
    idt_load();

    /* Bring up this CPU's LAPIC and timer. */
    lapic_enable();
    lapic_timer_init();

    /* Signal BSP that we are live. */
    __atomic_store_n(&cpu->started, 1, __ATOMIC_RELEASE);

    x86_cpu_enable_interrupts();

    /* Join the scheduler. */
    while (1) {
        process_yield();
    }
}
```

---

## BSP wake-up sequence  (`smp_cpus_up` in `smp.c`)

For each AP `i` in `1..g_cpu_count-1`:

1. Allocate a kernel stack (from the page-frame allocator).
2. Write AP stack, CR3, entry point, GDT into data slots at `0x500`.
3. `lapic_send_init_ipi(g_cpus[i].apic_id)`.
4. Delay ~10 ms.
5. `lapic_send_sipi(g_cpus[i].apic_id, 0x01)`.
6. Delay ~200 µs.
7. Second `lapic_send_sipi` (Intel spec requires two SIPIs).
8. Spin on `g_cpus[i].started` with a ~100 ms timeout.

**On success:** the AP is live. `g_cpus[i].started == 1` and the AP is already
running `process_yield()` in the scheduler loop.

**On timeout (degraded mode):** the BSP logs a warning and continues. The
system is still valid:
- `g_cpu_count` is not decremented; it reflects MADT-discovered CPUs.
- The timed-out AP's `started` flag remains `0`; it never entered the
  scheduler loop, so no threads are dispatched to it.
- No retry is attempted. The AP is permanently absent for this boot.
- All threads remain schedulable on whichever CPUs did come online.
- If every AP times out the kernel continues single-core on the BSP only,
  identical to an `WASMOS_SMP=0` build except that the per-CPU spinlock
  overhead is still present.

---

## Scheduler changes

### Globals migrated to `cpu_local_t`

| Former global (process.c)       | New location                         |
|---------------------------------|--------------------------------------|
| `g_current_process`             | `cpu_local()->current_process`       |
| `g_current_thread`              | `cpu_local()->current_thread`        |
| `process_context_t g_sched_ctx` | `cpu_local()->sched_ctx`             |
| `g_preempt_disable_count`       | `cpu_local()->preempt_disable_count` |
| `g_in_scheduler`                | `cpu_local()->in_scheduler`          |

`preempt_disable()`, `preempt_enable()`, and `preempt_disable_depth()` become
thin wrappers around `cpu_local()->preempt_disable_count`.

### Ready queue

The round-robin ready queue (`g_ready_queue[]` in `process.c`) remains a single
global for the initial SMP phase. `ready_queue_enqueue()` and
`ready_queue_dequeue()` are wrapped with an IRQ-safe spinlock
(`cli`+`xchg` acquire, `xchg`+`sti` release). Per-CPU queues with work
stealing are a later optimisation.

### Ready-gated spawn for service/driver children

Under SMP, an AP can pick up a newly enqueued process before process-manager
has finished setting `require_explicit_ready = 1`. If the child blocks on IPC
before PM arms the sync-ready wait, it auto-marks itself ready and the
`start` call returns prematurely while the service is still initialising.

The fix is `process_spawn_ready_gated()`: it sets `ready = 0` and
`require_explicit_ready = 1` on the process *before* enqueuing it, so no AP
can observe the process in a state where it can auto-mark itself ready. This
helper is used for all service and driver WASMOS-APP children spawned by
process-manager sync paths.

---

## Spinlock upgrade

The current `spinlock.h` disables interrupts on the calling CPU, which is
sufficient for single-core. With multiple CPUs, both interrupts and bus-locked
`xchg` / `cmpxchg` are required.

Upgrade plan for `spinlock_acquire()`:

```c
static inline void spinlock_acquire(spinlock_t *lock) {
    x86_cpu_disable_interrupts();
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        x86_cpu_enable_interrupts();
        __asm__ volatile("pause");
        x86_cpu_disable_interrupts();
    }
}
```

The `pause` instruction reduces bus contention during spin and is the canonical
Intel recommendation for spinlock back-off.

---

## Memory barriers

Accesses to `cpu_local()->started` use `__ATOMIC_RELEASE` (store) and
`__ATOMIC_ACQUIRE` (load) to ensure the AP's initialisation stores are visible
to the BSP before `started` is read as 1. All other inter-CPU state (ready
queue, process table) is protected by spinlocks, which imply full barriers on
acquire and release.

---

## MM/TLB Safety Contract

TLB shootdown IPIs are out of scope for this phase. This section explains why
the current design is still safe and what operations are therefore constrained.

**Why no shootdown is needed now.**
The kernel address space is identical on all CPUs — all share the same higher-half
PML4 subtree. User-process page tables are extended (mappings added) during spawn
and demand paging, but no mapping is ever *removed* while SMP is active in Phase 0–9:
process teardown (`process_reap`) does not execute concurrently on another CPU
because the reaped process is in `ZOMBIE` state and no AP will dequeue it from the
ready queue. No currently-running mapping is unmapped from under a live CPU.

**Scope of AP address-space access.**
APs may dispatch ring3 threads and enter user address spaces. The CR3 loaded on
dispatch points to the process PML4, which is only extended (never shrunk) while
the process is live. An AP that is mid-execution in a user address space will
never see a present-page disappear from under it.

**What must remain BSP-serialized.**
Mapping changes that could race with a concurrent AP execution are serialized
because they either (a) only happen before a process is runnable (spawn-time
mappings) or (b) only happen after the process is zombie and unreachable from
the ready queue. The physical page-frame allocator and the page-table walker
are protected by spinlocks.

**Future constraint.**
Once process teardown reclaims live page-table entries under concurrent APs
(required for multi-process shared memory revocation at ring3 scale), TLB
shootdown IPIs must be added before that path is enabled.

---

## What is out of scope for this phase

- Per-CPU ready queues and work stealing.
- TLB shootdown IPIs (see [MM/TLB Safety Contract](#mmtlb-safety-contract) for
  why the current phase is safe without them).
- CPU hotplug / ACPI re-enumeration.
- NUMA awareness.

---

## Validation

### Required build configuration

```
WASMOS_SMP=1        (depends on WASMOS_IRQ_IOAPIC=1, the default IRQ mode)
```

QEMU must expose at least 2 vCPUs:

```
qemu-system-x86_64 ... -smp 2
```

### Expected boot evidence (serial log)

| Log fragment | Meaning |
|---|---|
| BSP reaches `smp_cpus_up` | SMP bring-up path entered |
| AP `g_cpus[1].started` transitions to 1 | AP 1 came online successfully |
| `[test] sched progress ok` | Scheduler dispatched `SCHED_PROGRESS_MARKER_SWITCHES` threads; with 2 CPUs this proves cross-CPU dispatch |
| `[test] preempt ok` | Timer preemption fired on at least one CPU |
| No `[smp] AP N timeout` warning | All MADT-discovered APs came online within the 100 ms window |

### Regression targets

| Target | What it proves |
|---|---|
| `cmake --build build --target run-qemu-test` | Default `WASMOS_SMP=0` baseline is not regressed by the SMP infrastructure changes |
| Manual SMP build with `-DWASMOS_SMP=1 -DQEMU_SMP=2` | AP comes online, scheduler dispatches on both CPUs |

The default `run-qemu-test` CI gate always runs with `WASMOS_SMP=0`. An explicit
SMP-enabled build must be run manually to exercise the AP bring-up path.

---

## Files changed / created

| File                                          | Change                                                                                           |
|-----------------------------------------------|--------------------------------------------------------------------------------------------------|
| `Kconfig`                                     | Add `WASMOS_SMP` option (depends on `WASMOS_IRQ_IOAPIC`)                                         |
| `scripts/kconfig_to_cmake.py`                 | Add `"WASMOS_SMP"` to `BOOL_KEYS`                                                                |
| `CMakeLists.txt`                              | Add option, value, define; pass to kernel CFLAGS                                                 |
| `src/kernel/include/arch/x86_64/cpu_x86_64.h` | Expose `tss_t`, `GDT_ENTRY_COUNT`, stack size constants                                          |
| `src/kernel/include/arch/x86_64/smp.h`        | New — `cpu_local_t`, `g_cpus[]`, `cpu_local()` accessor                                          |
| `src/kernel/arch/x86_64/smp.c`                | New — `g_cpus[]` definition, `smp_init()`, `smp_cpus_up()`, `smp_ap_c_entry()`, trampoline setup |
| `src/kernel/arch/x86_64/smp_trampoline.S`     | New — 16→32→64-bit AP boot stub                                                                  |
| `src/kernel/arch/x86_64/cpu_x86_64.c`         | Migrate to `g_cpus[0]`; GS base MSR setup; remove static GDT/TSS/stacks                          |
| `src/kernel/arch/x86_64/ioapic.c`             | Add MADT type-0 CPU discovery (`#if WASMOS_SMP`)                                                 |
| `src/kernel/arch/x86_64/lapic.c`              | Add ICR helpers: `lapic_read_id`, `lapic_send_init_ipi`, `lapic_send_sipi`                       |
| `src/kernel/include/arch/x86_64/lapic.h`      | Declare new ICR helpers                                                                          |
| `src/kernel/process.c`                        | Replace scheduler globals with `cpu_local()` accessors; add ready-queue spinlock                 |
| `src/kernel/include/process.h`                | Update `preempt_disable_*` to use `cpu_local()`                                                  |
