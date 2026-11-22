/* device_manager_unit.h - what the device manager hands the supervisor for a
 * driver whose rule names a restart policy (ENV{RESTART}): the unit spec and
 * the capability descriptor of one SUPERVISOR_IPC_SUBMIT_REQ. */
#ifndef WASMOS_DEVICE_MANAGER_UNIT_H
#define WASMOS_DEVICE_MANAGER_UNIT_H

#include <stdint.h>
#include "device_manager_types.h"

/* Writes the spec of the unit that drives one device:
 *   <template>@<instance> <path> restart=<restart> [-- <args>]
 * where <template> is rule_path's file name without ".wap" and <path> the
 * absolute path the driver is spawned from. The instance key names the device
 * by what it is, not by when it was found -- "acpi:0070" (its I/O base),
 * "pci:00:03.0" (its function) -- so a restarted driver keeps its unit's name.
 * args may be NULL or empty. Returns the spec's length, or -1 when an argument
 * is missing or the spec, or its unit name, does not fit (out_cap bytes with
 * the NUL; SUPERVISOR_UNIT_NAME_MAX). */
int32_t dm_unit_spec(const char* rule_path, const char* instance, const char* path,
                     const char* restart, const char* args, char* out, uint32_t out_cap);

/* Encodes caps as a wasmos_spawn_caps_v2_t followed by its region grants, and by
 * the default DMA window when caps asks for DMA (the grant the packed spawn
 * opcodes synthesise), with caps->dma_max_bytes as its budget (one page when
 * that is 0). A caps without grants carries its single window in
 * io_port_min/max. A caps naming its PCI function carries it as
 * DEVMGR_CAP_PCI_FUNCTION and dma.pci_function. Returns the descriptor's
 * length, or -1 when it would not fit out_cap bytes. */
int32_t dm_caps_v2_encode(const spawn_caps_t* caps, uint8_t* out, uint32_t out_cap);

/* Bytes a watched-spawn request may take: the descriptor, the longest path and
 * argument string, and the largest capability descriptor. */
#define DM_WATCH_SPAWN_MAX                                                                         \
    ((uint32_t)sizeof(wasmos_spawn_watch_desc_t) + SPAWN_WATCH_PATH_MAX + SPAWN_WATCH_ARGS_MAX +   \
     SUPERVISOR_CAPS_MAX)

/* Writes one PROC_IPC_SPAWN_WATCHED request into out: a
 * wasmos_spawn_watch_desc_t naming notify_endpoint, followed by the path, the
 * startup args and caps encoded as dm_caps_v2_encode does, at the offsets the
 * descriptor records. This is the one spawn form that carries a driver's
 * startup args AND all of its register windows, so a driver needing both is
 * spawned by path rather than by boot-module index. args may be NULL or empty;
 * caps may be NULL for a spawn without hardware access. Returns the request's
 * length, or -1 for a missing or over-long path or args, or a request that does
 * not fit out_cap bytes. */
int32_t dm_watch_spawn_encode(const char* path, const char* args, const spawn_caps_t* caps,
                              int32_t notify_endpoint, uint8_t* out, uint32_t out_cap);

/* Turns the register windows a driver declared (meta->regions) into the grants
 * it actually gets (caps->regions). A static range is taken verbatim; a BAR
 * region is resolved against the device this driver matched, using the base,
 * length and kind pci-bus measured -- which is the whole point of the exercise,
 * since a firmware-assigned window cannot be written down in a manifest.
 *
 * Grant i is declaration i, because the declaration index IS the region index
 * the driver addresses at runtime. A region that cannot be granted (a BAR the
 * device does not implement or that has no measured size, a memory BAR that is
 * not page-aligned) therefore fails the whole resolution: skipping it would
 * renumber every region after it. Returns 0 with caps->region_count set, or -1
 * with caps->region_count 0. A driver declaring no regions resolves to 0. */
int dm_resolve_declared_regions(const wasmos_module_meta_desc_t* meta,
                                const pci_device_record_t* rec, spawn_caps_t* caps);

#endif
