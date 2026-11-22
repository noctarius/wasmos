/* its_aarch64.h - the GICv3 Interrupt Translation Service of QEMU virt, which
 * turns a device's message write (DeviceID, EventID) into an LPI. Private to
 * arch/aarch64; the caller serialises every call. */
#pragma once

#include <stdint.h>

/* Program the ITS tables and command queue, map collection 0 to the boot CPU's
 * redistributor, and enable LPIs there. Returns 0, or -1 when the machine has
 * no usable ITS or a table could not be allocated; MSIs are then unsupported.
 * Needs the frame allocator. */
int its_init(void);

/* Route (DeviceID `bdf`, EventID `event`) to LPI `lpi` on collection 0 and
 * enable the LPI. Maps the device on its first event. Returns 0, or -1 when
 * the device table is full, an ITT could not be allocated, or the ITS did not
 * consume its commands. */
int its_map_event(uint16_t bdf, uint32_t event, uint32_t lpi);

/* Disable LPI `lpi` and drop the (bdf, event) translation; a message the
 * device still sends afterwards is discarded by the ITS. */
void its_unmap_event(uint16_t bdf, uint32_t event, uint32_t lpi);

/* Physical address of GITS_TRANSLATER, the doorbell a device writes its
 * EventID to. */
uint64_t its_translater_phys(void);
