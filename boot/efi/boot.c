#include "uefi.h"
#include "elf.h"
#include "boot.h"

#define EFI_ALLOCATE_ADDRESS 2
#define EFI_LOADER_DATA 2
#define EFI_BUFFER_TOO_SMALL ((EFI_STATUS)0x8000000000000005ULL)

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

static void uefi_hex(UINT64 value, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        out[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    out[18] = '\0';
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

    EFI_FILE_PROTOCOL *kernel = 0;
    static CHAR16 kernel_path[] = L"\\kernel.elf";
    status = root->Open(root, &kernel, kernel_path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] Open \\\\kernel.elf failed: ", status);
        return status;
    }

    EFI_GUID file_info_guid = EFI_FILE_INFO_GUID;
    UINTN info_size = 0;
    EFI_FILE_INFO *info = 0;
    status = kernel->GetInfo(kernel, &file_info_guid, &info_size, info);
    if (status == EFI_BUFFER_TOO_SMALL) {
        status = bs->AllocatePool(EFI_LOADER_DATA, info_size, (void **)&info);
        if (EFI_ERROR(status)) {
            uefi_log_status(system, "[boot] AllocatePool(file info) failed: ", status);
            return status;
        }
        status = kernel->GetInfo(kernel, &file_info_guid, &info_size, info);
    }
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] GetInfo failed: ", status);
        return status;
    }

    void *kernel_buf = 0;
    UINTN kernel_size = (UINTN)info->FileSize;
    status = bs->AllocatePool(EFI_LOADER_DATA, kernel_size, &kernel_buf);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] AllocatePool(kernel) failed: ", status);
        return status;
    }

    status = kernel->Read(kernel, &kernel_size, kernel_buf);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] Read kernel failed: ", status);
        return status;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)kernel_buf;
    if (!elf_is_valid(ehdr)) {
        uefi_log(system, "[boot] Invalid ELF header\n");
        return 1;
    }

    uefi_log(system, "[boot] Loading PT_LOAD segments\n");
    Elf64_Phdr *phdrs = (Elf64_Phdr *)((UINT8 *)kernel_buf + ehdr->e_phoff);
    for (UINT16 i = 0; i < ehdr->e_phnum; ++i) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) {
            continue;
        }

        UINT64 pages = (ph->p_memsz + 0xFFF) / 0x1000;
        UINT64 dest = ph->p_paddr;
        status = bs->AllocatePages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA, pages, &dest);
        if (EFI_ERROR(status)) {
            uefi_log_status(system, "[boot] AllocatePages failed: ", status);
            return status;
        }

        void *segment_src = (UINT8 *)kernel_buf + ph->p_offset;
        void *segment_dst = (void *)(UINTN)dest;
        memcpy8(segment_dst, segment_src, (UINTN)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) {
            memset8((UINT8 *)segment_dst + ph->p_filesz, 0, (UINTN)(ph->p_memsz - ph->p_filesz));
        }
    }

    boot_info_t *boot_info = 0;
    status = bs->AllocatePool(EFI_LOADER_DATA, sizeof(boot_info_t), (void **)&boot_info);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] AllocatePool(boot info) failed: ", status);
        return status;
    }
    memset8(boot_info, 0, sizeof(boot_info_t));

    UINTN mmap_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    bs->GetMemoryMap(&mmap_size, 0, &map_key, &desc_size, &desc_version);
    mmap_size += desc_size * 2;

    void *mmap = 0;
    status = bs->AllocatePool(EFI_LOADER_DATA, mmap_size, &mmap);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] AllocatePool(mmap) failed: ", status);
        return status;
    }

    status = bs->GetMemoryMap(&mmap_size, mmap, &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] GetMemoryMap failed: ", status);
        return status;
    }

    boot_info->memory_map = mmap;
    boot_info->memory_map_size = mmap_size;
    boot_info->memory_desc_size = desc_size;
    boot_info->memory_desc_version = desc_version;

    uefi_log(system, "[boot] ExitBootServices\n");
    status = bs->ExitBootServices(image, map_key);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] ExitBootServices failed: ", status);
        return status;
    }

    void (*kernel_entry)(boot_info_t *) = (void (*)(boot_info_t *))(UINTN)ehdr->e_entry;
    kernel_entry(boot_info);

    return EFI_SUCCESS;
}
