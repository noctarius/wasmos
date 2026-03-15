#include "uefi.h"
#include "elf.h"
#include "boot.h"

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask = 2,
    PixelBltOnly = 3
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    UINT32 PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    UINT64 FrameBufferBase;
    UINT64 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
    void *QueryMode;
    void *SetMode;
    void *Blt;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_GOP_SET_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber
);

typedef EFI_STATUS (EFIAPI *EFI_GOP_QUERY_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber,
    UINTN *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
);

/*
 * The UEFI loader keeps policy intentionally narrow. Its only job is to build a
 * trustworthy handoff for the kernel: load ELF segments, snapshot the UEFI
 * memory map, preload the minimal disk bootstrap modules, and transfer control
 * through the versioned boot_info_t contract.
 */

#define EFI_ALLOCATE_ANY_PAGES 0
#define EFI_ALLOCATE_ADDRESS 2
#define EFI_LOADER_DATA 2
#define EFI_BUFFER_TOO_SMALL ((EFI_STATUS)0x8000000000000005ULL)
#define EFI_INVALID_PARAMETER ((EFI_STATUS)0x8000000000000002ULL)

typedef struct __attribute__((packed)) {
    char signature[8];
    UINT8 checksum;
    char oem_id[6];
    UINT8 revision;
    UINT32 rsdt_address;
    UINT32 length;
    UINT64 xsdt_address;
    UINT8 ext_checksum;
    UINT8 reserved[3];
} acpi_rsdp_t;

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    if (!a || !b) {
        return 0;
    }
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) {
        return 0;
    }
    for (UINTN i = 0; i < sizeof(a->Data4); ++i) {
        if (a->Data4[i] != b->Data4[i]) {
            return 0;
        }
    }
    return 1;
}

static void *find_acpi_rsdp(EFI_SYSTEM_TABLE *system, UINT32 *out_len) {
    static const EFI_GUID acpi20_guid =
        { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
    static const EFI_GUID acpi10_guid =
        { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };

    if (out_len) {
        *out_len = 0;
    }
    if (!system || !system->ConfigurationTable || system->NumberOfTableEntries == 0) {
        return 0;
    }

    /*
     * UEFI may expose both ACPI 1.0 and ACPI 2.0 entries. Prefer the ACPI 2.0
     * table when available but keep the ACPI 1.0 pointer as a fallback so early
     * hardware discovery still has a stable root pointer on older firmware.
     */
    EFI_CONFIGURATION_TABLE *tables = (EFI_CONFIGURATION_TABLE *)system->ConfigurationTable;
    void *rsdp = 0;
    for (UINTN i = 0; i < system->NumberOfTableEntries; ++i) {
        if (guid_eq(&tables[i].VendorGuid, &acpi20_guid)) {
            rsdp = tables[i].VendorTable;
            break;
        }
        if (!rsdp && guid_eq(&tables[i].VendorGuid, &acpi10_guid)) {
            rsdp = tables[i].VendorTable;
        }
    }

    if (!rsdp) {
        return 0;
    }
    if (out_len) {
        acpi_rsdp_t *desc = (acpi_rsdp_t *)rsdp;
        UINT32 len = 20;
        if (desc->revision >= 2 && desc->length >= 20) {
            len = desc->length;
        }
        *out_len = len;
    }
    return rsdp;
}

static void *memcpy8(void *dst, const void *src, UINTN n) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (UINTN i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

static void *memset8(void *dst, UINT8 val, UINTN n) {
    UINT8 *d = (UINT8 *)dst;
    for (UINTN i = 0; i < n; ++i) {
        d[i] = val;
    }
    return dst;
}

static void copy_cstr(char *dst, UINTN dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    UINTN i = 0;
    for (; src && src[i] != '\0' && i + 1 < dst_size; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void connect_handles_for_protocol(EFI_SYSTEM_TABLE *system, EFI_CONNECT_CONTROLLER connect_controller,
                                         EFI_LOCATE_HANDLE_BUFFER locate_handle_buffer, const EFI_GUID *protocol) {
    if (!system || !connect_controller || !locate_handle_buffer || !protocol) {
        return;
    }
    EFI_HANDLE *handles = 0;
    UINTN handle_count = 0;
    EFI_STATUS status = locate_handle_buffer(
        EFI_LOCATE_SEARCH_TYPE_BY_PROTOCOL,
        protocol,
        0,
        &handle_count,
        &handles
    );
    if (EFI_ERROR(status) || !handles || handle_count == 0) {
        return;
    }
    for (UINTN i = 0; i < handle_count; ++i) {
        if (!handles[i]) {
            continue;
        }
        connect_controller(handles[i], 0, 0, TRUE);
    }
    if (system->BootServices && system->BootServices->FreePool) {
        system->BootServices->FreePool(handles);
    }
}

static void connect_graphics_controllers(EFI_SYSTEM_TABLE *system) {
    static int g_graphics_connected = 0;
    if (g_graphics_connected || !system || !system->BootServices) {
        return;
    }
    EFI_CONNECT_CONTROLLER connect_controller =
        (EFI_CONNECT_CONTROLLER)system->BootServices->ConnectController;
    EFI_LOCATE_HANDLE_BUFFER locate_handle_buffer =
        (EFI_LOCATE_HANDLE_BUFFER)system->BootServices->LocateHandleBuffer;
    if (!connect_controller || !locate_handle_buffer) {
        return;
    }

    if (system->ConsoleOutHandle) {
        connect_controller(system->ConsoleOutHandle, 0, 0, TRUE);
    }
    if (system->StandardErrorHandle && system->StandardErrorHandle != system->ConsoleOutHandle) {
        connect_controller(system->StandardErrorHandle, 0, 0, TRUE);
    }

    static const EFI_GUID text_output_guid =
        { 0x387477c2, 0x69c7, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
    static const EFI_GUID pci_io_guid =
        { 0x2f707eb9, 0x3a1a, 0x11d4, { 0x9a, 0x46, 0x00, 0x90, 0x27, 0x3f, 0xcc, 0x69 } };
    static const EFI_GUID root_bridge_guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;

    connect_handles_for_protocol(system, connect_controller, locate_handle_buffer, &text_output_guid);
    connect_handles_for_protocol(system, connect_controller, locate_handle_buffer, &pci_io_guid);
    connect_handles_for_protocol(system, connect_controller, locate_handle_buffer, &root_bridge_guid);
    g_graphics_connected = 1;
}

static void uefi_log(EFI_SYSTEM_TABLE *system, const char *msg);
static void uefi_log_status(EFI_SYSTEM_TABLE *system, const char *msg, EFI_STATUS status);
static void uefi_log_hex_prefixed(EFI_SYSTEM_TABLE *system, const char *prefix, uint64_t value);

typedef struct {
    uint64_t base;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t flags;
} framebuffer_snapshot_t;

static inline void pci_out32(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %%eax, %%dx" : : "a"(value), "d"(port));
}

static inline uint32_t pci_in32(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %%dx, %%eax" : "=a"(value) : "d"(port));
    return value;
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t reg)
{
    uint32_t address = 0x80000000u;
    address |= ((uint32_t)bus << 16);
    address |= ((uint32_t)device << 11);
    address |= ((uint32_t)function << 8);
    address |= reg & 0xFC;
    pci_out32(0xCF8, address);
    return pci_in32(0xCFC);
}

static void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t reg, uint32_t value)
{
    uint32_t address = 0x80000000u;
    address |= ((uint32_t)bus << 16);
    address |= ((uint32_t)device << 11);
    address |= ((uint32_t)function << 8);
    address |= reg & 0xFC;
    pci_out32(0xCF8, address);
    pci_out32(0xCFC, value);
}

static int pci_probe_mem_bar(uint8_t bus,
                            uint8_t device,
                            uint8_t function,
                            uint8_t bar_index,
                            uint64_t *out_base,
                            uint64_t *out_size,
                            int *consumed_next)
{
    if (!out_base || !out_size || !consumed_next) {
        return -1;
    }
    *consumed_next = 0;
    uint8_t reg = 0x10 + (bar_index << 2);
    uint32_t bar_value = pci_config_read32(bus, device, function, reg);
    if (bar_value == 0) {
        return -1;
    }
    if (bar_value & 0x1) {
        return -1;
    }
    uint32_t type = (bar_value >> 1) & 0x3;
    if (type == 0x1) {
        return -1;
    }
    uint64_t base = bar_value & 0xFFFFFFF0u;
    if (type == 0x0) {
        pci_config_write32(bus, device, function, reg, 0xFFFFFFFFu);
        uint32_t mask = pci_config_read32(bus, device, function, reg);
        pci_config_write32(bus, device, function, reg, bar_value);
        uint32_t mask_clean = mask & 0xFFFFFFF0u;
        if (mask_clean == 0) {
            return -1;
        }
        *out_base = base;
        *out_size = ((uint64_t)(~mask_clean & 0xFFFFFFFFu)) + 1;
        return 0;
    }
    if (type == 0x2) {
        uint32_t bar_high = pci_config_read32(bus, device, function, reg + 4);
        uint64_t base64 = base | ((uint64_t)bar_high << 32);
        pci_config_write32(bus, device, function, reg, 0xFFFFFFFFu);
        pci_config_write32(bus, device, function, reg + 4, 0xFFFFFFFFu);
        uint32_t mask_low = pci_config_read32(bus, device, function, reg);
        uint32_t mask_high = pci_config_read32(bus, device, function, reg + 4);
        pci_config_write32(bus, device, function, reg, bar_value);
        pci_config_write32(bus, device, function, reg + 4, bar_high);
        uint64_t mask64 = (((uint64_t)mask_high) << 32) | (uint64_t)(mask_low & 0xFFFFFFF0u);
        mask64 &= ~0xFULL;
        if (mask64 == 0) {
            return -1;
        }
        *consumed_next = 1;
        *out_base = base64;
        *out_size = (~mask64) + 1;
        return 0;
    }
    return -1;
}

static int capture_framebuffer_from_pci_config(EFI_SYSTEM_TABLE *system,
                                               framebuffer_snapshot_t *snapshot)
{
    if (!system || !system->BootServices || !snapshot) {
        return -1;
    }
    uefi_log(system, "[boot] PCI direct memory scan fallback start\n");
    uint64_t best_base = 0;
    uint64_t best_size = 0;
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            for (uint8_t function = 0; function < 8; ++function) {
                uint32_t vendor_id = pci_config_read32(bus, device, function, 0);
                if ((vendor_id & 0xFFFF) == 0xFFFF) {
                    continue;
                }
                uint32_t class_code = pci_config_read32(bus, device, function, 8);
                uint8_t base_class = (class_code >> 24) & 0xFF;
                if (base_class != 0x03) {
                    continue;
                }
                int bar_index = 0;
                while (bar_index < 6) {
                    uint64_t candidate_base = 0;
                    uint64_t candidate_size = 0;
                    int consumed = 0;
                    if (pci_probe_mem_bar(bus, device, function, (uint8_t)bar_index,
                                          &candidate_base, &candidate_size, &consumed) == 0) {
                        if (candidate_size > best_size && candidate_base != 0) {
                            best_size = candidate_size;
                            best_base = candidate_base;
                            uefi_log(system, "[boot] PCI VGA candidate BAR via config\n");
                            uefi_log_hex_prefixed(system, "  base=", best_base);
                            uefi_log_hex_prefixed(system, "  size=", best_size);
                        }
                    }
                    bar_index += 1 + consumed;
                }
            }
        }
    }

    if (best_size == 0 || best_base == 0) {
        uefi_log(system, "[boot] PCI config scan found no VGA BAR\n");
        return -1;
    }

    snapshot->base = best_base;
    snapshot->size = best_size;
    snapshot->width = 1024;
    snapshot->height = 768;
    snapshot->stride = 1024;
    snapshot->flags = BOOT_INFO_FLAG_GOP_PRESENT;
    uefi_log(system, "[boot] VGA framebuffer mapped via PCI config scan\n");
    return 0;
}


static int capture_framebuffer_snapshot(EFI_SYSTEM_TABLE *system,
                                       framebuffer_snapshot_t *snapshot)
{
    if (!system || !system->BootServices || !snapshot) {
        return -1;
    }

    connect_graphics_controllers(system);
    static int gop_locate_failed = 0;
    static int gop_handle_locate_failed = 0;
    static const EFI_GUID gop_guid =
        { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xd4, 0x76, 0x2b, 0x04, 0x6f } };
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_STATUS status = EFI_NOT_FOUND;
    EFI_HANDLE *handles = 0;
    UINTN handle_count = 0;
    EFI_LOCATE_HANDLE_BUFFER locate_handle_buffer =
        (EFI_LOCATE_HANDLE_BUFFER)system->BootServices->LocateHandleBuffer;
    EFI_HANDLE_PROTOCOL handle_protocol =
        (EFI_HANDLE_PROTOCOL)system->BootServices->HandleProtocol;
    EFI_CONNECT_CONTROLLER connect_controller =
        (EFI_CONNECT_CONTROLLER)system->BootServices->ConnectController;
    EFI_LOCATE_PROTOCOL locate_protocol =
        (EFI_LOCATE_PROTOCOL)system->BootServices->LocateProtocol;

    if (connect_controller && system->ConsoleOutHandle) {
        connect_controller(system->ConsoleOutHandle, 0, 0, TRUE);
    }

    if (locate_protocol) {
        status = locate_protocol(&gop_guid, 0, (void **)&gop);
    }

    if ((EFI_ERROR(status) || !gop) && handle_protocol && system->ConsoleOutHandle) {
        status = handle_protocol(system->ConsoleOutHandle, &gop_guid, (void **)&gop);
    }

    if ((EFI_ERROR(status) || !gop) && locate_handle_buffer && handle_protocol) {
        status = locate_handle_buffer(EFI_LOCATE_SEARCH_TYPE_BY_PROTOCOL,
                                      &gop_guid, 0, &handle_count, &handles);
        if (EFI_ERROR(status)) {
            if (!gop_handle_locate_failed) {
                uefi_log_status(system, "[boot] LocateHandleBuffer error: ", status);
                gop_handle_locate_failed = 1;
            }
        } else {
            uefi_log(system, "[boot] LocateHandleBuffer succeeded\n");
            gop_handle_locate_failed = 0;
        }
        if (!EFI_ERROR(status)) {
            for (UINTN i = 0; i < handle_count; ++i) {
                EFI_HANDLE candidate = handles[i];
                if (connect_controller) {
                    connect_controller(candidate, 0, 0, TRUE);
                }
                status = handle_protocol(candidate, &gop_guid, (void **)&gop);
                if (EFI_ERROR(status) || !gop) {
                    continue;
                }
                break;
            }
        }
    }

    if (handles && system->BootServices && system->BootServices->FreePool) {
        system->BootServices->FreePool(handles);
    }
    static int pci_fallback_attempted = 0;
    if (EFI_ERROR(status)) {
        if (!pci_fallback_attempted) {
            pci_fallback_attempted = 1;
            if (capture_framebuffer_from_pci_config(system, snapshot) == 0) {
                return 0;
            }
        }
        if (!gop_locate_failed) {
            uefi_log_status(system, "[boot] LocateProtocol(GOP) failed: ", status);
            gop_locate_failed = 1;
        }
        return -1;
    }
    if (!gop) {
        if (!gop_locate_failed) {
            uefi_log(system, "[boot] GOP missing after handle iteration\n");
            gop_locate_failed = 1;
        }
        return -1;
    }

    EFI_GOP_SET_MODE set_mode = (EFI_GOP_SET_MODE)gop->SetMode;
    EFI_GOP_QUERY_MODE query_mode = (EFI_GOP_QUERY_MODE)gop->QueryMode;

    if (!gop->Mode) {
        if (!set_mode) {
            if (!gop_locate_failed) {
                uefi_log(system, "[boot] GOP SetMode missing\n");
                gop_locate_failed = 1;
            }
            return -1;
        }
        status = set_mode(gop, 0);
        if (EFI_ERROR(status) || !gop->Mode) {
            if (!gop_locate_failed) {
                uefi_log(system, "[boot] GOP mode missing after SetMode\n");
                gop_locate_failed = 1;
            }
            return -1;
        }
    }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info = gop->Mode->Info;
    UINTN mode_size = 0;
    if ((!mode_info || mode_info->HorizontalResolution == 0) && query_mode) {
        status = query_mode(gop, gop->Mode->Mode, &mode_size, &mode_info);
        if (EFI_ERROR(status) || !mode_info) {
            if (!gop_locate_failed) {
                uefi_log(system, "[boot] GOP QueryMode failed\n");
                gop_locate_failed = 1;
            }
            return -1;
        }
    }
    if (!mode_info) {
        if (!gop_locate_failed) {
            uefi_log(system, "[boot] GOP mode info missing\n");
            gop_locate_failed = 1;
        }
        return -1;
    }
    snapshot->base = (uint64_t)(uintptr_t)gop->Mode->FrameBufferBase;
    snapshot->size = gop->Mode->FrameBufferSize;
    snapshot->width = mode_info->HorizontalResolution;
    snapshot->height = mode_info->VerticalResolution;
    snapshot->stride = mode_info->PixelsPerScanLine;
    snapshot->flags = BOOT_INFO_FLAG_GOP_PRESENT;
    return 0;
}

static void apply_framebuffer_snapshot(boot_info_t *boot_info,
                                       const framebuffer_snapshot_t *snapshot)
{
    if (!boot_info || !snapshot || snapshot->base == 0 || snapshot->size == 0) {
        return;
    }

    boot_info->framebuffer_base = (void *)(uintptr_t)snapshot->base;
    boot_info->framebuffer_size = snapshot->size;
    boot_info->framebuffer_width = snapshot->width;
    boot_info->framebuffer_height = snapshot->height;
    boot_info->framebuffer_pixels_per_scanline = snapshot->stride;
    boot_info->flags |= snapshot->flags;
}

static void uefi_hex(UINT64 value, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        out[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    out[18] = '\0';
}

static void uefi_log_hex_prefixed(EFI_SYSTEM_TABLE *system, const char *prefix, uint64_t value) {
    char hex_buf[19];
    uefi_hex(value, hex_buf);
    uefi_log(system, prefix);
    uefi_log(system, hex_buf);
    uefi_log(system, "\n");
}

static void uefi_log(EFI_SYSTEM_TABLE *system, const char *msg) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = system->ConOut;
    if (!out || !out->OutputString) {
        return;
    }

    CHAR16 buf[256];
    UINTN idx = 0;
    for (UINTN i = 0; msg[i] != '\0' && idx + 1 < (sizeof(buf) / sizeof(buf[0])); ++i) {
        char c = msg[i];
        if (c == '\n') {
            if (idx + 2 >= (sizeof(buf) / sizeof(buf[0]))) {
                break;
            }
            buf[idx++] = '\r';
            buf[idx++] = '\n';
        } else {
            buf[idx++] = (CHAR16)c;
        }
    }
    buf[idx] = 0;
    out->OutputString(out, buf);
}

static void uefi_log_status(EFI_SYSTEM_TABLE *system, const char *msg, EFI_STATUS status) {
    char hex_buf[19];
    uefi_hex((UINT64)status, hex_buf);
    uefi_log(system, msg);
    uefi_log(system, hex_buf);
    uefi_log(system, "\n");
}

static int elf_is_valid(const Elf64_Ehdr *ehdr) {
    return ehdr->e_ident[0] == 0x7f &&
           ehdr->e_ident[1] == 'E' &&
           ehdr->e_ident[2] == 'L' &&
           ehdr->e_ident[3] == 'F';
}

static EFI_STATUS read_file_alloc(EFI_BOOT_SERVICES *bs,
                                  EFI_FILE_PROTOCOL *root,
                                  CHAR16 *path,
                                  void **out_buf,
                                  UINTN *out_size) {
    if (!bs || !root || !path || !out_buf || !out_size) {
        return 1;
    }

    EFI_STATUS status;
    EFI_FILE_PROTOCOL *file = 0;
    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        return status;
    }

    /*
     * UEFI file reads are size-driven, so resolve the file length first through
     * EFI_FILE_INFO and then allocate a pool buffer large enough to hold the
     * entire payload. The kernel expects every preloaded module as a contiguous
     * blob.
     */
    EFI_GUID file_info_guid = EFI_FILE_INFO_GUID;
    UINTN info_size = 0;
    EFI_FILE_INFO *info = 0;
    status = file->GetInfo(file, &file_info_guid, &info_size, info);
    if (status == EFI_BUFFER_TOO_SMALL) {
        status = bs->AllocatePool(EFI_LOADER_DATA, info_size, (void **)&info);
        if (EFI_ERROR(status)) {
            return status;
        }
        status = file->GetInfo(file, &file_info_guid, &info_size, info);
    }
    if (EFI_ERROR(status)) {
        return status;
    }

    UINTN size = (UINTN)info->FileSize;
    void *buf = 0;
    status = bs->AllocatePool(EFI_LOADER_DATA, size, &buf);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = file->Read(file, &size, buf);
    if (EFI_ERROR(status)) {
        return status;
    }

    *out_buf = buf;
    *out_size = size;
    return EFI_SUCCESS;
}

static int
initfs_valid(const void *blob, UINTN size)
{
    if (!blob || size < sizeof(wasmos_initfs_header_t)) {
        return 0;
    }
    const wasmos_initfs_header_t *hdr = (const wasmos_initfs_header_t *)blob;
    for (UINTN i = 0; i < 8; ++i) {
        if ((UINT8)hdr->magic[i] != (UINT8)WASMOS_INITFS_MAGIC[i]) {
            return 0;
        }
    }
    if (hdr->version != WASMOS_INITFS_VERSION ||
        hdr->header_size != sizeof(wasmos_initfs_header_t) ||
        hdr->entry_size != sizeof(wasmos_initfs_entry_t) ||
        hdr->total_size > size) {
        return 0;
    }
    UINTN table_bytes = (UINTN)hdr->entry_count * sizeof(wasmos_initfs_entry_t);
    if (hdr->header_size + table_bytes > hdr->total_size) {
        return 0;
    }
    const wasmos_initfs_entry_t *entries =
        (const wasmos_initfs_entry_t *)((const UINT8 *)blob + hdr->header_size);
    for (UINT32 i = 0; i < hdr->entry_count; ++i) {
        const wasmos_initfs_entry_t *entry = &entries[i];
        UINT64 end = (UINT64)entry->offset + (UINT64)entry->size;
        if (entry->offset < hdr->header_size + table_bytes || end > hdr->total_size) {
            return 0;
        }
    }
    return 1;
}

static void
uefi_log_initfs_entry(EFI_SYSTEM_TABLE *system, const char *prefix, const wasmos_initfs_entry_t *entry)
{
    if (!system || !prefix || !entry) {
        return;
    }
    uefi_log(system, prefix);
    uefi_log(system, entry->path);
    uefi_log(system, "\n");
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system) {
    EFI_BOOT_SERVICES *bs = system->BootServices;
    EFI_STATUS status;

    uefi_log(system, "[boot] start\n");
    EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    status = bs->HandleProtocol(image, &lip_guid, (void **)&loaded);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] HandleProtocol failed: ", status);
        return status;
    }

    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    status = bs->HandleProtocol(loaded->DeviceHandle, &fs_guid, (void **)&fs);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] Simple FS HandleProtocol failed: ", status);
        return status;
    }

    EFI_FILE_PROTOCOL *root = 0;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] OpenVolume failed: ", status);
        return status;
    }

    /* The kernel image itself is always loaded from the ESP root. */
    static CHAR16 kernel_path[] = L"\\kernel.elf";
    void *kernel_buf = 0;
    UINTN kernel_size = 0;
    status = read_file_alloc(bs, root, kernel_path, &kernel_buf, &kernel_size);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] Read \\\\kernel.elf failed: ", status);
        return status;
    }

    /* The bootloader now loads one initfs image instead of a hardcoded list of
     * individual bootstrap modules. It still exposes the contained WASMOS-APP
     * entries as boot modules so the kernel and early services can keep the
     * same module-index bootstrap contract. */
    static CHAR16 initfs_path[] = L"\\initfs.img";
    void *initfs_buf = 0;
    UINTN initfs_size = 0;
    status = read_file_alloc(bs, root, initfs_path, &initfs_buf, &initfs_size);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] Read \\\\initfs.img failed: ", status);
        return status;
    }
    if (!initfs_valid(initfs_buf, initfs_size)) {
        uefi_log(system, "[boot] invalid initfs image\n");
        return 1;
    }
    uefi_log(system, "[boot] loaded initfs: \\\\initfs.img\n");

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)kernel_buf;
    if (!elf_is_valid(ehdr)) {
        uefi_log(system, "[boot] Invalid ELF header\n");
        return 1;
    }

    uefi_log(system, "[boot] Loading PT_LOAD segments\n");
    Elf64_Phdr *phdrs = (Elf64_Phdr *)((UINT8 *)kernel_buf + ehdr->e_phoff);
    UINT64 alloc_bases[16];
    UINT64 alloc_pages[16];
    UINTN alloc_count = 0;
    for (UINT16 i = 0; i < ehdr->e_phnum; ++i) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) {
            continue;
        }

        UINT64 page_offset = ph->p_paddr & 0xFFF;
        UINT64 dest_base = ph->p_paddr - page_offset;
        UINT64 total_mem = ph->p_memsz + page_offset;
        UINT64 pages = (total_mem + 0xFFF) / 0x1000;
        UINT64 dest = dest_base;
        UINTN alloc_type = EFI_ALLOCATE_ADDRESS;
        if (dest == 0) {
            alloc_type = EFI_ALLOCATE_ANY_PAGES;
        }
        // Keep PT_LOAD allocation quiet unless it fails.
        int already_allocated = 0;
        for (UINTN j = 0; j < alloc_count; ++j) {
            UINT64 base = alloc_bases[j];
            UINT64 end = base + alloc_pages[j] * 0x1000;
            if (dest >= base && dest < end) {
                already_allocated = 1;
                break;
            }
        }
        if (!already_allocated) {
            status = bs->AllocatePages(alloc_type, EFI_LOADER_DATA, pages, &dest);
            if (EFI_ERROR(status)) {
                uefi_log_status(system, "[boot] AllocatePages failed: ", status);
                return status;
            }
            if (alloc_count < (sizeof(alloc_bases) / sizeof(alloc_bases[0]))) {
                alloc_bases[alloc_count] = dest;
                alloc_pages[alloc_count] = pages;
                alloc_count++;
            }
        }

        void *segment_src = (UINT8 *)kernel_buf + ph->p_offset;
        void *segment_dst = (UINT8 *)(UINTN)dest + page_offset;
        memcpy8(segment_dst, segment_src, (UINTN)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) {
            memset8((UINT8 *)segment_dst + ph->p_filesz, 0, (UINTN)(ph->p_memsz - ph->p_filesz));
        }
    }

    UINT32 rsdp_length = 0;
    void *rsdp = find_acpi_rsdp(system, &rsdp_length);
    if (rsdp) {
        uefi_log(system, "[boot] ACPI RSDP located\n");
    }

    /*
     * Capture the framebuffer snapshot before we start the ExitBootServices()
     * dance. GOP discovery and controller connects can mutate the UEFI memory
     * map, so doing this early keeps the final map_key stable.
     */
    framebuffer_snapshot_t framebuffer_snapshot;
    memset8(&framebuffer_snapshot, 0, sizeof(framebuffer_snapshot));
    int have_framebuffer = (capture_framebuffer_snapshot(system, &framebuffer_snapshot) == 0);
    if (have_framebuffer) {
        uefi_log(system, "[boot] framebuffer info set\n");
        uefi_log_hex_prefixed(system, "  base=", framebuffer_snapshot.base);
        uefi_log_hex_prefixed(system, "  size=", framebuffer_snapshot.size);
        uefi_log_hex_prefixed(system, "  width=", framebuffer_snapshot.width);
        uefi_log_hex_prefixed(system, "  height=", framebuffer_snapshot.height);
        uefi_log_hex_prefixed(system, "  stride=", framebuffer_snapshot.stride);
        uefi_log_hex_prefixed(system, "  flags=", framebuffer_snapshot.flags);
    }

    boot_info_t *boot_info = 0;
    UINTN mmap_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    bs->GetMemoryMap(&mmap_size, 0, &map_key, &desc_size, &desc_version);
    mmap_size += desc_size * 2;
    UINTN mmap_capacity = mmap_size;
    UINTN boot_capacity = 0;
    const wasmos_initfs_header_t *initfs_hdr = (const wasmos_initfs_header_t *)initfs_buf;
    const wasmos_initfs_entry_t *initfs_entries =
        (const wasmos_initfs_entry_t *)((const UINT8 *)initfs_buf + initfs_hdr->header_size);
    UINTN module_count = 0;
    for (UINT32 i = 0; i < initfs_hdr->entry_count; ++i) {
        if (initfs_entries[i].type == WASMOS_INITFS_ENTRY_WASMOS_APP) {
            module_count++;
            uefi_log_initfs_entry(system, "[boot] initfs module: ", &initfs_entries[i]);
        }
    }
    UINTN module_table_bytes = module_count * sizeof(boot_module_t);

    void *mmap = 0;
    UINT64 boot_buf = 0;

    uefi_log(system, "[boot] ExitBootServices\n");
    int exited = 0;
    void *map_dst = 0;
    UINTN map_bytes = 0;
    while (!exited) {
        if (!mmap) {
            status = bs->AllocatePool(EFI_LOADER_DATA, mmap_capacity, &mmap);
            if (EFI_ERROR(status)) {
                uefi_log_status(system, "[boot] AllocatePool(mmap) failed: ", status);
                return status;
            }
        }

        mmap_size = mmap_capacity;
        status = bs->GetMemoryMap(&mmap_size, mmap, &map_key, &desc_size, &desc_version);
        if (status == EFI_BUFFER_TOO_SMALL) {
            mmap_capacity = mmap_size + desc_size * 2;
            mmap = 0;
            continue;
        }
        if (EFI_ERROR(status)) {
            uefi_log_status(system, "[boot] GetMemoryMap retry failed: ", status);
            return status;
        }

        UINTN boot_bytes = sizeof(boot_info_t);
        map_bytes = mmap_size;
        UINTN total_bytes = boot_bytes + map_bytes + initfs_size + module_table_bytes;
        UINTN total_pages = (total_bytes + 0xFFF) / 0x1000;
        if (boot_capacity < total_pages) {
            status = bs->AllocatePages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, total_pages, &boot_buf);
            if (EFI_ERROR(status)) {
                uefi_log_status(system, "[boot] AllocatePages(boot info) failed: ", status);
                return status;
            }
            boot_capacity = total_pages;
            boot_info = (boot_info_t *)(UINTN)boot_buf;
        }

        memset8(boot_info, 0, sizeof(boot_info_t));
        boot_info->version = BOOT_INFO_VERSION;
        boot_info->size = (uint32_t)sizeof(boot_info_t);
        boot_info->flags = 0;
        map_dst = (void *)((UINT8 *)boot_info + boot_bytes);
        memcpy8(map_dst, mmap, map_bytes);
        boot_info->memory_map = map_dst;
        boot_info->memory_map_size = map_bytes;
        boot_info->memory_desc_size = desc_size;
        boot_info->memory_desc_version = desc_version;
        boot_info->modules = 0;
        boot_info->module_count = 0;
        boot_info->module_entry_size = sizeof(boot_module_t);
        boot_info->rsdp = rsdp;
        boot_info->rsdp_length = rsdp_length;
        boot_info->initfs = 0;
        boot_info->initfs_size = 0;
        boot_info->boot_config = 0;
        boot_info->boot_config_size = 0;

        if (have_framebuffer) {
            apply_framebuffer_snapshot(boot_info, &framebuffer_snapshot);
        }

        UINT8 *cursor = (UINT8 *)map_dst + map_bytes;
        UINT8 *initfs_copy = cursor;
        memcpy8(initfs_copy, initfs_buf, initfs_size);
        boot_info->initfs = initfs_copy;
        boot_info->initfs_size = (uint32_t)initfs_size;
        boot_info->flags |= BOOT_INFO_FLAG_INITFS_PRESENT;
        cursor += initfs_size;

        if (module_count > 0) {
            boot_module_t *mods = (boot_module_t *)cursor;
            memset8(mods, 0, module_table_bytes);
            cursor += module_table_bytes;

            UINT32 mod_index = 0;
            const wasmos_initfs_header_t *copied_hdr = (const wasmos_initfs_header_t *)initfs_copy;
            const wasmos_initfs_entry_t *copied_entries =
                (const wasmos_initfs_entry_t *)(initfs_copy + copied_hdr->header_size);
            for (UINT32 i = 0; i < copied_hdr->entry_count; ++i) {
                const wasmos_initfs_entry_t *entry = &copied_entries[i];
                UINT8 *payload = initfs_copy + entry->offset;
                if (entry->type == WASMOS_INITFS_ENTRY_WASMOS_APP) {
                    mods[mod_index].base = (UINT64)(UINTN)payload;
                    mods[mod_index].size = entry->size;
                    mods[mod_index].type = BOOT_MODULE_TYPE_WASMOS_APP;
                    mods[mod_index].reserved = 0;
                    copy_cstr(mods[mod_index].name, sizeof(mods[mod_index].name), entry->path);
                    mod_index++;
                } else if (entry->type == WASMOS_INITFS_ENTRY_CONFIG) {
                    boot_info->boot_config = payload;
                    boot_info->boot_config_size = entry->size;
                }
        }

        boot_info->modules = mods;
        boot_info->module_count = (uint32_t)module_count;
        boot_info->flags |= BOOT_INFO_FLAG_MODULES_PRESENT;
    }

        status = bs->ExitBootServices(image, map_key);
        if (!EFI_ERROR(status)) {
            exited = 1;
        } else if (status != EFI_INVALID_PARAMETER) {
            uefi_log_status(system, "[boot] ExitBootServices failed: ", status);
            return status;
        }
    }

    void (*kernel_entry)(boot_info_t *) = (void (*)(boot_info_t *))(UINTN)ehdr->e_entry;
    kernel_entry(boot_info);

    return EFI_SUCCESS;
}
