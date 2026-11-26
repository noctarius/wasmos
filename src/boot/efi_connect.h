/* efi_connect.h - the loader's ConnectController passes, run before the GOP
 * lookup so that drivers producing a graphics output bind to their controllers
 * even on firmware whose boot manager has not connected them.
 *
 * Each pass asks LocateHandleBuffer for every handle carrying one protocol and
 * connects each handle recursively. A pass is best-effort: a lookup that finds
 * nothing or a connect that fails is not an error, because GOP discovery has
 * fallbacks of its own. Header-only and reached through EFI_BOOT_SERVICES alone,
 * so a host test can stand in for the firmware (tests/unit/test_efi_connect.c). */
#ifndef WASMOS_BOOT_EFI_CONNECT_H
#define WASMOS_BOOT_EFI_CONNECT_H

#include "uefi.h"

/* Connects every handle that carries `protocol`, recursively, and frees the
 * handle buffer LocateHandleBuffer transferred to the loader. */
static inline void efi_connect_handles_for_protocol(EFI_BOOT_SERVICES* bs,
                                                    const EFI_GUID* protocol) {
    if (!bs || !protocol || !bs->ConnectController || !bs->LocateHandleBuffer) {
        return;
    }
    EFI_CONNECT_CONTROLLER connect_controller = (EFI_CONNECT_CONTROLLER)bs->ConnectController;
    EFI_LOCATE_HANDLE_BUFFER locate_handle_buffer =
        (EFI_LOCATE_HANDLE_BUFFER)bs->LocateHandleBuffer;
    EFI_HANDLE* handles = 0;
    UINTN handle_count = 0;
    EFI_STATUS status = locate_handle_buffer(
        EFI_LOCATE_SEARCH_TYPE_BY_PROTOCOL, protocol, 0, &handle_count, &handles);
    if (EFI_ERROR(status) || !handles) {
        return;
    }
    for (UINTN i = 0; i < handle_count; ++i) {
        if (handles[i]) {
            connect_controller(handles[i], 0, 0, TRUE);
        }
    }
    if (bs->FreePool) {
        bs->FreePool(handles);
    }
}

/* The three passes: text-output handles, every PCI device (EFI_PCI_IO_PROTOCOL),
 * and every PCI host bridge (EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL). */
static inline void efi_connect_graphics_controllers(EFI_BOOT_SERVICES* bs) {
    static const EFI_GUID text_output_guid = EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
    static const EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
    static const EFI_GUID root_bridge_guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
    efi_connect_handles_for_protocol(bs, &text_output_guid);
    efi_connect_handles_for_protocol(bs, &pci_io_guid);
    efi_connect_handles_for_protocol(bs, &root_bridge_guid);
}

#endif
