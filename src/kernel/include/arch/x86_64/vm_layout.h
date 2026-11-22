/* vm_layout.h - x86_64 kernel virtual-address layout: the higher-half alias of
 * low physical memory, the kernel MMIO slot, and the WARP linear-memory window.
 * Included only through arch/vm_layout.h. The values are tied to the 4-level
 * page-table structure paging.c builds (PML4 slot 511, PDPT slots 509/510) and
 * to entry.S and linker.ld, which repeat KERNEL_HIGHER_HALF_BASE. */
#pragma once

/* Base of the kernel's higher-half alias of low physical memory: a physical
 * address `p` inside the shared window is reachable at `p | KERNEL_HIGHER_HALF_BASE`.
 * The window is built by paging_init as 2 MiB large pages in PML4[511]/PDPT[510] and
 * is copied into every child root, so this alias resolves under any CR3.  It covers
 * only the first 512 MiB of physical memory; a physical address beyond that has no
 * alias and must not be OR-ed with this base. */
#define KERNEL_HIGHER_HALF_BASE 0xFFFFFFFF80000000ULL
/* Size of that window. A frame handed to anything that will dereference it at
 * `phys | KERNEL_HIGHER_HALF_BASE` must come from below this, which is what
 * pfa_alloc_pages_below is for; pfa_alloc_pages alone can return a frame with no
 * alias. Stated here rather than in each consumer because it is a property of
 * the mapping paging_init builds, and a private copy drifts from it silently. */
#define KERNEL_HIGHER_HALF_WINDOW_BYTES (512ULL * 1024ULL * 1024ULL)
/* Size of the higher-half alias entry.S builds before paging_init (32 2 MiB
 * pages in bootstrap_pd_high). Anything that dereferences a frame through the
 * alias before paging_init -- the frame allocator's own tables in pfa_init --
 * must allocate below this. */
#define KERNEL_BOOTSTRAP_WINDOW_BYTES (64ULL * 1024ULL * 1024ULL)
/* PDPT slot inside the shared higher-half (PML4[511]) reserved for kernel MMIO
 * mappings.  paging_verify_user_root_impl permits this slot to be present in a user
 * root but does not descend into it, because its PD contents are device-specific. */
#define KERNEL_MMIO_PDPT_INDEX 509u
/* Kernel VA where framebuffer_map_higher_half maps the UEFI GOP aperture, inside the
 * KERNEL_MMIO_PDPT_INDEX slot.  Supervisor-only; the in-page offset of the GOP base is
 * re-added on top of this VA. */
#define KERNEL_MMIO_FB_VA 0xFFFFFFFF40000000ULL
/* Kernel VA span, in the upper half of the KERNEL_MMIO_PDPT_INDEX slot, where
 * device_window_map maps the memory-mapped register windows drivers are
 * granted. Supervisor-only and uncached; the lower half stays the
 * framebuffer's. */
#define KERNEL_MMIO_DEVICE_VA 0xFFFFFFFF60000000ULL
#define KERNEL_MMIO_DEVICE_BYTES (512ULL * 1024ULL * 1024ULL)

/* Dedicated per-app WARP linear-memory kernel-VA window, carved from free PDPT
 * slots in the shared higher-half (PML4[511]).  2 GiB (2 PDPT slots) per app,
 * up to 48 apps -> slots [64, 160), all free (only 509=MMIO, 510=direct-map are
 * used).  The higher-half PDPT is a single shared page, so entries installed
 * here appear in every CR3 automatically (the kernel-side linmem alias).
 * Populated on demand by the WARP linmem allocator, so the verifier permits
 * these slots present-or-absent.  Isolation: this alias is mapped SUPERVISOR
 * ONLY (never MEM_REGION_FLAG_USER - enforced in paging_map_4k_in_root's
 * higher-half no-USER check), so ring-3 code cannot reach it; ring-3 wasm uses
 * the separate per-app user mapping at WARP_R3_LINMEM_BASE (PML4 slot 1). */
#define WARP_LINMEM_PDPT_INDEX 64u
#define WARP_LINMEM_PDPT_COUNT 96u
#define WARP_LINMEM_VA_BASE 0xFFFFFF9000000000ULL /* 0xFFFFFF8000000000 + 64*1GiB */
#define WARP_LINMEM_VA_STRIDE (2ULL * 1024ULL * 1024ULL * 1024ULL) /* 2 GiB / app */
