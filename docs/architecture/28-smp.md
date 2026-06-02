# 28 — Symmetric Multi-Processing (SMP)

## Status

Planned. `WASMOS_SMP` Kconfig option not yet implemented.
All code in this document describes the target design, not the current state.

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
`.incbin` into a C array in `smp.c` and copied to `0x1000` at runtime. The
`0x1000` page is identity-mapped before the first SIPI.

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
8. Spin on `g_cpus[i].started` with a ~100 ms timeout; log a warning and
   continue on timeout.

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

## What is out of scope for this phase

- Per-CPU ready queues and work stealing.
- TLB shootdown IPIs (needed when user-process mappings are removed; stubbed
  until ring-3 SMP is required).
- CPU hotplug / ACPI re-enumeration.
- NUMA awareness.

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
