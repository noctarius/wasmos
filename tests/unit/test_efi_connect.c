/* The loader's ConnectController passes (src/boot/efi_connect.h), run against a
 * fake firmware that publishes handles under the protocol GUIDs the UEFI 2.10
 * spec assigns.
 *
 * Regression: 2026-11-26-boot-pci-guids -- neither PCI protocol GUID the loader
 * looked up was the spec value, so LocateHandleBuffer found no handle for either
 * and both PCI connect passes were silent no-ops. A firmware whose boot manager
 * leaves a graphics controller unconnected therefore produced no GOP for the
 * loader to find, and the boot continued without a framebuffer.
 *
 * The fake's GUIDs are written here from the spec text, independently of
 * uefi.h, so a wrong initialiser there cannot also make the fake agree with it. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "efi_connect.h"

/* UEFI 2.10 §14.4: 4CF5B200-68B8-4CA5-9EEC-B23E3F50029A. */
static const EFI_GUID k_spec_pci_io = {
    0x4cf5b200, 0x68b8, 0x4ca5, {0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a}};
/* UEFI 2.10 §14.2: 2F707EBB-4A1A-11D4-9A38-0090273FC14D. */
static const EFI_GUID k_spec_root_bridge = {
    0x2f707ebb, 0x4a1a, 0x11d4, {0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
/* UEFI 2.10 §12.4: 387477C2-69C7-11D2-8E39-00A0C969723B. */
static const EFI_GUID k_spec_text_output = {
    0x387477c2, 0x69c7, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

/* Handle values are distinct addresses the fake never dereferences. */
static char g_handle_storage[8];
#define H(i) ((EFI_HANDLE) & g_handle_storage[(i)])

typedef struct {
    const EFI_GUID* guid;
    EFI_HANDLE handles[4];
    UINTN count;
} published_t;

static published_t g_published[3];
static EFI_HANDLE g_connected[16];
static int g_connected_count;
static int g_live_pool_buffers;

static int guid_equal(const EFI_GUID* a, const EFI_GUID* b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

static EFI_STATUS EFIAPI fake_locate_handle_buffer(EFI_LOCATE_SEARCH_TYPE type,
                                                   const EFI_GUID* protocol, void* key,
                                                   UINTN* count, EFI_HANDLE** buffer) {
    (void)key;
    assert(type == EFI_LOCATE_SEARCH_TYPE_BY_PROTOCOL);
    for (size_t i = 0; i < sizeof(g_published) / sizeof(g_published[0]); ++i) {
        const published_t* p = &g_published[i];
        if (p->guid && p->count > 0 && guid_equal(p->guid, protocol)) {
            EFI_HANDLE* out = malloc(p->count * sizeof(EFI_HANDLE));
            memcpy(out, p->handles, p->count * sizeof(EFI_HANDLE));
            *count = p->count;
            *buffer = out;
            ++g_live_pool_buffers;
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI fake_connect_controller(EFI_HANDLE controller, EFI_HANDLE* driver,
                                                 void* remaining, BOOLEAN recursive) {
    (void)driver;
    (void)remaining;
    assert(recursive == TRUE);
    assert(g_connected_count < (int)(sizeof(g_connected) / sizeof(g_connected[0])));
    g_connected[g_connected_count++] = controller;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fake_free_pool(void* buffer) {
    free(buffer);
    --g_live_pool_buffers;
    return EFI_SUCCESS;
}

static EFI_BOOT_SERVICES g_bs;

static void firmware_reset(void) {
    memset(g_published, 0, sizeof(g_published));
    memset(g_connected, 0, sizeof(g_connected));
    g_connected_count = 0;
    g_live_pool_buffers = 0;
    memset(&g_bs, 0, sizeof(g_bs));
    g_bs.LocateHandleBuffer = (void*)fake_locate_handle_buffer;
    g_bs.ConnectController = (void*)fake_connect_controller;
    g_bs.FreePool = fake_free_pool;
}

static int was_connected(EFI_HANDLE h) {
    for (int i = 0; i < g_connected_count; ++i) {
        if (g_connected[i] == h) {
            return 1;
        }
    }
    return 0;
}

static void test_every_pci_device_is_connected(void) {
    firmware_reset();
    g_published[0] = (published_t){&k_spec_pci_io, {H(0), H(1), H(2)}, 3};
    efi_connect_graphics_controllers(&g_bs);
    assert(was_connected(H(0)));
    assert(was_connected(H(1)));
    assert(was_connected(H(2)));
}

static void test_every_pci_root_bridge_is_connected(void) {
    firmware_reset();
    g_published[0] = (published_t){&k_spec_root_bridge, {H(3), H(4)}, 2};
    efi_connect_graphics_controllers(&g_bs);
    assert(was_connected(H(3)));
    assert(was_connected(H(4)));
}

static void test_text_output_handles_are_connected(void) {
    firmware_reset();
    g_published[0] = (published_t){&k_spec_text_output, {H(5)}, 1};
    efi_connect_graphics_controllers(&g_bs);
    assert(was_connected(H(5)));
}

static void test_all_three_passes_run_and_free_their_handle_buffers(void) {
    firmware_reset();
    g_published[0] = (published_t){&k_spec_text_output, {H(5)}, 1};
    g_published[1] = (published_t){&k_spec_pci_io, {H(0), H(1)}, 2};
    g_published[2] = (published_t){&k_spec_root_bridge, {H(3)}, 1};
    efi_connect_graphics_controllers(&g_bs);
    assert(g_connected_count == 4);
    assert(g_live_pool_buffers == 0);
}

static void test_a_firmware_without_pci_connects_nothing(void) {
    firmware_reset();
    efi_connect_graphics_controllers(&g_bs);
    assert(g_connected_count == 0);
    assert(g_live_pool_buffers == 0);
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_every_pci_device_is_connected),
        WASMOS_TEST_CASE(test_every_pci_root_bridge_is_connected),
        WASMOS_TEST_CASE(test_text_output_handles_are_connected),
        WASMOS_TEST_CASE(test_all_three_passes_run_and_free_their_handle_buffers),
        WASMOS_TEST_CASE(test_a_firmware_without_pci_connects_nothing),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_efi_connect: ok\n");
    return 0;
}
