/* vm_layout.h - aarch64 kernel virtual-address layout. Included only through
 * arch/vm_layout.h.
 *
 * TCR_EL1 selects 48-bit VAs with the 4 KiB granule in both halves
 * (T0SZ = T1SZ = 16), so translation has four levels and the level indices are
 * the x86_64 PML4/PDPT/PD/PT indices: TTBR0 slot 0 is the low identity slot,
 * slot 1 the user slot (USER_VA_MIN), and everything below lives in TTBR1
 * slot 511, whose level-1 table maps 1 GiB per entry. The TTBR1 tables are
 * shared by every address space; a user root is a TTBR0 table only, so
 * nothing here is copied into a child root. The values are tied to entry.S
 * (which builds the direct map) and linker.ld (which repeats
 * KERNEL_HIGHER_HALF_BASE). */
#pragma once

/* Base of the kernel's direct map of physical memory: a physical address `p`
 * inside the window is reachable at `p | KERNEL_HIGHER_HALF_BASE`. The window
 * starts at physical 0 because QEMU virt's RAM does not: devices occupy the
 * first GiB and RAM begins at 0x40000000, so a window that starts at 0 keeps
 * the neutral `phys | base` arithmetic valid for every RAM frame. */
#define KERNEL_HIGHER_HALF_BASE 0xFFFFFF8000000000ULL
/* Size of the direct map: physical [0, 4 GiB) as four level-1 blocks, the first
 * Device-nGnRE, the other three Normal write-back. RAM above 4 GiB (a machine
 * started with more than 3 GiB) has no alias. */
#define KERNEL_HIGHER_HALF_WINDOW_BYTES (4ULL * 1024ULL * 1024ULL * 1024ULL)
/* entry.S builds the whole direct map before kmain, so the window that is
 * valid before paging_init is the full one. */
#define KERNEL_BOOTSTRAP_WINDOW_BYTES KERNEL_HIGHER_HALF_WINDOW_BYTES

/* Kernel VA where framebuffer_map_higher_half maps the GOP aperture (base +
 * 256 GiB), and the span above it where device_window_map maps driver register
 * windows. Supervisor-only; the device span is mapped Device-nGnRE. */
#define KERNEL_MMIO_FB_VA 0xFFFFFFC000000000ULL
#define KERNEL_MMIO_DEVICE_VA 0xFFFFFFC020000000ULL
#define KERNEL_MMIO_DEVICE_BYTES (512ULL * 1024ULL * 1024ULL)

/* Per-app linear-memory kernel-VA window (base + 128 GiB): 48 slots of 2 GiB.
 * WARP_LINMEM_PDPT_COUNT counts the window in 1 GiB units (level-1 entries of
 * TTBR1 slot 511, as PDPT entries on x86_64). Supervisor-only. */
#define WARP_LINMEM_PDPT_COUNT 96u
#define WARP_LINMEM_VA_BASE 0xFFFFFFA000000000ULL
#define WARP_LINMEM_VA_STRIDE (2ULL * 1024ULL * 1024ULL * 1024ULL)
