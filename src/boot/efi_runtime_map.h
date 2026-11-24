/* efi_runtime_map.h - the virtual map the loader hands SetVirtualAddressMap
 * for the firmware clock (Kconfig WASMOS_RTC_EFI, aarch64).
 *
 * Every EFI_MEMORY_RUNTIME region is named at the address the kernel's direct
 * map already gives it, phys | KERNEL_HIGHER_HALF_BASE, so the firmware's
 * runtime code runs where the kernel calls it and needs no mapping of its own.
 * The direct map covers physical [0, KERNEL_HIGHER_HALF_WINDOW_BYTES) (the
 * aarch64 vm_layout.h). Header-only and free of firmware calls, so the decision
 * is reachable from a host test (tests/unit/test_efi_runtime_map.c). */
#ifndef WASMOS_BOOT_EFI_RUNTIME_MAP_H
#define WASMOS_BOOT_EFI_RUNTIME_MAP_H

#include "uefi.h"

#include "arch/aarch64/vm_layout.h"

/* Whether the runtime region `d` can be reached through the direct map: its
 * end inside the window, and outside the Device-nGnRE head unless it is device
 * registers. Firmware code there would take an instruction abort on its first
 * call, and firmware data an alignment fault. */
static inline int efi_runtime_region_reachable(const EFI_MEMORY_DESCRIPTOR* d) {
    UINT64 end = d->PhysicalStart + d->NumberOfPages * 0x1000ULL;
    if (end > KERNEL_HIGHER_HALF_WINDOW_BYTES || end < d->PhysicalStart) {
        return 0;
    }
    if (d->Type == EFI_MEMORY_MAPPED_IO || d->Type == EFI_MEMORY_MAPPED_IO_PORT_SPACE) {
        return 1;
    }
    return d->PhysicalStart >= KERNEL_DIRECT_MAP_DEVICE_BYTES;
}

/* Gives every runtime region of `map` (desc_size apart, map_bytes long) its
 * direct-map VirtualStart. Returns 1 when every region is reachable, or 0 with
 * the map unchanged when one is not: SetVirtualAddressMap must then not be
 * called. */
static inline int efi_runtime_map_assign(UINT8* map, UINTN map_bytes, UINTN desc_size) {
    for (UINTN off = 0; off + desc_size <= map_bytes; off += desc_size) {
        const EFI_MEMORY_DESCRIPTOR* d = (const EFI_MEMORY_DESCRIPTOR*)(map + off);
        if ((d->Attribute & EFI_MEMORY_RUNTIME) != 0 && !efi_runtime_region_reachable(d)) {
            return 0;
        }
    }
    for (UINTN off = 0; off + desc_size <= map_bytes; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)(map + off);
        if ((d->Attribute & EFI_MEMORY_RUNTIME) != 0) {
            d->VirtualStart = d->PhysicalStart | KERNEL_HIGHER_HALF_BASE;
        }
    }
    return 1;
}

#endif
