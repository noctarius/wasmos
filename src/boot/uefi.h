#ifndef WASMOS_UEFI_H
#define WASMOS_UEFI_H

#include <stdint.h>

#define EFI_SUCCESS 0
#define EFI_NOT_FOUND ((EFI_STATUS)0x800000000000000EULL)
#define EFI_ERROR(x) ((x) != EFI_SUCCESS)

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;

typedef uint16_t CHAR16;
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

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

typedef enum {
    EFI_LOCATE_SEARCH_TYPE_BY_HANDLE,
    EFI_LOCATE_SEARCH_TYPE_BY_REGISTER_NOTIFY,
    EFI_LOCATE_SEARCH_TYPE_BY_PROTOCOL
} EFI_LOCATE_SEARCH_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    EFI_LOCATE_SEARCH_TYPE SearchType,
    const EFI_GUID *Protocol,
    void *SearchKey,
    UINTN *NoHandles,
    EFI_HANDLE **Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    const EFI_GUID *Protocol,
    void *Registration,
    void **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE Handle,
    const EFI_GUID *Protocol,
    void **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_CONNECT_CONTROLLER)(
    EFI_HANDLE ControllerHandle,
    EFI_HANDLE *DriverImageHandle,
    void *RemainingDevicePath,
    BOOLEAN Recursive
);

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
        CHAR16 *String
    );
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
};

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    struct _EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(
        UINTN Type,
        UINTN MemoryType,
        UINTN Pages,
        UINT64 *Memory
    );
    EFI_STATUS (EFIAPI *FreePages)(UINT64 Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(
        UINTN *MemoryMapSize,
        void *MemoryMap,
        UINTN *MapKey,
        UINTN *DescriptorSize,
        UINT32 *DescriptorVersion
    );
    EFI_STATUS (EFIAPI *AllocatePool)(UINTN PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(
        EFI_HANDLE Handle,
        const EFI_GUID *Protocol,
        void **Interface
    );
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(
        const EFI_GUID *Protocol,
        void *Registration,
        void **Interface
    );
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    UINT32 Type;
    UINT64 PhysicalStart;
    UINT64 VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    UINT32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    UINT64 ImageSize;
    UINT32 ImageCodeType;
    UINT32 ImageDataType;
    EFI_STATUS (EFIAPI *Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    struct { UINT64 CreateTime[2]; } CreateTime;
    struct { UINT64 LastAccessTime[2]; } LastAccessTime;
    struct { UINT64 ModificationTime[2]; } ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(
        EFI_FILE_PROTOCOL *This,
        EFI_FILE_PROTOCOL **NewHandle,
        CHAR16 *FileName,
        UINT64 OpenMode,
        UINT64 Attributes
    );
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    void *Write;
    void *GetPosition;
    void *SetPosition;
    EFI_STATUS (EFIAPI *GetInfo)(
        EFI_FILE_PROTOCOL *This,
        const EFI_GUID *InformationType,
        UINTN *BufferSize,
        void *Buffer
    );
};

typedef struct {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(
        void *This,
        EFI_FILE_PROTOCOL **Root
    );
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

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

typedef struct _EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_PCI_IO_PROTOCOL_PCI)(
    EFI_PCI_IO_PROTOCOL *This,
    EFI_PCI_IO_PROTOCOL_WIDTH Width,
    UINT32 Offset,
    UINTN Count,
    void *Buffer
);

typedef struct {
    EFI_PCI_IO_PROTOCOL_PCI Read;
    EFI_PCI_IO_PROTOCOL_PCI Write;
} EFI_PCI_IO_PROTOCOL_CONFIG_ACCESS;

typedef EFI_STATUS (EFIAPI *EFI_PCI_IO_PROTOCOL_GET_BAR_ATTRIBUTES)(
    EFI_PCI_IO_PROTOCOL *This,
    UINT8 BarIndex,
    void *ResourceType,
    void **BarAttribute
);

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
    void *PollMem;
    void *PollIo;
    void *Mem;
    void *Io;
    EFI_PCI_IO_PROTOCOL_CONFIG_ACCESS Pci;
    void *CopyMem;
    void *Map;
    void *Unmap;
    void *AllocateBuffer;
    void *FreeBuffer;
    void *Flush;
    void *GetLocation;
    void *Attributes;
    EFI_PCI_IO_PROTOCOL_GET_BAR_ATTRIBUTES GetBarAttributes;
    void *SetBarAttributes;
    void *RomSize;
    void *RomImage;
};

#define EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID \
    {0x2c8b3a3c, 0x7c96, 0x4f50, {0x9f, 0xea, 0xf7, 0xb1, 0x00, 0x65, 0x9f, 0xf9}}

typedef EFI_PCI_IO_PROTOCOL_WIDTH EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH;

typedef struct _EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
    UINT64 Address,
    UINTN Count,
    void *Buffer
);

typedef struct {
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI Read;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI Write;
} EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_CONFIG_ACCESS;

struct _EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL {
    void *PollMem;
    void *PollIo;
    void *Mem;
    void *Io;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_CONFIG_ACCESS Pci;
};

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5B1B31A1,0x9562,0x11d2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}}

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    {0x964e5b22,0x6459,0x11d2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}

#define EFI_FILE_INFO_GUID \
    {0x09576e92,0x6d3f,0x11d2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

#endif
