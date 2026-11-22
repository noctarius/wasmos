/* io_region.h - region-addressed device register access for driver host calls
 * (hostcalls.yaml io_region_in8/16/32, io_region_out8/16/32).
 *
 * A driver names a register as (region, offset), never as an absolute port or
 * physical address; capability_region_resolve turns that into a port or a
 * kernel VA inside a window the driver's spawn profile granted. Both runtimes'
 * shims call these, so the access itself exists once. */
#ifndef WASMOS_IO_REGION_H
#define WASMOS_IO_REGION_H

#include <stdint.h>

/* Read `width` bytes (1, 2 or 4) at `offset` within region `region` of
 * context_id's spawn profile into *out_value, zero-extended. An I/O window is
 * read with one IN instruction, a memory-mapped one with one load of that
 * width. Returns 0, or the negative WASMOS_ERR_IO_* code
 * capability_region_resolve produced; *out_value is untouched on failure. */
int io_region_read(uint32_t context_id, uint32_t region, uint32_t offset, uint32_t width,
                   uint32_t* out_value);

/* Write the low `width` bytes (1, 2 or 4) of `value` at `offset` within region
 * `region`; excess high bits are masked off, not refused. Returns 0, or the
 * negative WASMOS_ERR_IO_* code capability_region_resolve produced. */
int io_region_write(uint32_t context_id, uint32_t region, uint32_t offset, uint32_t width,
                    uint32_t value);

#endif
