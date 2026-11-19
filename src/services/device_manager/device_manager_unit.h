/* device_manager_unit.h - what the device manager hands the supervisor for a
 * driver whose rule names a restart policy (ENV{RESTART}): the unit spec and
 * the capability descriptor of one SUPERVISOR_IPC_SUBMIT_REQ. */
#ifndef WASMOS_DEVICE_MANAGER_UNIT_H
#define WASMOS_DEVICE_MANAGER_UNIT_H

#include <stdint.h>
#include "device_manager_types.h"

/* Writes the spec of the unit that drives the ACPI device at io_base:
 *   <template>@acpi:<io> /boot/<rule_path> restart=<restart> [-- <args>]
 * where <template> is rule_path's file name without ".wap". The instance key is
 * the device's I/O base, so a restarted driver keeps its unit's name. args may
 * be NULL or empty. Returns the spec's length, or -1 when an argument is
 * missing or the spec, or its unit name, does not fit (out_cap bytes with the
 * NUL; SUPERVISOR_UNIT_NAME_MAX). */
int32_t dm_unit_spec(const char* rule_path, uint16_t io_base, const char* restart, const char* args,
                     char* out, uint32_t out_cap);

/* Encodes caps as a wasmos_spawn_caps_v2_t followed by its I/O ranges, and by
 * the default DMA window when caps asks for DMA (the grant the packed spawn
 * opcodes synthesise). A caps without ranges carries its single window in
 * io_port_min/max. Returns the descriptor's length, or -1 when it would not fit
 * out_cap bytes. */
int32_t dm_caps_v2_encode(const spawn_caps_t* caps, uint8_t* out, uint32_t out_cap);

#endif
