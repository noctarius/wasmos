#include "uefi.h"
#include "elf.h"
#include "boot.h"

#define EFI_ALLOCATE_ANY_PAGES 0
#define EFI_ALLOCATE_ADDRESS 2
#define EFI_LOADER_DATA 2
#define EFI_BUFFER_TOO_SMALL ((EFI_STATUS)0x8000000000000005ULL)
#define EFI_INVALID_PARAMETER ((EFI_STATUS)0x8000000000000002ULL)

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

    static CHAR16 kernel_path[] = L"\\kernel.elf";
    void *kernel_buf = 0;
    UINTN kernel_size = 0;
    status = read_file_alloc(bs, root, kernel_path, &kernel_buf, &kernel_size);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] Read \\\\kernel.elf failed: ", status);
        return status;
    }

    static CHAR16 init_path[] = L"\\apps\\init.wasmosapp";
    void *init_buf = 0;
    UINTN init_size = 0;
    status = read_file_alloc(bs, root, init_path, &init_buf, &init_size);
    if (EFI_ERROR(status)) {
        uefi_log_status(system, "[boot] Read \\\\apps\\\\init.wasmosapp failed: ", status);
        return status;
    }
    uefi_log(system, "[boot] preloaded module: \\\\apps\\\\init.wasmosapp\n");

    static CHAR16 app_path[] = L"\\apps\\chardev_client.wasmosapp";
    void *app_buf = 0;
    UINTN app_size = 0;
    status = read_file_alloc(bs, root, app_path, &app_buf, &app_size);
    if (EFI_ERROR(status)) {
        app_buf = 0;
        app_size = 0;
        uefi_log(system, "[boot] optional module not found: \\\\apps\\\\chardev_client.wasmosapp\n");
    } else {
        uefi_log(system, "[boot] preloaded module: \\\\apps\\\\chardev_client.wasmosapp\n");
    }

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

    boot_info_t *boot_info = 0;
    UINTN mmap_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    bs->GetMemoryMap(&mmap_size, 0, &map_key, &desc_size, &desc_version);
    mmap_size += desc_size * 2;
    UINTN mmap_capacity = mmap_size;
    UINTN boot_capacity = 0;
    UINTN module_count = 0;
    if (init_buf && init_size > 0) {
        module_count++;
    }
    if (app_buf && app_size > 0) {
        module_count++;
    }
    UINTN module_table_bytes = module_count * sizeof(boot_module_t);
    UINTN module_data_bytes = init_size + app_size;

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
        UINTN total_bytes = boot_bytes + map_bytes + module_table_bytes + module_data_bytes;
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

        UINT8 *cursor = (UINT8 *)map_dst + map_bytes;
        if (module_count > 0) {
            boot_module_t *mods = (boot_module_t *)cursor;
            memset8(mods, 0, module_table_bytes);
            cursor += module_table_bytes;

            UINT32 mod_index = 0;
            if (init_buf && init_size > 0) {
                mods[mod_index].base = (UINT64)(UINTN)cursor;
                mods[mod_index].size = init_size;
                mods[mod_index].type = BOOT_MODULE_TYPE_WASMOS_APP;
                mods[mod_index].reserved = 0;
                copy_cstr(mods[mod_index].name, sizeof(mods[mod_index].name), "apps/init.wasmosapp");
                memcpy8(cursor, init_buf, init_size);
                cursor += init_size;
                mod_index++;
            }

            if (app_buf && app_size > 0) {
                mods[mod_index].base = (UINT64)(UINTN)cursor;
                mods[mod_index].size = app_size;
                mods[mod_index].type = BOOT_MODULE_TYPE_WASMOS_APP;
                mods[mod_index].reserved = 0;
                copy_cstr(mods[mod_index].name, sizeof(mods[mod_index].name), "apps/chardev_client.wasmosapp");
                memcpy8(cursor, app_buf, app_size);
                cursor += app_size;
                mod_index++;
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
