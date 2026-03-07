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

static int elf_is_valid(const Elf64_Ehdr *ehdr) {
    return ehdr->e_ident[0] == 0x7f &&
           ehdr->e_ident[1] == 'E' &&
           ehdr->e_ident[2] == 'L' &&
           ehdr->e_ident[3] == 'F';
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system) {
    EFI_BOOT_SERVICES *bs = system->BootServices;
    EFI_STATUS status;

    EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    status = bs->HandleProtocol(image, &lip_guid, (void **)&loaded);
    if (EFI_ERROR(status)) {
        return status;
    }

    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    status = bs->HandleProtocol(loaded->DeviceHandle, &fs_guid, (void **)&fs);
    if (EFI_ERROR(status)) {
        return status;
    }

    EFI_FILE_PROTOCOL *root = 0;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) {
        return status;
    }

    EFI_FILE_PROTOCOL *kernel = 0;
    static CHAR16 kernel_path[] = L"\\kernel.elf";
    status = root->Open(root, &kernel, kernel_path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        return status;
    }

    EFI_GUID file_info_guid = EFI_FILE_INFO_GUID;
    UINTN info_size = 0;
    EFI_FILE_INFO *info = 0;
    status = kernel->GetInfo(kernel, &file_info_guid, &info_size, info);
    if (status == EFI_BUFFER_TOO_SMALL) {
        status = bs->AllocatePool(EFI_LOADER_DATA, info_size, (void **)&info);
        if (EFI_ERROR(status)) {
            return status;
        }
        status = kernel->GetInfo(kernel, &file_info_guid, &info_size, info);
    }
    if (EFI_ERROR(status)) {
        return status;
    }

    void *kernel_buf = 0;
    UINTN kernel_size = (UINTN)info->FileSize;
    status = bs->AllocatePool(EFI_LOADER_DATA, kernel_size, &kernel_buf);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = kernel->Read(kernel, &kernel_size, kernel_buf);
    if (EFI_ERROR(status)) {
        return status;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)kernel_buf;
    if (!elf_is_valid(ehdr)) {
        return 1;
    }

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
        return status;
    }

    status = bs->GetMemoryMap(&mmap_size, mmap, &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        return status;
    }

    boot_info->memory_map = mmap;
    boot_info->memory_map_size = mmap_size;
    boot_info->memory_desc_size = desc_size;
    boot_info->memory_desc_version = desc_version;

    status = bs->ExitBootServices(image, map_key);
    if (EFI_ERROR(status)) {
        return status;
    }

    void (*kernel_entry)(boot_info_t *) = (void (*)(boot_info_t *))(UINTN)ehdr->e_entry;
    kernel_entry(boot_info);

    return EFI_SUCCESS;
}
