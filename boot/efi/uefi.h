#ifndef WASMOS_UEFI_H
#define WASMOS_UEFI_H

#include <stdint.h>

#define EFI_SUCCESS 0
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

#define EFIAPI __attribute__((ms_abi))

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    void *ConOut;
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
    EFI_STATUS (EFIAPI *HandleProtocol)(
        EFI_HANDLE Handle,
        const EFI_GUID *Protocol,
        void **Interface
    );
    EFI_STATUS (EFIAPI *LocateProtocol)(
        const EFI_GUID *Protocol,
        void *Registration,
        void **Interface
    );
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
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

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5B1B31A1,0x9562,0x11d2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}}

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    {0x964e5b22,0x6459,0x11d2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}

#define EFI_FILE_INFO_GUID \
    {0x09576e92,0x6d3f,0x11d2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

#endif
