/* The memory-map buffer and the boot_info page run the ExitBootServices loop
 * sizes (src/boot/efi_loader_alloc.h), run against a fake firmware that counts
 * live allocations.
 *
 * Regression: 2026-11-26-boot-mmap-retry-leak -- on EFI_BUFFER_TOO_SMALL the
 * loader dropped the undersized pool buffer and allocated a larger one, and a
 * boot_info run that had to grow was replaced the same way. Neither old buffer
 * was freed, so each stayed allocated as EFI_LOADER_DATA at ExitBootServices
 * and the kernel kept it reserved for its whole lifetime.
 *
 * The fake's GetMemoryMap reports a map that grows by one descriptor per live
 * allocation, as a real firmware's does, so a retry that leaks keeps the map
 * growing and the leak is visible both in the live count and in the map size. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "efi_loader_alloc.h"

#define DESC ((UINTN)48)
#define BASE_DESCRIPTORS ((UINTN)20)

typedef struct {
    void* ptr;
    UINTN bytes;
} pool_t;

static pool_t g_pool[16];
static int g_live_pool;
static int g_live_page_runs;
static UINTN g_live_pages;
static UINT64 g_next_page_base;
static int g_extra_descriptors;
static int g_get_map_calls;

static UINTN map_bytes_now(void) {
    return (BASE_DESCRIPTORS + (UINTN)g_extra_descriptors + (UINTN)g_live_pool +
            (UINTN)g_live_page_runs) *
           DESC;
}

static EFI_STATUS EFIAPI fake_allocate_pool(UINTN type, UINTN bytes, void** out) {
    assert(type == EFI_LOADER_DATA);
    for (size_t i = 0; i < sizeof(g_pool) / sizeof(g_pool[0]); ++i) {
        if (!g_pool[i].ptr) {
            g_pool[i].ptr = malloc(bytes);
            g_pool[i].bytes = bytes;
            ++g_live_pool;
            *out = g_pool[i].ptr;
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI fake_free_pool(void* ptr) {
    for (size_t i = 0; i < sizeof(g_pool) / sizeof(g_pool[0]); ++i) {
        if (g_pool[i].ptr == ptr) {
            free(ptr);
            g_pool[i].ptr = 0;
            --g_live_pool;
            return EFI_SUCCESS;
        }
    }
    assert(!"FreePool of a buffer the firmware never handed out");
    return EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI fake_get_memory_map(UINTN* size, void* map, UINTN* key, UINTN* desc_size,
                                             UINT32* version) {
    ++g_get_map_calls;
    UINTN need = map_bytes_now();
    *desc_size = DESC;
    *version = 1;
    if (!map || *size < need) {
        *size = need;
        return EFI_BUFFER_TOO_SMALL;
    }
    for (size_t i = 0; i < sizeof(g_pool) / sizeof(g_pool[0]); ++i) {
        if (g_pool[i].ptr == map) {
            assert(g_pool[i].bytes >= *size);
        }
    }
    memset(map, 0x5A, need);
    *size = need;
    *key = (UINTN)g_get_map_calls;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fake_allocate_pages(UINTN type, UINTN mem_type, UINTN pages,
                                             UINT64* base) {
    assert(type == EFI_ALLOCATE_ANY_PAGES);
    assert(mem_type == EFI_LOADER_DATA);
    *base = g_next_page_base;
    g_next_page_base += pages * 0x1000u;
    ++g_live_page_runs;
    g_live_pages += pages;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fake_free_pages(UINT64 base, UINTN pages) {
    assert(base != 0);
    assert(g_live_pages >= pages);
    --g_live_page_runs;
    g_live_pages -= pages;
    return EFI_SUCCESS;
}

static EFI_BOOT_SERVICES g_bs;

static void firmware_reset(void) {
    for (size_t i = 0; i < sizeof(g_pool) / sizeof(g_pool[0]); ++i) {
        free(g_pool[i].ptr);
    }
    memset(g_pool, 0, sizeof(g_pool));
    g_live_pool = 0;
    g_live_page_runs = 0;
    g_live_pages = 0;
    g_next_page_base = 0x40000000ULL;
    g_extra_descriptors = 0;
    g_get_map_calls = 0;
    memset(&g_bs, 0, sizeof(g_bs));
    g_bs.AllocatePool = fake_allocate_pool;
    g_bs.FreePool = fake_free_pool;
    g_bs.GetMemoryMap = fake_get_memory_map;
    g_bs.AllocatePages = fake_allocate_pages;
    g_bs.FreePages = fake_free_pages;
}

typedef struct {
    void* map;
    UINTN capacity;
    UINTN size;
    UINTN key;
    UINTN desc_size;
    UINT32 desc_version;
} map_state_t;

static EFI_STATUS read_map(map_state_t* m) {
    return efi_read_memory_map(
        &g_bs, &m->map, &m->capacity, &m->size, &m->key, &m->desc_size, &m->desc_version);
}

static void test_a_buffer_that_fits_is_read_in_one_call(void) {
    firmware_reset();
    map_state_t m = {0};
    m.capacity = 64 * DESC;
    assert(read_map(&m) == EFI_SUCCESS);
    assert(m.map != 0);
    assert(g_get_map_calls == 1);
    assert(g_live_pool == 1);
    assert(m.size == map_bytes_now());
}

static void test_a_too_small_buffer_is_replaced_not_leaked(void) {
    firmware_reset();
    map_state_t m = {0};
    m.capacity = DESC;
    assert(read_map(&m) == EFI_SUCCESS);
    assert(g_get_map_calls == 2);
    assert(g_live_pool == 1);
    assert(m.size == map_bytes_now());
    assert(m.capacity >= m.size);
}

static void test_a_map_that_grows_between_reads_leaves_one_buffer(void) {
    firmware_reset();
    map_state_t m = {0};
    m.capacity = 64 * DESC;
    assert(read_map(&m) == EFI_SUCCESS);
    for (int round = 0; round < 6; ++round) {
        /* Something between the read and ExitBootServices grew the map past
         * the buffer, so the next pass of the loop sees BUFFER_TOO_SMALL. */
        g_extra_descriptors += 64;
        assert(read_map(&m) == EFI_SUCCESS);
        assert(g_live_pool == 1);
    }
    assert(m.size == (BASE_DESCRIPTORS + 6u * 64u + 1u) * DESC);
}

static void test_a_boot_info_run_that_fits_is_kept(void) {
    firmware_reset();
    UINT64 base = 0;
    UINTN capacity = 0;
    assert(efi_reserve_loader_pages(&g_bs, &base, &capacity, 8) == EFI_SUCCESS);
    UINT64 first = base;
    assert(efi_reserve_loader_pages(&g_bs, &base, &capacity, 5) == EFI_SUCCESS);
    assert(base == first);
    assert(capacity == 8);
    assert(g_live_page_runs == 1);
    assert(g_live_pages == 8);
}

static void test_a_boot_info_run_that_grows_frees_the_old_run(void) {
    firmware_reset();
    UINT64 base = 0;
    UINTN capacity = 0;
    assert(efi_reserve_loader_pages(&g_bs, &base, &capacity, 8) == EFI_SUCCESS);
    assert(efi_reserve_loader_pages(&g_bs, &base, &capacity, 9) == EFI_SUCCESS);
    assert(efi_reserve_loader_pages(&g_bs, &base, &capacity, 12) == EFI_SUCCESS);
    assert(capacity == 12);
    assert(g_live_page_runs == 1);
    assert(g_live_pages == 12);
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_buffer_that_fits_is_read_in_one_call),
        WASMOS_TEST_CASE(test_a_too_small_buffer_is_replaced_not_leaked),
        WASMOS_TEST_CASE(test_a_map_that_grows_between_reads_leaves_one_buffer),
        WASMOS_TEST_CASE(test_a_boot_info_run_that_fits_is_kept),
        WASMOS_TEST_CASE(test_a_boot_info_run_that_grows_frees_the_old_run),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    firmware_reset();
    printf("test_efi_loader_alloc: ok\n");
    return 0;
}
