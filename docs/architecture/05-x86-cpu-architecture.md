## x86_64 CPU Architecture

This document describes the low-level x86_64 CPU setup: the GDT, IDT, and TSS
structures; the PIC-based IRQ routing model; exception handling and page fault
classification; and the syscall gate.

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

### PIC and IRQ Routing

**Source**: `arch/x86_64/irq_x86_64.c`

```c
#define IRQ_VECTOR_BASE 32
#define IRQ_COUNT       16

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

#define IPC_IRQ_EVENT_TYPE 0xFF00u
```

`x86_irq_init()` (called from `x86_cpu_init`):
1. Remaps PIC1 to base vector 32, PIC2 to base 40 (standard x86 remapping).
2. Sets cascade (0x04/0x02) and 8086 mode for both PICs.
3. Restores the saved IMR masks (`g_pic_mask1 = 0xFF`, `g_pic_mask2 = 0xFF`
   — all IRQs masked at boot).

#### IRQ Route Table

```c
typedef struct {
    uint8_t  in_use;
    uint32_t owner_context_id;
    uint32_t endpoint;
} irq_route_t;

static irq_route_t g_irq_routes[IRQ_COUNT];
```

`irq_route_ipc(irq_line, endpoint, context_id)`:
- Validates policy via `policy_authorize(POLICY_ACTION_IRQ_ROUTE, irq_line)`.
- Stores the endpoint and owner in `g_irq_routes[irq_line]`.
- Unmasks the corresponding PIC bit.

`x86_irq_ack(context_id, irq_line)`:
- Verifies the caller owns the route (`owner_context_id` match).
- Sends EOI to PIC1 and, if IRQ ≥ 8, to PIC2.

`irq_unroute(irq_line, context_id)`:
- Clears the route entry.
- Masks the PIC bit.

#### IRQ Dispatch

`x86_irq_handler(vector)` runs in interrupt context from the ISR stub:
1. Computes `irq_line = vector - IRQ_VECTOR_BASE`.
2. If the route has an endpoint, sends `IPC_IRQ_EVENT_TYPE (0xFF00)` to that
   endpoint using the fast IPC send path.
3. **Does not send EOI** — that is the responsibility of the IRQ owner process
   via `irq_ack()`. This prevents level-triggered re-fire before the device
   register has been read.
4. For IRQ0 (timer), calls the timer tick handler.
5. Checks for IST1 stack overflow via the canary at the IST1 base.

Spurious IRQ handling: reads the In-Service Register (ISR) from PIC1/PIC2 and
discards the EOI if the corresponding bit is clear.

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
x86_cpu_init()
  ├─ x86_write_msr(IA32_EFER, NXE enabled)
  ├─ tss_init()    ← fill IST stacks, write canary, install TSS in GDT
  ├─ gdt_install() ← lgdt, far-lretq to reload CS, ltr TSS
  ├─ idt_install() ← install exception stubs 0–31, lidt
  ├─ install IRQ stubs 32–47 (IRQ0 on IST1)
  ├─ install syscall gate at vector 0x80 (DPL 3)
  └─ irq_init()    ← x86_irq_init() → PIC remap + mask all
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

3. **EOI is the IRQ owner's responsibility.** The kernel sends no EOI from the
   interrupt handler. The owning process calls `irq_ack()` after reading the
   device register. Sending EOI before reading the device can cause
   level-triggered re-fire.

4. **IST1 canary.** `g_irq0_ist_canary` is written at the bottom of the IST1
   stack. Overflow is detected in `x86_irq_handler` before dispatching.

5. **GDT TSS entry is a 16-byte system descriptor.** Entries 5 and 6 together
   form one TSS descriptor (the 64-bit system descriptor format). `ltr 0x28`
   loads the full TSS.

6. **Higher-half relocation is done before `sti`.** Interrupts remain disabled
   until after `x86_cpu_relocate_tables_high()` and `x86_cpu_enable_interrupts()`
   are called from `kmain`.
