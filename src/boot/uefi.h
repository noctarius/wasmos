/* uefi.h - Minimal UEFI type and protocol definitions for the WASMOS bootloader.
 * Only the subset needed to load kernel.elf and initfs.img from the ESP is declared.
 * All function pointers use __attribute__((ms_abi)) because UEFI firmware on x86_64
 * uses the Windows calling convention regardless of host toolchain. */
#ifndef WASMOS_UEFI_H
#define WASMOS_UEFI_H

#include <stdint.h>

/* EFI_STATUS values.  A UEFI status is an error when its top bit is set, which
 * is what EFI_ERROR(x) approximates here: it treats every non-EFI_SUCCESS value
 * as a failure, so the spec's warning codes (top bit clear, value non-zero, e.g.
 * EFI_WARN_DELETE_FAILURE) are reported as errors too.  No call site in the
 * loader distinguishes warnings, so the approximation is exact in practice. */
#define EFI_SUCCESS 0
#define EFI_NOT_FOUND ((EFI_STATUS)0x800000000000000EULL)
#define EFI_ERROR(x) ((x) != EFI_SUCCESS)

typedef uint64_t EFI_STATUS;
/* Opaque firmware object reference (EFI_HANDLE).  Never dereferenced by the
 * loader; only passed back to boot services. */
typedef void* EFI_HANDLE;

/* UEFI strings are UCS-2, not UTF-8: every path literal handed to
 * EFI_FILE_PROTOCOL.Open must be an L"..." wide string. */
typedef uint16_t CHAR16;
/* Spec scalar names.  UINTN/INTN are the spec's native-width integers and are
 * fixed at 64 bits here because the loader only targets x86_64; a 32-bit UEFI
 * target would change their width and every struct that embeds them. */
typedef uint64_t UINTN;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t UINT8;
typedef int64_t INTN;
typedef uint8_t BOOLEAN;
#define TRUE 1
#define FALSE 0

#define EFIAPI __attribute__((ms_abi))

/* EFI_GUID.  Data1..Data3 are stored little-endian (so a GUID literal is written
 * in the spec's textual field order and needs no byte swapping), Data4 is a byte
 * array in printed order.  Compared field-wise by guid_eq() in boot.c. */
typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

/* SearchType argument of LocateHandle/LocateHandleBuffer.  The loader only ever
 * passes BY_PROTOCOL, which searches for handles supporting a given GUID. */
typedef enum {
    EFI_LOCATE_SEARCH_TYPE_BY_HANDLE,
    EFI_LOCATE_SEARCH_TYPE_BY_REGISTER_NOTIFY,
    EFI_LOCATE_SEARCH_TYPE_BY_PROTOCOL
} EFI_LOCATE_SEARCH_TYPE;

/* Call-through types for the four boot services the loader reaches by way of a
 * void* slot in EFI_BOOT_SERVICES rather than a typed member.  All four return
 * EFI_SUCCESS (0) on success and an EFI_ERROR status otherwise.
 *
 * EFI_LOCATE_HANDLE_BUFFER allocates *Buffer from the boot-services pool and
 * transfers it to the caller, who must FreePool it; *NoHandles is the element
 * count.  Nothing is allocated on failure.
 *
 * EFI_LOCATE_PROTOCOL / EFI_HANDLE_PROTOCOL return a borrowed *Interface owned
 * by the firmware; it stays valid until ExitBootServices.
 *
 * EFI_CONNECT_CONTROLLER binds drivers to a controller handle; Recursive=TRUE
 * walks the child handles it creates.  The loader uses it to force GOP-producing
 * drivers to attach before the framebuffer snapshot is taken. */
typedef EFI_STATUS(EFIAPI* EFI_LOCATE_HANDLE_BUFFER)(EFI_LOCATE_SEARCH_TYPE SearchType,
                                                     const EFI_GUID* Protocol, void* SearchKey,
                                                     UINTN* NoHandles, EFI_HANDLE** Buffer);

typedef EFI_STATUS(EFIAPI* EFI_LOCATE_PROTOCOL)(const EFI_GUID* Protocol, void* Registration,
                                                void** Interface);

typedef EFI_STATUS(EFIAPI* EFI_HANDLE_PROTOCOL)(EFI_HANDLE Handle, const EFI_GUID* Protocol,
                                                void** Interface);

typedef EFI_STATUS(EFIAPI* EFI_CONNECT_CONTROLLER)(EFI_HANDLE ControllerHandle,
                                                   EFI_HANDLE* DriverImageHandle,
                                                   void* RemainingDevicePath, BOOLEAN Recursive);

/* Common header of every UEFI table (EFI_TABLE_HEADER).  The loader reads none
 * of these fields; the struct exists so the members after it land at the
 * spec-mandated offsets. */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL.  Only OutputString is typed; the remaining
 * members are void* placeholders that exist to preserve the spec's member order,
 * and calling one through this declaration is not possible.  OutputString takes
 * a NUL-terminated CHAR16 string and does not translate '\n', so the caller
 * emits "\r\n" itself (uefi_log() in boot.c does). */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void* Reset;
    EFI_STATUS(EFIAPI* OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, CHAR16* String);
    void* TestString;
    void* QueryMode;
    void* SetMode;
    void* SetAttribute;
    void* ClearScreen;
    void* SetCursorPosition;
    void* EnableCursor;
};

/* EFI_SYSTEM_TABLE, the root object handed to efi_main.  Only ConOut,
 * ConsoleOutHandle/StandardErrorHandle (used to force controller connects),
 * BootServices and the configuration table (searched for the ACPI RSDP) are
 * consumed; RuntimeServices, ConIn and StdErr are void* placeholders holding
 * spec offsets.  NumberOfTableEntries counts EFI_CONFIGURATION_TABLE elements at
 * ConfigurationTable.  Every pointer in the table is firmware-owned and becomes
 * invalid for boot-services members once ExitBootServices succeeds. */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16* FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void* ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;
    EFI_HANDLE StandardErrorHandle;
    void* StdErr;
    void* RuntimeServices;
    struct _EFI_BOOT_SERVICES* BootServices;
    UINTN NumberOfTableEntries;
    void* ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* EFI_BOOT_SERVICES.  Members the loader calls are typed; the rest are void*
 * placeholders that only hold the spec's offsets, and a few of those (notably
 * ConnectController, LocateHandleBuffer) are cast to the matching typedef above
 * at the call site.  Contracts of the typed members, all returning EFI_SUCCESS
 * (0) on success:
 *
 *   AllocatePages(Type, MemoryType, Pages, Memory) - Pages counts 4 KiB pages.
 *     *Memory is in/out: an input address for EFI_ALLOCATE_ADDRESS (fails if the
 *     range is already allocated) and output-only for EFI_ALLOCATE_ANY_PAGES.
 *   GetMemoryMap(Size, Map, Key, DescSize, DescVersion) - *Size is in/out; with
 *     Map=0 or a short *Size it returns EFI_BUFFER_TOO_SMALL and writes the
 *     needed byte count.  Descriptors are DescSize bytes apart, which is NOT
 *     necessarily sizeof(EFI_MEMORY_DESCRIPTOR).  *Key is the map key that
 *     ExitBootServices validates; ANY subsequent allocation invalidates it.
 *   AllocatePool/FreePool - byte-granular pool; the buffer is transferred to the
 *     caller and must be freed before ExitBootServices or it stays allocated as
 *     its MemoryType and the kernel never reclaims it.
 *   HandleProtocol/LocateProtocol - see the typedefs above; *Interface is
 *     borrowed from the firmware.
 *   ExitBootServices(ImageHandle, MapKey) - returns EFI_INVALID_PARAMETER when
 *     MapKey is stale, which is a retryable condition, not a fatal one.  After
 *     it succeeds no boot service may be called again. */
typedef struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    void* RaiseTPL;
    void* RestoreTPL;
    EFI_STATUS(EFIAPI* AllocatePages)(UINTN Type, UINTN MemoryType, UINTN Pages, UINT64* Memory);
    EFI_STATUS(EFIAPI* FreePages)(UINT64 Memory, UINTN Pages);
    EFI_STATUS(EFIAPI* GetMemoryMap)(UINTN* MemoryMapSize, void* MemoryMap, UINTN* MapKey,
                                     UINTN* DescriptorSize, UINT32* DescriptorVersion);
    EFI_STATUS(EFIAPI* AllocatePool)(UINTN PoolType, UINTN Size, void** Buffer);
    EFI_STATUS(EFIAPI* FreePool)(void* Buffer);
    void* CreateEvent;
    void* SetTimer;
    void* WaitForEvent;
    void* SignalEvent;
    void* CloseEvent;
    void* CheckEvent;
    void* InstallProtocolInterface;
    void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    EFI_STATUS(EFIAPI* HandleProtocol)(EFI_HANDLE Handle, const EFI_GUID* Protocol,
                                       void** Interface);
    void* Reserved;
    void* RegisterProtocolNotify;
    void* LocateHandle;
    void* LocateDevicePath;
    void* InstallConfigurationTable;
    void* LoadImage;
    void* StartImage;
    void* Exit;
    void* UnloadImage;
    EFI_STATUS(EFIAPI* ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    void* GetNextMonotonicCount;
    void* Stall;
    void* SetWatchdogTimer;
    void* ConnectController;
    void* DisconnectController;
    void* OpenProtocol;
    void* CloseProtocol;
    void* OpenProtocolInformation;
    void* ProtocolsPerHandle;
    void* LocateHandleBuffer;
    EFI_STATUS(EFIAPI* LocateProtocol)(const EFI_GUID* Protocol, void* Registration,
                                       void** Interface);
    void* InstallMultipleProtocolInterfaces;
    void* UninstallMultipleProtocolInterfaces;
    void* CalculateCrc32;
    void* CopyMem;
    void* SetMem;
    void* CreateEventEx;
} EFI_BOOT_SERVICES;

/* EFI_MEMORY_DESCRIPTOR, one entry of the GetMemoryMap array.  Type is an
 * EFI_MEMORY_TYPE (EFI_LOADER_DATA = 2 is what this loader allocates with),
 * NumberOfPages counts 4 KiB pages from PhysicalStart, and Attribute carries the
 * EFI_MEMORY_* cacheability/protection bits.  VirtualStart stays 0 unless the
 * OS calls SetVirtualAddressMap, which this loader does not.  Array elements are
 * spaced by the DescriptorSize GetMemoryMap reports, not by this sizeof. */
typedef struct {
    UINT32 Type;
    UINT64 PhysicalStart;
    UINT64 VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* EFI_CONFIGURATION_TABLE entry.  The array at EFI_SYSTEM_TABLE.ConfigurationTable
 * maps a vendor GUID to an unstructured table pointer; the loader scans it for
 * the ACPI 2.0 and ACPI 1.0 GUIDs to recover the RSDP. */
typedef struct {
    EFI_GUID VendorGuid;
    void* VendorTable;
} EFI_CONFIGURATION_TABLE;

/* EFI_LOADED_IMAGE_PROTOCOL, obtained by HandleProtocol on the loader's own
 * image handle.  DeviceHandle is the only field read: it identifies the volume
 * the image was loaded from, i.e. the ESP whose filesystem protocol holds
 * kernel.elf and initfs.img. */
typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE* SystemTable;
    EFI_HANDLE DeviceHandle;
    void* FilePath;
    void* Reserved;
    UINT32 LoadOptionsSize;
    void* LoadOptions;
    void* ImageBase;
    UINT64 ImageSize;
    UINT32 ImageCodeType;
    UINT32 ImageDataType;
    EFI_STATUS(EFIAPI* Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

/* EFI_FILE_INFO, the structure GetInfo returns for EFI_FILE_INFO_GUID.  Size is
 * the byte size of this variable-length record including the FileName tail;
 * FileSize is the logical file length the loader allocates a buffer for, and
 * PhysicalSize the space occupied on the medium.  The three timestamps are
 * EFI_TIME records, declared here as opaque 16-byte pairs because the loader
 * does not read them.  FileName is a NUL-terminated CHAR16 string that extends
 * past the declared [1] element. */
typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    struct {
        UINT64 CreateTime[2];
    } CreateTime;
    struct {
        UINT64 LastAccessTime[2];
    } LastAccessTime;
    struct {
        UINT64 ModificationTime[2];
    } ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

/* EFI_FILE_PROTOCOL, a handle to one open file or directory.  Typed members:
 *
 *   Open(This, NewHandle, FileName, OpenMode, Attributes) - FileName is a CHAR16
 *     path relative to This, using '\' separators; OpenMode is a bitmask such as
 *     EFI_FILE_MODE_READ.  *NewHandle is transferred to the caller and released
 *     with Close.
 *   Read(This, BufferSize, Buffer) - *BufferSize is in/out: bytes requested in,
 *     bytes actually read out.  A short read is reported that way, not as an
 *     error, so the caller must check the returned count.
 *   GetInfo(This, InformationType, BufferSize, Buffer) - with a *BufferSize too
 *     small it returns EFI_BUFFER_TOO_SMALL and writes the required size, which
 *     is the two-call pattern read_file_alloc() in boot.c relies on.
 *
 * Delete/Write/GetPosition/SetPosition are void* placeholders holding offsets. */
struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS(EFIAPI* Open)(EFI_FILE_PROTOCOL* This, EFI_FILE_PROTOCOL** NewHandle,
                             CHAR16* FileName, UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS(EFIAPI* Close)(EFI_FILE_PROTOCOL* This);
    void* Delete;
    EFI_STATUS(EFIAPI* Read)(EFI_FILE_PROTOCOL* This, UINTN* BufferSize, void* Buffer);
    void* Write;
    void* GetPosition;
    void* SetPosition;
    EFI_STATUS(EFIAPI* GetInfo)(EFI_FILE_PROTOCOL* This, const EFI_GUID* InformationType,
                                UINTN* BufferSize, void* Buffer);
};

/* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL.  OpenVolume returns the volume's root
 * directory in *Root; the handle is transferred to the caller and is the base
 * every subsequent Open() path is resolved against. */
typedef struct {
    UINT64 Revision;
    EFI_STATUS(EFIAPI* OpenVolume)(void* This, EFI_FILE_PROTOCOL** Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* EFI_PCI_IO_PROTOCOL_WIDTH: access width for the PCI I/O accessors.  The plain
 * Uint8..Uint64 widths advance the address per element, the Fifo* widths repeat
 * at a fixed address, and the Fill* widths repeat one buffer value across the
 * address range. */
typedef enum {
    EfiPciIoWidthUint8,
    EfiPciIoWidthUint16,
    EfiPciIoWidthUint32,
    EfiPciIoWidthUint64,
    EfiPciIoWidthFifoUint8,
    EfiPciIoWidthFifoUint16,
    EfiPciIoWidthFifoUint32,
    EfiPciIoWidthFifoUint64,
    EfiPciIoWidthFillUint8,
    EfiPciIoWidthFillUint16,
    EfiPciIoWidthFillUint32,
    EfiPciIoWidthFillUint64
} EFI_PCI_IO_PROTOCOL_WIDTH;

/* EFI_PCI_IO_PROTOCOL and its helper types mirror the spec's per-device PCI
 * accessor.  Nothing in the loader calls through them: the only PCI use is
 * connect_graphics_controllers(), which needs the protocol GUID and not the
 * interface, and the framebuffer fallback reads config space directly through
 * ports 0xCF8/0xCFC.  They are declared so a future user has the layout. */
typedef struct _EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;

/* Config-space accessor: Offset is a byte offset into the device's config space,
 * Count the number of Width-sized elements to transfer through Buffer. */
typedef EFI_STATUS(EFIAPI* EFI_PCI_IO_PROTOCOL_PCI)(EFI_PCI_IO_PROTOCOL* This,
                                                    EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset,
                                                    UINTN Count, void* Buffer);

/* Read/Write pair embedded as the Pci member of EFI_PCI_IO_PROTOCOL. */
typedef struct {
    EFI_PCI_IO_PROTOCOL_PCI Read;
    EFI_PCI_IO_PROTOCOL_PCI Write;
} EFI_PCI_IO_PROTOCOL_CONFIG_ACCESS;

/* GetBarAttributes: *BarAttribute is an ACPI 2.0 QWORD Address Space Descriptor
 * chain (EFI_ADDRESS_SPACE_DESCRIPTOR below) allocated from the boot-services
 * pool and transferred to the caller, who must FreePool it. */
typedef EFI_STATUS(EFIAPI* EFI_PCI_IO_PROTOCOL_GET_BAR_ATTRIBUTES)(EFI_PCI_IO_PROTOCOL* This,
                                                                   UINT8 BarIndex,
                                                                   void* ResourceType,
                                                                   void** BarAttribute);

/* ACPI 2.0 QWORD Address Space Descriptor as returned by GetBarAttributes, with
 * the trailing end tag folded in.  QWORD_ASD is the descriptor tag byte (0x8A)
 * and Length the descriptor body length; AddressMin/AddressMax/AddressLength
 * describe the decoded window and AddressTransOffset the CPU-to-bus translation.
 * FIXME: the declaration is not packed, so the compiler inserts padding after
 * QWORD_ASD and before the 8-byte fields and the layout does not match the ACPI
 * byte stream.  Harmless today only because nothing reads it. */
typedef struct {
    uint8_t QWORD_ASD;
    uint16_t Length;
    uint8_t ResourceType;
    uint8_t GeneralFlags;
    uint8_t TypeSpecificFlags;
    uint64_t AddressSpaceGranularity;
    uint64_t AddressMin;
    uint64_t AddressMax;
    uint64_t AddressTransOffset;
    uint64_t AddressLength;
    uint8_t EndTag;
    uint8_t Checksum;
} EFI_ADDRESS_SPACE_DESCRIPTOR;

struct _EFI_PCI_IO_PROTOCOL {
    void* PollMem;
    void* PollIo;
    void* Mem;
    void* Io;
    EFI_PCI_IO_PROTOCOL_CONFIG_ACCESS Pci;
    void* CopyMem;
    void* Map;
    void* Unmap;
    void* AllocateBuffer;
    void* FreeBuffer;
    void* Flush;
    void* GetLocation;
    void* Attributes;
    EFI_PCI_IO_PROTOCOL_GET_BAR_ATTRIBUTES GetBarAttributes;
    void* SetBarAttributes;
    void* RomSize;
    void* RomImage;
};

/* Initialiser for the GUID named EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID in the
 * spec.  The literal below is NOT that GUID's spec value, so the one use of it
 * (connect_graphics_controllers() in boot.c) matches no handle and its connect
 * pass is a silent no-op; see the FIXME at boot.c:213, which covers this and the
 * equally wrong PCI I/O GUID literal defined next to it. */
#define EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID                                                       \
    {0x2c8b3a3c, 0x7c96, 0x4f50, {0x9f, 0xea, 0xf7, 0xb1, 0x00, 0x65, 0x9f, 0xf9}}

/* The root bridge accessors use the same width encoding as the per-device PCI
 * I/O protocol. */
typedef EFI_PCI_IO_PROTOCOL_WIDTH EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH;

/* EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL: the host-bridge-wide counterpart of
 * EFI_PCI_IO_PROTOCOL, addressing config space by a packed
 * bus/device/function/register Address instead of a per-device offset.  Declared
 * for completeness; the loader never calls through it, and only the members up
 * to Pci are declared. */
typedef struct _EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL;

typedef EFI_STATUS(EFIAPI* EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL* This, EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
    UINT64 Address, UINTN Count, void* Buffer);

typedef struct {
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI Read;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI Write;
} EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_CONFIG_ACCESS;

struct _EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL {
    void* PollMem;
    void* PollIo;
    void* Mem;
    void* Io;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_CONFIG_ACCESS Pci;
};

/* EFI_GUID initialisers, each named after its spec identifier and matching the
 * spec value.  They are macros rather than objects because EFI_GUID is passed by
 * address: every use copies the initialiser into a local or static EFI_GUID.
 *
 *   EFI_LOADED_IMAGE_PROTOCOL_GUID       - HandleProtocol on the loader's own
 *                                          image handle; yields DeviceHandle.
 *   EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID - HandleProtocol on that DeviceHandle;
 *                                          yields the ESP volume.
 *   EFI_FILE_INFO_GUID                   - InformationType for
 *                                          EFI_FILE_PROTOCOL.GetInfo, selecting
 *                                          the EFI_FILE_INFO record. */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID                                                             \
    {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID                                                       \
    {0x964e5b22, 0x6459, 0x11d2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_FILE_INFO_GUID                                                                         \
    {0x09576e92, 0x6d3f, 0x11d2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

/* OpenMode bit for a read-only open; the loader never opens for write. */
#define EFI_FILE_MODE_READ 0x0000000000000001ULL

#endif
