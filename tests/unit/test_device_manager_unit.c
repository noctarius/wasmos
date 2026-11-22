/* Host unit test for what the device manager hands the supervisor
 * (src/services/device_manager/device_manager_unit.c): the unit spec of a
 * driver whose ACPI or PCI rule names a restart policy, and the capability
 * descriptor that goes with it. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "device_manager_unit.h"
#include "wasmos_driver_abi.h"

#include "test_shuffle.h"

/* The unit is named for the device it drives, so a restart keeps the name; the
 * driver is loaded from the path it would be spawned from and gets the rule's
 * startup args. */
static void test_a_spec_names_the_device_and_its_policy(void) {
    char out[SUPERVISOR_SPEC_MAX + 1];
    int32_t n = dm_unit_spec("system/drivers/rtc.wap",
                             "acpi:0070",
                             "/boot/system/drivers/rtc.wap",
                             "on-failure",
                             "io=0070 irq=08",
                             out,
                             sizeof(out));
    assert(n > 0 && (uint32_t)n == strlen(out));
    assert(strcmp(out,
                  "rtc@acpi:0070 /boot/system/drivers/rtc.wap restart=on-failure -- "
                  "io=0070 irq=08") == 0);
    n = dm_unit_spec(
        "keyboard.wap", "acpi:0060", "/boot/keyboard.wap", "always", "", out, sizeof(out));
    assert(n > 0 && strcmp(out, "keyboard@acpi:0060 /boot/keyboard.wap restart=always") == 0);
    n = dm_unit_spec("system/drivers/virtio_rng.wap",
                     "pci:00:03.0",
                     "/init/system/drivers/virtio_rng.wap",
                     "on-failure",
                     0,
                     out,
                     sizeof(out));
    assert(n > 0 && strcmp(out,
                           "virtio_rng@pci:00:03.0 /init/system/drivers/virtio_rng.wap "
                           "restart=on-failure") == 0);
}

/* Refused rather than truncated: a cut name or path is a different unit. */
static void test_a_spec_that_does_not_fit_is_refused(void) {
    char out[SUPERVISOR_SPEC_MAX + 1];
    char small[24];
    const char* rtc = "system/drivers/rtc.wap";
    const char* path = "/boot/system/drivers/rtc.wap";
    assert(dm_unit_spec(rtc, "acpi:0070", path, "never", 0, small, sizeof(small)) == -1);
    assert(dm_unit_spec("system/drivers/a_driver_name_far_too_long_for_a_unit.wap",
                        "acpi:0070",
                        path,
                        "never",
                        0,
                        out,
                        sizeof(out)) == -1);
    assert(dm_unit_spec("/abs/rtc.wap", "acpi:0070", path, "never", 0, out, sizeof(out)) == -1);
    assert(dm_unit_spec(rtc, "acpi:0070", path, "", 0, out, sizeof(out)) == -1);
    assert(dm_unit_spec("system/drivers/.wap", "acpi:0070", path, "never", 0, out, sizeof(out)) ==
           -1);
    assert(dm_unit_spec(rtc, "", path, "never", 0, out, sizeof(out)) == -1);
    assert(dm_unit_spec(rtc, "acpi:0070", "relative.wap", "never", 0, out, sizeof(out)) == -1);
}

/* A single window travels in io_port_min/max, ranges after the header. */
static void test_caps_encode_windows_and_irq(void) {
    uint8_t out[SUPERVISOR_CAPS_MAX];
    wasmos_spawn_caps_v2_t hdr;
    wasmos_region_grant_t r[2];
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_IO_PORT | DEVMGR_CAP_IRQ;
    caps.io_port_min = 0x70;
    caps.io_port_max = 0x71;
    caps.irq_mask = 1u << 8;
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) == (int32_t)sizeof(hdr));
    memcpy(&hdr, out, sizeof(hdr));
    assert(hdr.io_port_min == 0x70 && hdr.io_port_max == 0x71 && hdr.irq_mask == (1u << 8) &&
           hdr.region_count == 0 && hdr.dma.window_count == 0);

    caps.region_count = 2;
    caps.regions[0] = (wasmos_region_grant_t){WASMOS_REGION_GRANT_IO, 1, 0x60};
    caps.regions[1] = (wasmos_region_grant_t){WASMOS_REGION_GRANT_MMIO, 0x4000, 0xFEB00000u};
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) == (int32_t)WASMOS_SPAWN_CAPS_V2_SIZE(2, 0));
    memcpy(r, out + sizeof(hdr), sizeof(r));
    assert(r[0].kind == WASMOS_REGION_GRANT_IO && r[0].base == 0x60 && r[0].length == 1);
    assert(r[1].kind == WASMOS_REGION_GRANT_MMIO && r[1].base == 0xFEB00000u &&
           r[1].length == 0x4000);
}

/* A PCI driver's descriptor names its function, which the kernel quiesces when
 * the driver ends; nothing else carries the flag. */
static void test_caps_encode_the_pci_function(void) {
    uint8_t out[SUPERVISOR_CAPS_MAX];
    wasmos_spawn_caps_v2_t hdr;
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_IO_PORT;
    caps.io_port_min = 0xC000;
    caps.io_port_max = 0xC01F;
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) > 0);
    memcpy(&hdr, out, sizeof(hdr));
    assert((hdr.cap_flags & DEVMGR_CAP_PCI_FUNCTION) == 0u && hdr.dma.pci_function == 0u);
    caps.pci_function_valid = 1;
    caps.pci_function = (0u << 8) | (3u << 3) | 0u;
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) > 0);
    memcpy(&hdr, out, sizeof(hdr));
    assert((hdr.cap_flags & DEVMGR_CAP_PCI_FUNCTION) != 0u && hdr.dma.pci_function == 0x18u);
}

/* DMA carries the default window; a buffer too small is refused. */
static void test_caps_encode_dma_and_bounds(void) {
    uint8_t out[128];
    wasmos_spawn_caps_v2_t hdr;
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_DMA;
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) == (int32_t)WASMOS_SPAWN_CAPS_V2_SIZE(0, 1));
    memcpy(&hdr, out, sizeof(hdr));
    assert(hdr.dma.window_count == 1u && hdr.dma.direction_flags == WASMOS_DMA_DIR_BIDIR);
    assert(dm_caps_v2_encode(&caps, out, (uint32_t)sizeof(hdr)) == -1);
}

/* Regression: 2026-11-22-devmgr-region-index-shift -- a declared BAR region the
 * device could not satisfy as an I/O window (a memory BAR, or a BAR the device
 * does not implement) was skipped, so every later region moved down one index.
 * The driver addresses its windows by declaration index, so its next access
 * landed in a different window than the one it named -- another register
 * block of the same device, or nothing -- instead of failing at spawn. */
static void test_declared_regions_keep_their_index(void) {
    wasmos_module_meta_desc_t meta;
    pci_device_record_t rec;
    spawn_caps_t caps;
    int rc;
    memset(&meta, 0, sizeof(meta));
    memset(&rec, 0, sizeof(rec));
    memset(&caps, 0, sizeof(caps));
    meta.region_count = 2;
    meta.regions[0].kind = WASMOS_APP_REGION_BAR;
    meta.regions[0].bar_index = 0;
    meta.regions[1].kind = WASMOS_APP_REGION_IO;
    meta.regions[1].first = 0x1F0;
    meta.regions[1].last = 0x1F7;
    rec.bars[0].kind = WASMOS_PCI_BAR_MEM32;
    rec.bars[0].base = 0xFEB00000u;
    rec.bars[0].size = 0x4000u;
    rc = dm_resolve_declared_regions(&meta, &rec, &caps);
    assert(rc != 0 || (caps.region_count == 2 && caps.regions[1].base == 0x1F0));
}

/* A memory BAR resolves to a memory-mapped grant of its measured size and an
 * I/O BAR to a port window, each at the index it was declared at. */
static void test_bars_resolve_to_grants_of_their_kind(void) {
    wasmos_module_meta_desc_t meta;
    pci_device_record_t rec;
    spawn_caps_t caps;
    memset(&meta, 0, sizeof(meta));
    memset(&rec, 0, sizeof(rec));
    memset(&caps, 0, sizeof(caps));
    meta.region_count = 3;
    meta.regions[0].kind = WASMOS_APP_REGION_BAR;
    meta.regions[0].bar_index = 4;
    meta.regions[1].kind = WASMOS_APP_REGION_BAR;
    meta.regions[1].bar_index = 0;
    meta.regions[2].kind = WASMOS_APP_REGION_IO;
    meta.regions[2].first = 0x3F6;
    meta.regions[2].last = 0x3F6;
    rec.bars[0].kind = WASMOS_PCI_BAR_IO;
    rec.bars[0].base = 0xC040u;
    rec.bars[0].size = 0x20u;
    rec.bars[4].kind = WASMOS_PCI_BAR_MEM64;
    rec.bars[4].base = 0x800000000ull;
    rec.bars[4].size = 0x4000u;
    assert(dm_resolve_declared_regions(&meta, &rec, &caps) == 0 && caps.region_count == 3);
    assert(caps.regions[0].kind == WASMOS_REGION_GRANT_MMIO &&
           caps.regions[0].base == 0x800000000ull && caps.regions[0].length == 0x4000u);
    assert(caps.regions[1].kind == WASMOS_REGION_GRANT_IO && caps.regions[1].base == 0xC040u &&
           caps.regions[1].length == 0x20u);
    assert(caps.regions[2].kind == WASMOS_REGION_GRANT_IO && caps.regions[2].base == 0x3F6u &&
           caps.regions[2].length == 1u);

    /* A memory BAR that does not fill whole pages cannot be mapped without
     * exposing its neighbours. */
    rec.bars[4].size = 0x100u;
    assert(dm_resolve_declared_regions(&meta, &rec, &caps) != 0 && caps.region_count == 0);
}

/* A BAR the device does not implement cannot be granted at all; the spawn is
 * refused rather than the driver getting a shorter window list than it
 * declared. */
static void test_an_absent_bar_refuses_the_grant(void) {
    wasmos_module_meta_desc_t meta;
    pci_device_record_t rec;
    spawn_caps_t caps;
    memset(&meta, 0, sizeof(meta));
    memset(&rec, 0, sizeof(rec));
    memset(&caps, 0, sizeof(caps));
    meta.region_count = 2;
    meta.regions[0].kind = WASMOS_APP_REGION_BAR;
    meta.regions[0].bar_index = 2;
    meta.regions[1].kind = WASMOS_APP_REGION_IO;
    meta.regions[1].first = 0x60;
    meta.regions[1].last = 0x64;
    assert(dm_resolve_declared_regions(&meta, &rec, &caps) != 0);
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_spec_names_the_device_and_its_policy),
        WASMOS_TEST_CASE(test_a_spec_that_does_not_fit_is_refused),
        WASMOS_TEST_CASE(test_caps_encode_windows_and_irq),
        WASMOS_TEST_CASE(test_caps_encode_dma_and_bounds),
        WASMOS_TEST_CASE(test_caps_encode_the_pci_function),
        WASMOS_TEST_CASE(test_declared_regions_keep_their_index),
        WASMOS_TEST_CASE(test_an_absent_bar_refuses_the_grant),
        WASMOS_TEST_CASE(test_bars_resolve_to_grants_of_their_kind),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_device_manager_unit: ok\n");
    return 0;
}
