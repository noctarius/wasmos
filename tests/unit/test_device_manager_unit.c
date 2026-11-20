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
    wasmos_io_range_t r[2];
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_IO_PORT | DEVMGR_CAP_IRQ;
    caps.io_port_min = 0x70;
    caps.io_port_max = 0x71;
    caps.irq_mask = 1u << 8;
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) == (int32_t)sizeof(hdr));
    memcpy(&hdr, out, sizeof(hdr));
    assert(hdr.io_port_min == 0x70 && hdr.io_port_max == 0x71 && hdr.irq_mask == (1u << 8) &&
           hdr.io_range_count == 0 && hdr.dma.window_count == 0);

    caps.io_range_count = 2;
    caps.io_ranges[0].first = caps.io_ranges[0].last = 0x60;
    caps.io_ranges[1].first = caps.io_ranges[1].last = 0x64;
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) == (int32_t)WASMOS_SPAWN_CAPS_V2_SIZE(2, 0));
    memcpy(r, out + sizeof(hdr), sizeof(r));
    assert(r[0].first == 0x60 && r[1].first == 0x64);
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

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_spec_names_the_device_and_its_policy),
        WASMOS_TEST_CASE(test_a_spec_that_does_not_fit_is_refused),
        WASMOS_TEST_CASE(test_caps_encode_windows_and_irq),
        WASMOS_TEST_CASE(test_caps_encode_dma_and_bounds),
        WASMOS_TEST_CASE(test_caps_encode_the_pci_function),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_device_manager_unit: ok\n");
    return 0;
}
