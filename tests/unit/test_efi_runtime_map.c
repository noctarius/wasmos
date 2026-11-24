/* The virtual map the loader hands SetVirtualAddressMap for the firmware clock
 * (src/boot/efi_runtime_map.h).
 *
 * Regression: 2026-11-24-efi-runtime-code-in-device-gib -- a runtime-code or
 * runtime-data region below 1 GiB passed the check, which only asked whether a
 * region ended inside the direct-map window. The first GiB of the aarch64
 * direct map is Device-nGnRE and never executable (paging_init, entry.S), so the
 * handover succeeded and the kernel's first GetTime took an instruction abort.
 * A refused map also left the VirtualStart fields of the regions before the
 * offending one rewritten, in the map copy that reaches the kernel.
 *
 * The function is pure over the descriptor array, so every case builds a map,
 * calls it, and checks the verdict and each VirtualStart. Descriptors are laid
 * out with a stride larger than sizeof(EFI_MEMORY_DESCRIPTOR), as firmware
 * reports, so a function that indexed by sizeof would read the padding. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "efi_runtime_map.h"

#define STRIDE 48u
#define GIB 0x40000000ULL

static UINT8 g_map[8 * STRIDE];
static UINTN g_count;

static void map_reset(void) {
    memset(g_map, 0xA5, sizeof(g_map));
    g_count = 0;
}

static void map_add(UINT32 type, UINT64 phys, UINT64 pages, int runtime) {
    EFI_MEMORY_DESCRIPTOR d;
    memset(&d, 0, sizeof(d));
    d.Type = type;
    d.PhysicalStart = phys;
    d.NumberOfPages = pages;
    d.Attribute = runtime ? EFI_MEMORY_RUNTIME : 0;
    memcpy(g_map + g_count * STRIDE, &d, sizeof(d));
    ++g_count;
}

static UINT64 virt(UINTN i) {
    EFI_MEMORY_DESCRIPTOR d;
    memcpy(&d, g_map + i * STRIDE, sizeof(d));
    return d.VirtualStart;
}

static int assign(void) {
    return efi_runtime_map_assign(g_map, g_count * STRIDE, STRIDE);
}

static void test_ram_code_and_data_are_named_at_their_direct_map_address(void) {
    map_reset();
    map_add(EFI_RUNTIME_SERVICES_CODE, 0x5F000000ULL, 16, 1);
    map_add(EFI_RUNTIME_SERVICES_DATA, 0x5F100000ULL, 8, 1);
    assert(assign() == 1);
    assert(virt(0) == (0x5F000000ULL | KERNEL_HIGHER_HALF_BASE));
    assert(virt(1) == (0x5F100000ULL | KERNEL_HIGHER_HALF_BASE));
}

static void test_runtime_mmio_in_the_device_gib_is_named(void) {
    /* The PL031 and the flash are device registers, and Device memory is
     * what they must be reached through. */
    map_reset();
    map_add(EFI_MEMORY_MAPPED_IO, 0x09010000ULL, 1, 1);
    map_add(EFI_MEMORY_MAPPED_IO, 0x04000000ULL, 0x1000, 1);
    map_add(EFI_RUNTIME_SERVICES_CODE, 0x5F000000ULL, 16, 1);
    assert(assign() == 1);
    assert(virt(0) == (0x09010000ULL | KERNEL_HIGHER_HALF_BASE));
    assert(virt(1) == (0x04000000ULL | KERNEL_HIGHER_HALF_BASE));
}

static void test_runtime_code_in_the_device_gib_is_refused(void) {
    map_reset();
    map_add(EFI_RUNTIME_SERVICES_CODE, 0x0E000000ULL, 16, 1);
    assert(assign() == 0);
}

static void test_runtime_data_in_the_device_gib_is_refused(void) {
    /* Device memory faults an unaligned access, which firmware data sees. */
    map_reset();
    map_add(EFI_RUNTIME_SERVICES_DATA, GIB - 0x1000ULL, 2, 1);
    assert(assign() == 0);
}

static void test_a_region_past_the_window_is_refused(void) {
    map_reset();
    map_add(EFI_RUNTIME_SERVICES_CODE, KERNEL_HIGHER_HALF_WINDOW_BYTES - 0x1000ULL, 2, 1);
    assert(assign() == 0);
}

static void test_a_wrapping_region_is_refused(void) {
    map_reset();
    map_add(EFI_RUNTIME_SERVICES_CODE, 0x5F000000ULL, 0x000FFFFFFFFFFFFFULL, 1);
    assert(assign() == 0);
}

static void test_a_refused_map_is_left_unchanged(void) {
    map_reset();
    map_add(EFI_RUNTIME_SERVICES_CODE, 0x5F000000ULL, 16, 1);
    map_add(EFI_RUNTIME_SERVICES_CODE, 0x0E000000ULL, 16, 1);
    assert(assign() == 0);
    assert(virt(0) == 0);
    assert(virt(1) == 0);
}

static void test_regions_without_the_runtime_bit_are_ignored(void) {
    map_reset();
    map_add(EFI_RUNTIME_SERVICES_CODE, 0x0E000000ULL, 16, 0);
    map_add(7u, 8 * GIB, 16, 0);
    map_add(EFI_RUNTIME_SERVICES_CODE, 0x5F000000ULL, 16, 1);
    assert(assign() == 1);
    assert(virt(0) == 0);
    assert(virt(1) == 0);
    assert(virt(2) == (0x5F000000ULL | KERNEL_HIGHER_HALF_BASE));
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_ram_code_and_data_are_named_at_their_direct_map_address),
        WASMOS_TEST_CASE(test_runtime_mmio_in_the_device_gib_is_named),
        WASMOS_TEST_CASE(test_runtime_code_in_the_device_gib_is_refused),
        WASMOS_TEST_CASE(test_runtime_data_in_the_device_gib_is_refused),
        WASMOS_TEST_CASE(test_a_region_past_the_window_is_refused),
        WASMOS_TEST_CASE(test_a_wrapping_region_is_refused),
        WASMOS_TEST_CASE(test_a_refused_map_is_left_unchanged),
        WASMOS_TEST_CASE(test_regions_without_the_runtime_bit_are_ignored),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_efi_runtime_map: ok\n");
    return 0;
}
