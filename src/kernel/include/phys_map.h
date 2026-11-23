#ifndef WASMOS_KERNEL_PHYS_MAP_H
#define WASMOS_KERNEL_PHYS_MAP_H

#include <stdint.h>

/*
 * phys_map.h — the windows wasmos_phys_map has mapped into guests.
 *
 * wasmos_phys_map maps firmware ACPI table memory into the caller's linear
 * memory (abi/hostcalls.yaml `phys_map`). Both runtimes place the window with
 * their own overlay mapper; this table is the runtime-neutral record of what
 * each process holds, so a repeated request is answered from an existing
 * window and process teardown can find every window it must unmap. A window
 * lives until its process is reaped; nothing unmaps one earlier.
 *
 * TODO(phys-map-readonly): windows are mapped read-write, so an mmio.map holder
 * can write firmware ACPI memory. WARP's ring-3 republish maps every committed
 * linear-memory page read-write and would have to keep a read-only window so
 * first (docs/TASKS.md).
 */

/* The largest window one call maps, in pages. */
#define PHYS_MAP_MAX_PAGES 1024u

typedef struct {
    uint32_t pid;
    uint32_t offset; /* linear-memory offset of the window's first page */
    uint64_t phys;   /* page-aligned physical base */
    uint32_t pages;
    /* Kernel VA of the window's first page in the runtime's linear-memory
     * slot, or 0 when the runtime maps the window only into the process's own
     * address space (wasm3). Teardown unmaps a nonzero one before the slot is
     * decommitted, which would otherwise free the firmware frames. */
    uint64_t slot_va;
} phys_map_window_t;

/* The whole pages [*out_page, *out_page + *out_pages * 4096) that cover
 * [phys, phys+size). Returns 0, or -1 for a zero phys or size, a wrapping
 * range, or one spanning more than PHYS_MAP_MAX_PAGES pages. */
int phys_map_span(uint64_t phys, uint64_t size, uint64_t* out_page, uint32_t* out_pages);

/* When `pid` holds a window covering every byte of [phys, phys+size), stores the
 * linear-memory offset of the byte at `phys` and returns 1; else returns 0. */
int phys_map_window_find(uint32_t pid, uint64_t phys, uint64_t size, uint32_t* out_off);

/* Records a window. Returns 0, or -1 when the table is full. */
int phys_map_window_record(const phys_map_window_t* window);

/* Removes every window `pid` holds, passing each to `unmap` (may be NULL)
 * after it has left the table. */
void phys_map_window_release_pid(uint32_t pid, void (*unmap)(const phys_map_window_t* window));

#endif
