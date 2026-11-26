/* efi_loader_alloc.h - the two buffers the ExitBootServices loop sizes and
 * re-sizes: the pool buffer GetMemoryMap fills and the page run that holds
 * boot_info, the map copy, initfs and the module table.
 *
 * Every byte allocated as EFI_LOADER_DATA and still allocated at
 * ExitBootServices stays reserved for the kernel's lifetime, so growing either
 * buffer releases the one it replaces. FreePool and FreePages are Memory
 * Allocation Services, which the UEFI spec's ExitBootServices() description
 * still allows after a failed first call. Header-only and reached through
 * EFI_BOOT_SERVICES alone, so a host test can stand in for the firmware
 * (tests/unit/test_efi_loader_alloc.c). */
#ifndef WASMOS_BOOT_EFI_LOADER_ALLOC_H
#define WASMOS_BOOT_EFI_LOADER_ALLOC_H

#include "uefi.h"

/* Reads the current memory map into *map, a pool buffer of *capacity bytes.
 * *map may be 0, and is then allocated. While the firmware reports
 * EFI_BUFFER_TOO_SMALL the buffer is freed and replaced by one sized for the
 * reported map plus two descriptors, since the replacement allocation itself
 * can add one. On EFI_SUCCESS *size, *map_key, *desc_size and *desc_version
 * describe the map in *map; on any other status that status is returned and
 * *map is either 0 or a live buffer the caller still owns. */
static inline EFI_STATUS efi_read_memory_map(EFI_BOOT_SERVICES* bs, void** map, UINTN* capacity,
                                             UINTN* size, UINTN* map_key, UINTN* desc_size,
                                             UINT32* desc_version) {
    for (;;) {
        if (!*map) {
            EFI_STATUS status = bs->AllocatePool(EFI_LOADER_DATA, *capacity, map);
            if (EFI_ERROR(status)) {
                *map = 0;
                return status;
            }
        }
        *size = *capacity;
        EFI_STATUS status = bs->GetMemoryMap(size, *map, map_key, desc_size, desc_version);
        if (status != EFI_BUFFER_TOO_SMALL) {
            return status;
        }
        bs->FreePool(*map);
        *map = 0;
        *capacity = *size + *desc_size * 2;
    }
}

/* Ensures the page run at *base holds at least `pages` pages. A run of
 * *capacity pages that is too small is freed before its replacement is
 * allocated; on failure *base is 0, *capacity is 0 and the status is returned.
 * The caller rebuilds the run's contents after every call. */
static inline EFI_STATUS efi_reserve_loader_pages(EFI_BOOT_SERVICES* bs, UINT64* base,
                                                  UINTN* capacity, UINTN pages) {
    if (*base && *capacity >= pages) {
        return EFI_SUCCESS;
    }
    if (*base) {
        bs->FreePages(*base, *capacity);
        *base = 0;
        *capacity = 0;
    }
    EFI_STATUS status = bs->AllocatePages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, pages, base);
    if (EFI_ERROR(status)) {
        *base = 0;
        return status;
    }
    *capacity = pages;
    return EFI_SUCCESS;
}

#endif
