## x86_64 CPU Architecture

This document describes the low-level x86_64 CPU setup: the GDT, IDT, and TSS
structures; the interrupt controller and IRQ routing model; exception handling
and page fault classification; and the syscall gate.

All code lives in `src/kernel/arch/x86_64/`.

---

### Descriptor Tables

**Source**: `cpu_x86_64.c`

#### GDT

```c
#define GDT_ENTRY_COUNT 7

#define KERNEL_CS_SELECTOR  0x08
#define KERNEL_DS_SELECTOR  0x10
#define USER_CS_SELECTOR    0x18
#define USER_DS_SELECTOR    0x20
#define KERNEL_TSS_SELECTOR 0x28  /* two slots: TSS low + TSS high */
```

The static GDT has 7 entries (indices 0–6):

| Index | Selector | Value                | Meaning                                       |
|-------|----------|----------------------|-----------------------------------------------|
| 0     | —        | `0x0000000000000000` | Null                                          |
| 1     | `0x08`   | `0x00AF9A000000FFFF` | Kernel CS — long mode, DPL 0, non-conforming  |
| 2     | `0x10`   | `0x00AF92000000FFFF` | Kernel DS — writable data, DPL 0              |
| 3     | `0x18`   | `0x00AFFA000000FFFF` | User CS — long mode, DPL 3                    |
| 4     | `0x20`   | `0x00AFF2000000FFFF` | User DS — writable data, DPL 3                |
| 5     | `0x28`   | Computed             | TSS low (64-bit system descriptor, type 0x89) |
| 6     | `0x2C`   | Computed             | TSS high (upper 32 bits of TSS base)          |

`gdt_install()` loads GDTR, performs a far-return (`lretq`) to reload CS to
`0x08`, reloads DS/ES/SS to `0x10`, then loads the TSS selector via `ltr`.

#### TSS

```c
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;       // kernel stack for ring0 entry from ring3
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;       // dedicated stack for IRQ0 (timer)
    uint64_t ist2–ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;       // set to sizeof(tss_t) — no I/O permission bitmap
} tss_t;
```

Constants:
- `IRQ0_IST_INDEX = 1` — timer uses IST slot 1
- `IRQ0_IST_STACK_SIZE = 16384` — 16 KiB for IST1 and rsp0 stacks
- `g_irq0_ist_canary = 0xCAFEBABEDEADC0DE` — stack underflow sentinel written to IST1 base

`tss_init()` fills `g_irq0_ist_stack` with `0xCC` bytes, writes the canary at
the base, then sets `g_tss.rsp0` and `g_tss.ist1` to the top of their
respective stacks. `iopb` is set to `sizeof(g_tss)` to deny I/O permission
bitmap access to user mode.

`x86_cpu_set_kernel_stack(rsp0)` updates `g_tss.rsp0` at context-switch time
to point to the per-process kernel stack.

#### IDT

```c
#define IDT_ENTRY_COUNT 256
#define EXCEPTION_COUNT 32

#define IDT_TYPE_INTERRUPT_GATE      0x8E  // DPL 0, present
#define IDT_TYPE_INTERRUPT_GATE_USER 0xEE  // DPL 3, present (syscall gate)
```

`idt_install()` zeros all 256 entries, then installs:
- Vectors 0–31: exception stubs from `x86_exception_stub_table[]` using
  `IDT_TYPE_INTERRUPT_GATE` (DPL 0, kernel-only).
- Vectors 32–47 (`IRQ_VECTOR_BASE` to `IRQ_VECTOR_BASE + IRQ_COUNT - 1`):
  IRQ stubs from `x86_irq_stub_table[]` using `IDT_TYPE_INTERRUPT_GATE`.
  Vector 32 (IRQ0, timer) uses `idt_set_gate_ist(..., IRQ0_IST_INDEX)` to
  run on the dedicated IST1 stack.
- Vector `0x80` (128): `isr_syscall_128` with `IDT_TYPE_INTERRUPT_GATE_USER`
  (DPL 3, callable from ring 3 via `int 0x80`).

#### Higher-Half Relocation

`x86_cpu_relocate_tables_high()` is called after the kernel remaps to the
higher-half window. It adjusts all physical pointers in the GDT (TSS base),
TSS (rsp0, ist1), and reloads GDTR/IDTR/TR with the high-alias addresses.

---

### Interrupt Controller and IRQ Routing

**Source**: `arch/x86_64/irq_x86_64.c`, `arch/x86_64/lapic.c`,
`arch/x86_64/ioapic.c`

The interrupt controller is selected at build time via the `WASMOS_IRQ_MODE`
compile-time define, configured through the Kconfig `choice`
`WASMOS_IRQ_PIC / WASMOS_IRQ_LAPIC / WASMOS_IRQ_IOAPIC`:

| Mode | `WASMOS_IRQ_MODE` | Controller | Timer source | Ext. device IRQs |
|------|-------------------|------------|--------------|------------------|
| `WASMOS_IRQ_PIC`   | 0 | Legacy 8259 PIC + PIT ch.0 | PIT channel 0 | via 8259 |
| `WASMOS_IRQ_LAPIC` | 1 | LAPIC (no ext. IRQs) | LAPIC periodic timer | **none** |
| `WASMOS_IRQ_IOAPIC`| 2 | LAPIC + I/O APIC | LAPIC periodic timer | via IOAPIC |

All three modes share the same IDT vectors (32–47 for IRQs, 0x80 for syscall)
and the same IPC-based IRQ routing table. The default is `WASMOS_IRQ_IOAPIC`.

**LAPIC-only mode limitation**: the 8259 PIC is fully masked and there is no
I/O APIC, so external device IRQs (keyboard IRQ 1, mouse IRQ 12, etc.) have
no delivery path to the CPU. `x86_irq_register` returns `-1` for any
`irq_line != 0` in this mode so that drivers fall back to polling rather than
blocking forever on `ipc_recv`.

```c
#define IRQ_VECTOR_BASE 32
#define IRQ_COUNT       16

#define IPC_IRQ_EVENT_TYPE 0xFF00u
```

#### Two-Phase Interrupt Init

- **Phase 1** — `x86_irq_init()` (called from `x86_cpu_init` inside `kmain`):
  clears the IRQ route table. In PIC mode, also remaps PIC1 to base vector 32,
  PIC2 to base 40, and masks all IRQs.

- **Phase 2** — `irq_late_init(boot_info)` (called from `kmain` after
  `timer_init()`, which runs after `mm_init()`): dispatches to
  `x86_irq_late_init(boot_info)`. In IOAPIC mode, this calls `ioapic_init()`,
  which needs both `paging_map_4k()` (requires mm/paging up) and
  `boot_info->rsdp` for MADT discovery. In PIC and LAPIC modes this is a no-op.

The LAPIC timer is initialized inside `timer_init()` (phase 1.5) rather than in
either init phase: it calls `lapic_init(hz)`, which maps the LAPIC MMIO page,
enables the LAPIC, disables the 8259, and starts the periodic timer.

#### PIC Mode (WASMOS_IRQ_MODE == 0)

`x86_irq_init()` remaps the 8259:
1. PIC1 to base vector 32, PIC2 to base 40.
2. Sets cascade (0x04/0x02) and 8086 mode.
3. Restores saved IMR masks — all lines masked until `irq_unmask()` is called.

PIC EOI: `irq_send_eoi()` sends `PIC_EOI (0x20)` to `PIC1_CMD`, and also to
`PIC2_CMD` for IRQs ≥ 8. EOI is sent by the kernel after dispatching the IRQ
message to the owning endpoint, but **only after masking the line** to prevent
level-triggered re-fire before the driver reads the device register.

#### LAPIC Mode (WASMOS_IRQ_MODE == 1)

`lapic_init(hz)`:
1. Reads the LAPIC physical base from `IA32_APIC_BASE MSR (0x1B)`, bits [51:12].
2. Maps one 4 KB MMIO page at kernel VA `0xFFFFFFFF80001000`
   (in the unmapped 2 MB gap below the kernel image) with cache-disable flags
   (`PT_FLAG_PCD`).
3. Enables the LAPIC via the SVR register (bit 8) and sets the spurious vector
   to `0xFF` (255).
4. Masks all 8259 PIC lines (`outb(0x21, 0xFF); outb(0xA1, 0xFF)`).
5. Calibrates LAPIC ticks-per-ms using PIT channel 2 in one-shot mode (10 ms
   reference window), then programs the LVT_TIMER in periodic mode at the
   requested `hz`, targeting vector 32 (IRQ_VECTOR_BASE).

The spurious vector 0xFF is installed in the IDT as `isr_lapic_spurious`, which
performs only `iretq` (per Intel SDM Vol 3A §10.9 — no EOI for spurious vectors).

LAPIC EOI: `irq_send_eoi()` calls `lapic_eoi()` (writes 0 to LAPIC EOI register
at offset `0x0B0`). This is issued from the kernel, not deferred to the driver.
External device IRQs are not routed in LAPIC-only mode (no I/O APIC).

#### IOAPIC Mode (WASMOS_IRQ_MODE == 2)

Builds on LAPIC mode (LAPIC timer and EOI are identical). `ioapic_init(boot_info)`
(called from `irq_late_init`):

1. **MADT discovery**: reads `boot_info->rsdp + 24` for the XSDT physical
   address; walks XSDT entries looking for the `"APIC"` signature; extracts:
   - type-1 entry: I/O APIC physical base (default `0xFEC00000`)
   - type-2 entries: ISA IRQ source overrides → GSI map (`g_gsi_map[isa_irq] = gsi`)

2. **MMIO mapping**: maps the I/O APIC MMIO page at kernel VA `0xFFFFFFFF80002000`
   (one page above the LAPIC) using `paging_map_4k()` with cache-disable flags.

3. **RTE programming**: programs all 16 ISA Redirection Table Entries via
   indirect MMIO access (IOREGSEL at offset `0x00`, IOWIN at `0x10`). Each RTE
   is written masked (`bit 16 = 1`), **level-triggered**, active-high, fixed
   delivery to physical LAPIC 0 (BSP), at vector `IRQ_VECTOR_BASE + isa_irq`
   (32–47).

   ISA bus defaults are edge-triggered, but the kernel uses level-triggered for
   all 16 RTE entries because of the mask-based dispatch model: the RTE is
   masked immediately after delivery and unmasked only after the driver reads
   the hardware register (`irq_ack`).  In edge-triggered mode, any rising edge
   that arrives while the RTE is masked is silently discarded.  PS/2 devices
   (i8042 IRQ 1 / IRQ 12) hold the IRQ line HIGH while OBF is set and
   immediately reload OBF from their internal buffer — so the line never
   deasserts between back-to-back bytes.  With level-triggered mode, unmasking
   the RTE re-delivers immediately if the line is still HIGH, correctly draining
   queued bytes.

Drivers unmask their IRQ line via the existing `irq_register()` → `irq_unmask()`
→ `x86_irq_unmask()` → `ioapic_unmask_irq()` path, which clears bit 16 of the
RTE's low word. Re-masking after dispatch (`x86_irq_mask()` → `ioapic_mask_irq()`)
prevents re-fire before the driver reads the device register; the driver
re-enables via `irq_ack()`.

For level-triggered RTEs the LAPIC automatically broadcasts an EOIS (EOI
signal) to all I/O APICs when EOI is written, clearing the Remote IRR bit and
allowing re-delivery once the RTE is unmasked.  No separate IOAPIC EOI write is
needed from software.

#### IRQ Route Table

```c
typedef struct {
    uint8_t  in_use;
    uint32_t owner_context_id;
    uint32_t endpoint;
} irq_route_t;

static irq_route_t g_irq_routes[IRQ_COUNT];
```

`x86_irq_register(context_id, irq_line, endpoint)`:
- Validates policy via `policy_authorize(POLICY_ACTION_IRQ_ROUTE, irq_line)`.
- Verifies the endpoint is owned by `context_id`.
- Stores the endpoint and owner in `g_irq_routes[irq_line]`.
- Calls `x86_irq_unmask(irq_line)` — dispatches to PIC unmask or IOAPIC RTE
  clear depending on `WASMOS_IRQ_MODE`.

`x86_irq_ack(context_id, irq_line)`:
- Verifies the caller owns the route.
- Calls `x86_irq_unmask(irq_line)` to re-enable the line after the driver has
  read the device register.

`x86_irq_unregister(context_id, irq_line)`:
- Clears the route entry and masks the line.

#### IRQ Dispatch

`x86_irq_handler(vector)` runs in interrupt context from the ISR stub:
1. Computes `irq_line = vector - IRQ_VECTOR_BASE`.
2. In PIC mode: checks for spurious IRQ (reads ISR from PIC1/PIC2; discards if
   the corresponding ISR bit is clear).
3. For IRQ0 (timer), calls `timer_handle_irq()`.
4. If the route has an endpoint and `irq_line != 0`, masks the line to prevent
   re-fire before the driver reads the device register.
5. Sends `IPC_IRQ_EVENT_TYPE (0xFF00)` to the routed endpoint.
6. Calls `irq_send_eoi(irq_line)`: PIC EOI in mode 0, `lapic_eoi()` in modes 1
   and 2.

---

### Exception Handling

**Source**: `cpu_x86_64.c`

#### Page Fault Classification

`x86_page_fault_handler(error_code, frame)`:
1. Reads CR2 (faulting address).
2. Calls `memory_service_handle_fault_ipc(context_id, cr2, error_code)` to
   attempt demand-paging resolution.
3. If unhandled and the fault came from user mode (CS bits [1:0] == 3),
   classifies the reason:

```c
typedef enum {
    PF_REASON_UNMAPPED,        // page not present
    PF_REASON_WRITE_VIOLATION, // present but write on read-only
    PF_REASON_EXEC_VIOLATION,  // present but NX violation
    PF_REASON_USER_TO_KERNEL,  // user CR2 address outside [USER_VA_MIN, USER_VA_MAX)
    PF_REASON_PROTECTION,      // other present-page violation
} pf_reason_t;
```

```c
#define USER_VA_MIN 0x0000008000000000ULL
#define USER_VA_MAX 0x0000010000000000ULL
```

On unhandled user fault: logs `[fault] user-pf pid=N reason=X err=... cr2=...`,
emits any ring3 test markers, calls `process_set_exit_status(proc, -11)`, then
`process_yield(PROCESS_RUN_EXITED)`.

#### User Exception Handler

`x86_user_exception_handler(vector, frame)` handles non-PF user exceptions.
Only vectors in `{0, 1, 4, 6, 7, 12, 13, 17}` are caught:

| Vector | Fault                    |
|--------|--------------------------|
| 0      | #DE divide error         |
| 1      | #DB debug                |
| 4      | #OF overflow             |
| 6      | #UD invalid opcode       |
| 7      | #NM device not available |
| 12     | #SS stack segment        |
| 13     | #GP general protection   |
| 17     | #AC alignment check      |

All other vectors return -1 and escalate to the kernel panic path. On a caught
user exception: logs `[fault] user-exc pid=N vector=N rip=...`, emits any
ring3 test markers, sets exit status -11, and yields.

#### Kernel Panic

`x86_exception_panic_frame(vector, frame)` is the unhandled exception path:
1. Reads CR2 (for #PF) and CR3.
2. Extracts `err`, `rip`, `cs`, `rflags` from the interrupt frame.
3. Dumps registers to serial via `serial_printf_unlocked`.
4. If RIP is within `[__kernel_start, __kernel_end]`, dumps 16 bytes at RIP.
5. Calls `panic_render_screen()` which writes a structured panic screen to the
   framebuffer (vector, registers, CR2/CR3, pid, process name, stack range,
   kernel text range, framebuffer info if available).
6. `cli` + `hlt` loop — system halted.

---

### Syscall Gate

Vector `0x80` is installed with `IDT_TYPE_INTERRUPT_GATE_USER` (DPL 3),
making it callable from ring-3 userspace via `int 0x80`. The handler
`isr_syscall_128` in `cpu_isr.S` saves the register frame and calls the
kernel syscall dispatcher in `syscall.c`.

WASM processes do not use the syscall gate directly. The gate is used by
native ring-3 ELF processes (see `ring3_native.ld`).

---

### NXE (No-Execute Enable)

`x86_cpu_init()` reads `IA32_EFER_MSR (0xC0000080)` and sets the NXE bit
`(1 << 11)` if not already set. This enables the NX bit in page table entries,
which is used by the paging layer to mark non-executable regions.

---

### Initialization Sequence

Called from `kmain()` after early serial and memory setup:

```
x86_cpu_init()                              ← phase 1
  ├─ x86_write_msr(IA32_EFER, NXE enabled)
  ├─ tss_init()    ← fill IST stacks, write canary, install TSS in GDT
  ├─ gdt_install() ← lgdt, far-lretq to reload CS, ltr TSS
  ├─ idt_install() ← install exception stubs 0–31, lidt
  ├─ install IRQ stubs 32–47 (IRQ0 on IST1)
  ├─ install syscall gate at vector 0x80 (DPL 3)
  ├─ [WASMOS_IRQ_MODE >= 1] install isr_lapic_spurious at vector 255
  └─ irq_init() → x86_irq_init()
       ├─ clear g_irq_routes[]
       └─ [mode 0] PIC remap + mask all lines

mm_init() + further kernel init ...         ← paging/physmem ready

timer_init(250)                             ← phase 1.5
  ├─ [mode 0] PIT channel 0 → sq-wave 250 Hz, irq_unmask(0)
  └─ [mode >= 1] lapic_init(250)
       ├─ lapic_map()       ← paging_map_4k at 0xFFFFFFFF80001000
       ├─ lapic_enable()    ← SVR write, MSR global enable
       ├─ pic_disable()     ← outb(0x21/0xA1, 0xFF)
       └─ lapic_timer_set_hz(250) ← calibrate via PIT ch.2, program LVT_TIMER

irq_late_init(boot_info)                    ← phase 2
  └─ [mode 2] ioapic_init(boot_info)
       ├─ madt_parse()      ← RSDP → XSDT → "APIC" table, GSI overrides
       ├─ ioapic_map()      ← paging_map_4k at 0xFFFFFFFF80002000
       └─ ioapic_program_rtes() ← 16 RTEs, all masked, vectors 32–47

cpu_enable_interrupts()                     ← sti
```

After `x86_cpu_relocate_tables_high()`: GDTR, IDTR, and TR are reloaded with
higher-half virtual addresses.

`x86_cpu_enable_interrupts()` / `x86_cpu_disable_interrupts()` wrap
`sti` / `cli`.

---

### Structural Invariants

1. **All exception gates are DPL 0.** User code cannot invoke exception
   vectors directly; the only DPL-3 IDT gate is vector 0x80.

2. **IRQ0 runs on IST1.** The timer interrupt has its own stack regardless of
   the interrupted stack pointer, preventing stack confusion on preemption of
   ring-3 code.

3. **EOI semantics depend on mode.** In PIC mode (`WASMOS_IRQ_MODE == 0`), EOI
   is sent by the kernel from the interrupt handler (after masking the line and
   dispatching the IPC message). In LAPIC and IOAPIC modes, `lapic_eoi()` is
   sent by the kernel at the end of `x86_irq_handler`; no separate driver EOI
   is required. Non-timer IRQ lines are masked at dispatch time to prevent
   re-fire; the driver re-enables via `irq_ack()` → `x86_irq_unmask()`.

4. **IST1 canary.** `g_irq0_ist_canary` is written at the bottom of the IST1
   stack. Overflow is detected in `x86_irq_handler` before dispatching.

5. **GDT TSS entry is a 16-byte system descriptor.** Entries 5 and 6 together
   form one TSS descriptor (the 64-bit system descriptor format). `ltr 0x28`
   loads the full TSS.

6. **Higher-half relocation is done before `sti`.** Interrupts remain disabled
   until after `x86_cpu_relocate_tables_high()` and `x86_cpu_enable_interrupts()`
   are called from `kmain`.

7. **IOAPIC init deferred after `mm_init()`.** `paging_map_4k()` needs the
   physmem allocator for page-table pages. `ioapic_init()` (and `lapic_map()`)
   are therefore called after `mm_init()`, not from the early `x86_cpu_init()`
   path.
