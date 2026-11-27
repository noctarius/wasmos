## aarch64 Platform (QEMU `virt`)

> **Documentation status: Implemented reference.** Describes the aarch64 port as
> it runs in the source tree; open work is listed under *Deferred Work* and in
> the "arm64 Port" section of `docs/TASKS.md`.

**Related**: [04-boot-and-handoff.md](04-boot-and-handoff.md) (the UEFI loader
both architectures share), [05-x86-cpu-architecture.md](05-x86-cpu-architecture.md)
(the x86_64 counterpart of this document),
[06-memory-management.md](06-memory-management.md),
[16-device-manager-and-bus-enumeration.md](16-device-manager-and-bus-enumeration.md),
[20-graphics-framebuffer-and-compositor.md](20-graphics-framebuffer-and-compositor.md).

---

### Target

The aarch64 port targets QEMU's `virt` machine with a GICv3
(`-M virt,gic-version=3`), booted by edk2 (AAVMF). It runs under Hypervisor.framework
on Apple Silicon (`-accel hvf -cpu host`) and under TCG on CI's aarch64-linux
runner. Bare-metal hardware is out of scope.

| Aspect | aarch64 | x86_64 for comparison |
|---|---|---|
| Configuration | `configs/aarch64_wasm3_{single,smp}_defconfig`, `aarch64_warp_single_defconfig` | four defconfigs |
| CPUs | single or SMP (PSCI `CPU_ON`) | single or SMP (INIT-SIPI-SIPI) |
| WASM runtime | wasm3; WARP builds (JIT only) but its ring-3 hooks are not implemented | wasm3 or WARP |
| Loader | `BOOTAA64.EFI` | `BOOTX64.EFI` |
| Interrupts | GICv3 + ITS (MSI/MSI-X as LPIs) | LAPIC/IOAPIC (or PIC) |
| Timer | EL1 virtual timer | LAPIC timer (or PIT) |
| PCI config | ECAM from the ACPI MCFG | port 0xCF8 |
| Console | PL011 (ACPI `ARMH0011`) | 16550 on COM1 |
| Clock | PL031 or UEFI runtime services (Kconfig) | CMOS |
| Display | virtio-gpu (ramfb selectable) | bochs VBE (virtio-gpu selectable) |
| Keyboard, mouse | virtio-input | PS/2 |
| Boot disk | virtio-blk | ATA |
| Power off | PSCI `SYSTEM_OFF` | ACPI |

Everything CPU- or platform-specific sits behind the arch interface headers
(`src/kernel/include/arch/*.h`), implemented in `src/kernel/arch/aarch64/`; every
per-architecture build and QEMU value is one entry of `ARCHES` in
`scripts/buildgen/toolchains.py` (toolchain triples, objcopy targets, firmware,
the machine, display and input devices). The UEFI loader is built for the
`aarch64-unknown-windows` triple: clang has no aarch64 UEFI triple, and that
target emits the PE/COFF lld-link needs with the AAPCS64 calling convention UEFI
uses.

### Boot and Exception Levels

The loader is the same `src/boot/boot.c` as on x86_64
([04-boot-and-handoff.md](04-boot-and-handoff.md)). Two aarch64 differences
matter after it: the firmware's GOP on a virtio GPU is blit-only, so the loader
allocates the scanout itself, and the loader calls `SetVirtualAddressMap` with
every UEFI runtime region at its kernel direct-map address, which is what lets
the kernel call the firmware's clock later (`boot_info` version 7).

`entry.S` drops from EL2 to EL1 when entered at EL2, builds the direct map,
enables the MMU and calls `kmain`. The kernel runs at EL1 and user space at
EL0: an `SVC` from EL0 enters `syscall_dispatch` (number in `x8`, result in
`x0`), and EL0 exceptions land on the per-CPU kernel entry stack
(`cpu_set_kernel_stack`, the counterpart of x86_64's `TSS.rsp0`). The vectors
(`vectors.S`) save the FP/SIMD state as well as the general registers, since
kernel code -- wasm3's float opcodes -- uses FP/SIMD registers, and every
context resumes through `ERET` (`context_switch.S`). Loaders of native code sync
the instruction cache before running it. The native ring-3 syscall ABI is the
shared `wasmos/syscall_native.h` with a per-arch trap stub (`svc #0`).

### Memory Layout

`TCR_EL1` selects 48-bit VAs with the 4 KiB granule in both halves, so the
four translation levels index exactly as x86_64's PML4/PDPT/PD/PT and the slot
geometry is shared (`src/kernel/include/arch/aarch64/vm_layout.h`):

- TTBR0 holds a process's address space: slot 0 the low identity slot, slot 1
  the user slot. A user root is a TTBR0 table only.
- TTBR1 is shared by every address space and holds the kernel: the direct map
  of physical [0, 4 GiB) at `phys | 0xFFFFFF8000000000`, then the framebuffer
  alias, driver register windows, the per-bus ECAM window and the per-app
  linear-memory windows.
- The direct map starts at physical 0 because QEMU virt's RAM does not:
  devices occupy the first GiB and RAM starts at 0x40000000. That first GiB is
  mapped Device-nGnRE, never executable; the other three are Normal
  write-back. RAM is mapped in 4 KiB pages, so a guard-page unmap never splits
  a live block.

Page-table updates follow break-before-make, and TLB maintenance is broadcast
(`TLBI ...IS`); there are no ASIDs yet. Aborts decode into the same
`MM_FAULT_*` memory-service path as x86_64 page faults (`paging.c`).

### Interrupts and Timer

The GICv3 distributor and redistributor (`gic_aarch64.c`) deliver device
interrupts as SPIs and the tick from the EL1 virtual timer (`timer_aarch64.c`).
An `irq_line` in `irq.h` is the GIC INTID on aarch64; routing goes through the
same arch-neutral sharing layer as on x86_64, with a slot table in
`irq_aarch64.c`. The PL011 is the one driver that routes an SPI today.

MSI and MSI-X go through the GICv3 ITS (`its_aarch64.c`, `msi_aarch64.c`):
vector slot `i` is EventID `i` and LPI 8192 + `i`, and the DeviceID is the
requester id of the PCI function the calling driver's spawn profile names
(`pci_function_of_context`). `msi_alloc` therefore keeps its ABI across
architectures; a caller that drives no function gets `msi.NO_FUNCTION`. The
virtio drivers that take interrupts -- block, net, rng, input, gpu -- bind
MSI-X. The kernel-log console (`char/virtio_serial`) takes none: it disables its
queue's interrupts, selects `VIRTIO_MSI_NO_VECTOR` and reclaims transmit
buffers by polling the used ring.

### SMP

With `WASMOS_SMP` the boot CPU starts the others through PSCI `CPU_ON`
(`smp_aarch64.c`), over the same conduit as `SYSTEM_OFF`; the neutral contract
is [28-smp.md](28-smp.md).

- **Discovery.** `smp_init` reads every enabled GICC entry of the ACPI MADT
  into `g_cpus[]`; the boot CPU, matched by MPIDR, keeps slot 0.
- **Entry.** A secondary starts at `_start_secondary` (`entry.S`) at its
  physical address with the MMU off, `x0` naming a boot block the boot CPU
  cleaned to the point of coherency: the kernel's own TTBR0 and TTBR1 (built by
  `paging_init`, not the bootstrap tables), its stack and its `cpu_id`. It takes
  the same EL2-to-EL1 path and MAIR/TCR setup as the boot CPU (shared macros),
  turns the MMU on and calls `smp_ap_c_entry`.
- **Per-CPU interrupt state.** Each CPU finds its own redistributor among the
  contiguous frames at QEMU virt's GICR base by matching MPIDR against
  `GICR_TYPER` (`gic_cpu_init`), records it in `cpu_local()->arch.gicr_base`,
  and enables its own virtual-timer PPI, so every CPU runs its own scheduler
  tick. SPIs are routed to the boot CPU and the ITS collection targets its
  redistributor, as x86_64 keeps device interrupts on the BSP.
- **Order.** Redistributor, scheduler state (`process_ap_init`), idle thread,
  timer, then `started` with release ordering, as on x86_64. The boot CPU starts
  the secondaries one at a time, because they share the boot block, and gives
  each a second before reporting it absent.
- **Stopping.** `arch_stop_other_cpus` raises SGI 1 on every other CPU, which
  hands its frame to `kpanic_nmi`. On a GIC with one security state
  (`GICD_CTLR.DS = 1`, QEMU `virt`) the SGI is Group 0 and arrives as an FIQ.
  The kernel masks only `PSTATE.I`, and every exception handler unmasks F on
  entry, so like x86_64's NMI the stop reaches a CPU spinning with interrupts
  off. Its priority (0x40) is above every IRQ's (0x80), so it also reaches a
  CPU stuck in a handler before the end of interrupt, which the GIC holds at
  that interrupt's priority (`tests/test_kpanic_stop.py`, boot option
  `debug.kpanic_stop_test=1` and `=2`).
  With two security states Group 0 is the secure world's and the stop is an
  ordinary Group 1 SGI, masked with IRQs.
- **TLB.** Every invalidation is already broadcast (`TLBI ...IS`), so mappings
  need no shootdown IPI.

Under `-accel hvf` the CPUs are real Apple cores with the architecture's weak
memory ordering (no TSO), so an ordering bug reproduces locally; under CI's TCG
the vCPUs are host threads (MTTCG), equally unordered.

### PCI

`pci_cfg_aarch64.c` reaches configuration space through ECAM, with the base
from the ACPI MCFG, mapping each bus's 1 MiB on first use. edk2 on aarch64
leaves a function's I/O and memory decode off; pci-bus enables decode for the
BARs a function implements at an assigned address. Device identity, BAR region
grants and the quiesce of a dead driver's function are as on x86_64.

### ACPI and Devices

acpi-bus parses QEMU virt's DSDT, which lists only `ACPI0007`, `ARMH0011`
(PL011), `QEMU0002` (fw_cfg), `LNRO0005` (virtio-mmio), `PNP0C0F`,
`PNP0A08`, `ACPI0013` and `PNP0C0C`. Devices the DSDT omits are published from
a board table (`acpi_board_devices` in `src/services/acpi_bus/`), keyed on QEMU's
OEM ID (`BOCHS `) plus the presence of a GTDT; the PL031 clock is one. Their
addresses and interrupts come from the machine's device tree
(`qemu-system-aarch64 -M virt,dumpdtb=f`).

Driver selection is per architecture: `scripts/initfs.<arch>.toml`,
`scripts/initfs/devmgr/rules/default.<arch>.rules` (bootstrap) and
`scripts/system/devmgr/rules/default.<arch>.rules` (runtime). On aarch64 the
boot disk is virtio-blk, so its driver and rule are in the initfs; the PS/2,
CMOS, 16550 and bochs rules are x86_64's alone.

| Device | Identity | Driver (`src/drivers/`) |
|---|---|---|
| PL011 UART | ACPI `ARMH0011`, 0x09000000, GSIV 33, SPCR console | `tty/pl011` (as `sin`) |
| PL031 RTC | board table, 0x09010000 | `rtc/pl031` (Kconfig default) |
| UEFI clock | runtime services | `rtc/efi` (`WASMOS_RTC_EFI`) |
| fw_cfg | ACPI `QEMU0002` | `gpu/ramfb` (declines without `etc/ramfb`) |
| virtio-gpu | PCI `1AF4:1050` | `gpu/virtio_gpu` + `gpu/framebuffer` |
| virtio-input | PCI `1AF4:1052` at slots 0x1c, 0x1d | `input/virtio_input` |
| virtio-blk | PCI `1AF4:1001` | `block/virtio_blk` (boot disk) |
| virtio-net, -rng, -console | PCI | `net/virtio_net`, `char/virtio_rng`, `char/virtio_serial` |

### Console and Kernel Log

QEMU virt has one UART. The kernel drives the PL011 for its own early output
(`platform_aarch64.c`) until the vt takes it over; user-space console I/O is the
`pl011` driver's. The kernel log goes to the virtio console port
(`char/virtio_serial`), which the harness captures as `syslog.log`; x86_64's
`log.serial=com2` has no aarch64 counterpart.

### Display and Input

The default display is a virtio GPU (`qemu_display` in `ARCHES`). Its GOP is
blit-only, so the loader allocates the scanout, the generic `framebuffer`
driver serves the text console on it, and `virtio_gpu` presents it, flushing
the damage the compositor and the text console report
([20-graphics-framebuffer-and-compositor.md](20-graphics-framebuffer-and-compositor.md),
"Display Presenter"). Runtime mode setting is ramfb's: a session that needs it
replaces the display with `QemuConfig.display_devices = ("-device", "ramfb")`.

Keyboard and mouse are virtio-input devices (`qemu_input` in `ARCHES`). Each
`virtio_input` instance registers `kbd` or `mouse` by what its device reports
and speaks the PS/2 drivers' protocols, translating evdev key codes to PS/2
Set 1 scancodes, so the vt and the compositor are unchanged
([15-drivers-and-services.md](15-drivers-and-services.md)).

### Clock and Power

The real-time clock is a Kconfig choice, "Real-time clock (aarch64)":
`WASMOS_RTC_PL031` (default) or `WASMOS_RTC_EFI`. Both are backends of the
shared rtc core (`src/drivers/rtc/core/`), as the x86_64 CMOS driver is; the
rule for the chosen one is appended by `RTC_RULES` in `scripts/system/BUILD.py`.
`halt` and `reboot` go through PSCI (`SYSTEM_OFF`, `SYSTEM_RESET`), over the
conduit the firmware's entry level selects (`cpu_aarch64.c`).

### Building and Testing

```sh
./configure out/aarch64_wasm3_single configs/aarch64_wasm3_single_defconfig
ninja -C out/aarch64_wasm3_single run-qemu-test
```

Every QEMU battery runs on aarch64 (`aarch64_wasm3_smp` in
`tests/batteries.json`, four CPUs), in CI on an aarch64-linux runner under TCG. A test
whose subject aarch64 lacks -- ATA drives, the PS/2 controller, a second UART
-- is skipped there with `requires_arch` (`scripts/qemu_test_framework.py`),
whose reason names what is missing.

### Deferred Work

- Stopping a CPU that spins with IRQs masked on a GIC with two security
  states: there Group 0 is the secure world's, and the stop would need a GICv3
  pseudo-NMI (priority masking through `ICC_PMR_EL1`).
- WARP: the ring-3 hooks (`src/kernel/arch/aarch64/warp_ring3_aarch64.c`: the
  trampoline stubs and the `ERET` entry), the linear-memory window and its
  fault path, and per-arch AOT artifacts. The JIT itself (WARP's aarch64 code
  generator) and the checkpoints' jump (`arch/warp_jmp.h`, assembly here since
  clang has no `__builtin_setjmp` on aarch64) are in the kernel.
- Containment of every EL0 exception: only an unresolved instruction or data
  abort terminates just the process; an undefined instruction, `BRK` or an
  alignment fault from EL0 panics the kernel (`aarch64_exception`,
  `cpu_aarch64.c`).
- ASIDs, so an address-space switch need not flush the TLB.
- Runtime mode setting through virtio-gpu, and virtio-gpu 3D.
- A framebuffer for firmware without a GOP (x86_64's 0xCF8 scan has no
  counterpart), and console discovery from the SPCR in the kernel.
- CI coverage of the UEFI clock choice, and topology-aware storage tests.
