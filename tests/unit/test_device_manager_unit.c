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

/* A single port window becomes the descriptor's one I/O region grant; declared
 * ranges replace it. The interrupts follow the regions (and DMA windows). */
static void test_caps_encode_windows_and_irq(void) {
    uint8_t out[SUPERVISOR_CAPS_MAX];
    wasmos_spawn_caps_v2_t hdr;
    wasmos_region_grant_t r[2];
    uint32_t irq = 0;
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_IO_PORT | DEVMGR_CAP_IRQ;
    caps.io_port_min = 0x70;
    caps.io_port_max = 0x71;
    caps.irq_count = 1;
    caps.irqs[0] = 8;
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) ==
           (int32_t)WASMOS_SPAWN_CAPS_V2_SIZE(1, 0, 1));
    memcpy(&hdr, out, sizeof(hdr));
    assert(hdr.region_count == 1 && hdr.irq_count == 1 && hdr.dma.window_count == 0);
    memcpy(r, out + sizeof(hdr), sizeof(r[0]));
    assert(r[0].kind == WASMOS_REGION_GRANT_IO && r[0].base == 0x70 && r[0].length == 2);
    memcpy(&irq, out + sizeof(hdr) + sizeof(r[0]), sizeof(irq));
    assert(irq == 8u);

    caps.region_count = 2;
    caps.regions[0] = (wasmos_region_grant_t){WASMOS_REGION_GRANT_IO, 1, 0x60};
    caps.regions[1] = (wasmos_region_grant_t){WASMOS_REGION_GRANT_MMIO, 0x4000, 0xFEB00000u};
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) ==
           (int32_t)WASMOS_SPAWN_CAPS_V2_SIZE(2, 0, 1));
    memcpy(r, out + sizeof(hdr), sizeof(r));
    assert(r[0].kind == WASMOS_REGION_GRANT_IO && r[0].base == 0x60 && r[0].length == 1);
    assert(r[1].kind == WASMOS_REGION_GRANT_MMIO && r[1].base == 0xFEB00000u &&
           r[1].length == 0x4000);
    memcpy(&irq, out + sizeof(hdr) + sizeof(r), sizeof(irq));
    assert(irq == 8u);
}

/* An interrupt identifier is carried whole, not as a mask bit: a GIC SPI
 * (aarch64 INTID 33, the PL011 on QEMU virt) is as expressible as an ISA line,
 * and a list beyond WASMOS_IRQ_GRANT_LIMIT is refused rather than cut. */
static void test_caps_encode_carries_an_interrupt_above_line_15(void) {
    uint8_t out[SUPERVISOR_CAPS_MAX];
    wasmos_spawn_caps_v2_t hdr;
    uint32_t irq = 0;
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_IRQ;
    caps.irq_count = 1;
    caps.irqs[0] = 33;
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) ==
           (int32_t)WASMOS_SPAWN_CAPS_V2_SIZE(0, 0, 1));
    memcpy(&hdr, out, sizeof(hdr));
    memcpy(&irq, out + sizeof(hdr), sizeof(irq));
    assert(hdr.irq_count == 1 && irq == 33u);
    caps.irq_count = WASMOS_IRQ_GRANT_LIMIT + 1u;
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) == -1);
}

/* Regression: 2026-11-22-devmgr-dma-budget-replaced -- every DMA grant the
 * device manager encoded carried a fixed 4096-byte budget, which the spawn
 * profile installs in place of the one the driver's manifest declares
 * (dma.buffer flags, in pages). virtio-rng, which pins an 8 KiB ring, failed
 * its first allocation with WASMOS_ERR_DMA_RANGE as soon as its real
 * capability set reached the spawn. */
static void test_a_dma_grant_carries_the_drivers_budget(void) {
    uint8_t out[128];
    wasmos_spawn_caps_v2_t hdr;
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_DMA;
    caps.dma_max_bytes = 4u * 4096u;
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) > 0);
    memcpy(&hdr, out, sizeof(hdr));
    assert(hdr.dma.max_bytes == 4u * 4096u);

    caps.dma_max_bytes = 0; /* no metadata: the single-page default */
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) > 0);
    memcpy(&hdr, out, sizeof(hdr));
    assert(hdr.dma.max_bytes == 4096u);
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
    assert(dm_caps_v2_encode(&caps, out, sizeof(out)) ==
           (int32_t)WASMOS_SPAWN_CAPS_V2_SIZE(0, 1, 0));
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

/* A driver with declared windows that also needs startup args gets both in one
 * request: the args a PCI driver finds its device by, and every window in
 * declaration order. The two used to be exclusive -- windows forced a spawn by
 * module index, which carries no args. */
static void test_a_watched_spawn_carries_args_and_every_window(void) {
    static uint8_t out[DM_WATCH_SPAWN_MAX];
    wasmos_spawn_watch_desc_t desc;
    wasmos_spawn_caps_v2_t hdr;
    wasmos_region_grant_t g[2];
    spawn_caps_t caps;
    const char* path = "/init/system/drivers/virtio_rng.wap";
    const char* args = "bus=0 dev=3 fn=0 irq=11";
    int32_t n;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_IO_PORT | DEVMGR_CAP_MMIO_MAP | DEVMGR_CAP_IRQ;
    caps.irq_count = 1;
    caps.irqs[0] = 11;
    caps.region_count = 2;
    caps.regions[0] = (wasmos_region_grant_t){WASMOS_REGION_GRANT_MMIO, 0x4000, 0xFEB00000u};
    caps.regions[1] = (wasmos_region_grant_t){WASMOS_REGION_GRANT_IO, 0x20, 0xC000};
    caps.pci_function_valid = 1;
    caps.pci_function = 3u << 3;

    n = dm_watch_spawn_encode(path, args, &caps, 42, out, sizeof(out));
    assert(n > 0);
    memcpy(&desc, out, sizeof(desc));
    assert(desc.version == SPAWN_WATCH_DESC_VERSION && desc.notify_endpoint == 42u &&
           desc.tty == 0u && desc.flags == 0u);
    assert(desc.path_len == strlen(path) && memcmp(out + desc.path_off, path, desc.path_len) == 0);
    assert(desc.args_len == strlen(args) && memcmp(out + desc.args_off, args, desc.args_len) == 0);
    assert(desc.caps_len == WASMOS_SPAWN_CAPS_V2_SIZE(2, 0, 1));
    assert((uint32_t)n == desc.caps_off + desc.caps_len);
    memcpy(&hdr, out + desc.caps_off, sizeof(hdr));
    assert(hdr.region_count == 2 && hdr.irq_count == 1 &&
           (hdr.cap_flags & DEVMGR_CAP_PCI_FUNCTION) != 0u && hdr.dma.pci_function == 0x18u);
    memcpy(g, out + desc.caps_off + sizeof(hdr), sizeof(g));
    assert(g[0].kind == WASMOS_REGION_GRANT_MMIO && g[0].base == 0xFEB00000u);
    assert(g[1].kind == WASMOS_REGION_GRANT_IO && g[1].base == 0xC000);
}

/* No args and no caps are both expressible; a missing or over-long path, or a
 * buffer too small, is refused rather than truncated. */
static void test_a_watched_spawn_request_is_refused_rather_than_cut(void) {
    static uint8_t out[DM_WATCH_SPAWN_MAX];
    static char long_path[SPAWN_WATCH_PATH_MAX + 2];
    wasmos_spawn_watch_desc_t desc;
    int32_t n = dm_watch_spawn_encode("/init/a.wap", 0, 0, 7, out, sizeof(out));
    assert(n == (int32_t)(sizeof(desc) + strlen("/init/a.wap")));
    memcpy(&desc, out, sizeof(desc));
    assert(desc.args_len == 0u && desc.caps_len == 0u);
    assert(dm_watch_spawn_encode("", 0, 0, 7, out, sizeof(out)) == -1);
    assert(dm_watch_spawn_encode(0, 0, 0, 7, out, sizeof(out)) == -1);
    memset(long_path, 'a', sizeof(long_path) - 1u);
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1u] = '\0';
    assert(dm_watch_spawn_encode(long_path, 0, 0, 7, out, sizeof(out)) == -1);
    assert(dm_watch_spawn_encode("/init/a.wap", 0, 0, 7, out, (uint32_t)sizeof(desc)) == -1);
}

/* An ACPI device record as registry_add_from_acpi_desc leaves it. */
static pci_device_record_t acpi_record(uint32_t irq) {
    pci_device_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.bus = 0xFFu;
    rec.irq_hint = irq;
    return rec;
}

/* A PL011 is one memory window and a GIC SPI: an MMIO grant rounded out to
 * whole pages, and the interrupt identifier above the ISA range. */
static void test_an_acpi_mmio_window_is_granted_in_whole_pages(void) {
    pci_device_record_t rec = acpi_record(33u);
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    rec.bars[0].kind = WASMOS_PCI_BAR_MEM64;
    rec.bars[0].base = 0x09000010u;
    rec.bars[0].size = 0x20u;
    dm_acpi_caps(&rec, &caps);
    assert(caps.cap_flags == (DEVMGR_CAP_MMIO_MAP | DEVMGR_CAP_IRQ));
    assert(caps.region_count == 1u);
    assert(caps.regions[0].kind == WASMOS_REGION_GRANT_MMIO);
    assert(caps.regions[0].base == 0x09000000u);
    assert(caps.regions[0].length == 0x1000u);
    assert(caps.irq_count == 1u && caps.irqs[0] == 33u);
}

/* The i8042 lists its data and command ports as two windows; both are granted,
 * in the firmware's order, and nothing between them. */
static void test_acpi_io_windows_are_granted_in_order(void) {
    pci_device_record_t rec = acpi_record(1u);
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    rec.bars[0].kind = WASMOS_PCI_BAR_IO;
    rec.bars[0].base = 0x60u;
    rec.bars[0].size = 1u;
    rec.bars[1].kind = WASMOS_PCI_BAR_IO;
    rec.bars[1].base = 0x64u;
    rec.bars[1].size = 1u;
    dm_acpi_caps(&rec, &caps);
    assert(caps.cap_flags == (DEVMGR_CAP_IO_PORT | DEVMGR_CAP_IRQ));
    assert(caps.region_count == 2u);
    assert(caps.regions[0].kind == WASMOS_REGION_GRANT_IO && caps.regions[0].base == 0x60u &&
           caps.regions[0].length == 1u);
    assert(caps.regions[1].kind == WASMOS_REGION_GRANT_IO && caps.regions[1].base == 0x64u &&
           caps.regions[1].length == 1u);
}

/* A device that names no interrupt is granted none, and no IRQ capability. */
static void test_an_acpi_device_without_an_interrupt_gets_none(void) {
    pci_device_record_t rec = acpi_record(DM_IRQ_HINT_NONE);
    spawn_caps_t caps;
    uint8_t out[SUPERVISOR_CAPS_MAX];
    memset(&caps, 0, sizeof(caps));
    rec.bars[0].kind = WASMOS_PCI_BAR_IO;
    rec.bars[0].base = 0x70u;
    rec.bars[0].size = 8u;
    dm_acpi_caps(&rec, &caps);
    assert(caps.cap_flags == DEVMGR_CAP_IO_PORT);
    assert(caps.irq_count == 0u);
    assert(dm_caps_v2_encode(&caps, out, (uint32_t)sizeof(out)) ==
           (int32_t)WASMOS_SPAWN_CAPS_V2_SIZE(1, 0, 0));
}

/* A 16550 at `io` as registry_add_from_acpi_desc leaves it. */
static pci_device_record_t acpi_uart(uint16_t io, uint32_t irq, uint32_t flags) {
    pci_device_record_t rec = acpi_record(irq);
    (void)snprintf(rec.hid, sizeof(rec.hid), "%s", "PNP0501");
    rec.bars[0].kind = WASMOS_PCI_BAR_IO;
    rec.bars[0].base = io;
    rec.bars[0].size = 8u;
    rec.io_port_base = io;
    rec.acpi_flags = flags;
    return rec;
}

/* Regression: 2026-11-23-console-selection -- when the firmware named a
 * console (SPCR), only that UART was told so; every other UART was spawned
 * with no word on it and fell back to its own default, so COM1 still claimed
 * the console's name beside the UART the firmware chose, and one of the two
 * registrations failed. A UART the firmware passed over must be told it is
 * not the console. */
static void test_a_uart_the_firmware_passed_over_is_told_it_is_not_the_console(void) {
    char out[96];
    pci_device_record_t com1 = acpi_uart(0x3F8u, 4u, WASMOS_ACPI_DEVICE_CONSOLE_KNOWN);
    pci_device_record_t com2 =
        acpi_uart(0x2F8u, 3u, WASMOS_ACPI_DEVICE_CONSOLE | WASMOS_ACPI_DEVICE_CONSOLE_KNOWN);
    assert(dm_acpi_spawn_args(&com1, out, sizeof(out)) > 0);
    assert(strcmp(out, "hid=PNP0501 io=3F8 irq=4 console=0") == 0);
    assert(dm_acpi_spawn_args(&com2, out, sizeof(out)) > 0);
    assert(strcmp(out, "hid=PNP0501 io=2F8 irq=3 console=1") == 0);
}

/* With no SPCR the firmware said nothing, and the driver keeps its default. */
static void test_without_a_firmware_console_no_console_arg_is_given(void) {
    char out[96];
    pci_device_record_t com1 = acpi_uart(0x3F8u, 4u, 0u);
    assert(dm_acpi_spawn_args(&com1, out, sizeof(out)) > 0);
    assert(strcmp(out, "hid=PNP0501 io=3F8 irq=4") == 0);
}

/* A PCI device record with an interrupt hint, or DM_IRQ_HINT_NONE. */
static pci_device_record_t pci_record(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                                      uint32_t irq) {
    pci_device_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.class_code = class_code;
    rec.subclass = subclass;
    rec.prog_if = prog_if;
    rec.irq_hint = irq;
    return rec;
}

/* Regression: 2026-11-23-pci-irq-default -- a PCI driver whose device record
 * carried no interrupt hint was granted ISA lines 14 and 15 on every
 * architecture. Those are legacy IDE's fixed lines, meaningful only for an IDE
 * controller whose channels run in compatibility mode; on aarch64 they are
 * not even routable interrupts (INTIDs below 32 are SGIs and PPIs), so every
 * virtio driver there was granted two identifiers that name nothing. */
static void test_a_pci_device_without_a_hint_is_granted_no_interrupt(void) {
    pci_device_record_t virtio = pci_record(0x01u, 0x00u, 0x00u, DM_IRQ_HINT_NONE);
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_IRQ;
    dm_pci_irqs(&virtio, &caps);
    assert(caps.irq_count == 0u);
}

/* PCI IDE in compatibility mode (prog_if bits 0 and 2 clear) decodes the ISA
 * channels and raises lines 14 and 15 whatever its interrupt line register
 * says (PCI IDE Controller Specification 1.0, 2.1). */
static void test_compatibility_mode_ide_is_granted_lines_14_and_15(void) {
    pci_device_record_t ide = pci_record(0x01u, 0x01u, 0x80u, DM_IRQ_HINT_NONE);
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_IRQ;
    dm_pci_irqs(&ide, &caps);
    assert(caps.irq_count == 2u && caps.irqs[0] == 14u && caps.irqs[1] == 15u);
}

/* Regression: 2026-11-23-ide-mixed-mode -- compatibility mode was treated as
 * all-or-nothing for the controller, so an IDE controller with one channel in
 * native mode and the other in compatibility mode, and no interrupt hint, was
 * granted no interrupt: the compatibility channel's fixed line was lost and its
 * driver could never take an interrupt. Each channel's mode is its own prog_if
 * bit (0 primary, 2 secondary). Raised on PR #71. */
static void test_each_compatibility_channel_is_granted_its_own_line(void) {
    pci_device_record_t primary_native = pci_record(0x01u, 0x01u, 0x81u, DM_IRQ_HINT_NONE);
    pci_device_record_t secondary_native = pci_record(0x01u, 0x01u, 0x84u, DM_IRQ_HINT_NONE);
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_IRQ;
    dm_pci_irqs(&primary_native, &caps);
    assert(caps.irq_count == 1u && caps.irqs[0] == 15u);
    dm_pci_irqs(&secondary_native, &caps);
    assert(caps.irq_count == 1u && caps.irqs[0] == 14u);
    /* The hint is the native channel's interrupt; the other still needs its line. */
    primary_native.irq_hint = 11u;
    dm_pci_irqs(&primary_native, &caps);
    assert(caps.irq_count == 2u && caps.irqs[0] == 11u && caps.irqs[1] == 15u);
}

/* A hint is the device's interrupt; native-mode IDE is an ordinary PCI device. */
static void test_a_hint_is_the_grant(void) {
    pci_device_record_t native_ide = pci_record(0x01u, 0x01u, 0x8Fu, 11u);
    spawn_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.cap_flags = DEVMGR_CAP_IRQ;
    dm_pci_irqs(&native_ide, &caps);
    assert(caps.irq_count == 1u && caps.irqs[0] == 11u);
    caps.cap_flags = 0u; /* no interrupt without the capability */
    dm_pci_irqs(&native_ide, &caps);
    assert(caps.irq_count == 0u);
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_spec_names_the_device_and_its_policy),
        WASMOS_TEST_CASE(test_a_spec_that_does_not_fit_is_refused),
        WASMOS_TEST_CASE(test_caps_encode_windows_and_irq),
        WASMOS_TEST_CASE(test_caps_encode_carries_an_interrupt_above_line_15),
        WASMOS_TEST_CASE(test_caps_encode_dma_and_bounds),
        WASMOS_TEST_CASE(test_caps_encode_the_pci_function),
        WASMOS_TEST_CASE(test_a_dma_grant_carries_the_drivers_budget),
        WASMOS_TEST_CASE(test_declared_regions_keep_their_index),
        WASMOS_TEST_CASE(test_an_absent_bar_refuses_the_grant),
        WASMOS_TEST_CASE(test_bars_resolve_to_grants_of_their_kind),
        WASMOS_TEST_CASE(test_a_watched_spawn_carries_args_and_every_window),
        WASMOS_TEST_CASE(test_a_watched_spawn_request_is_refused_rather_than_cut),
        WASMOS_TEST_CASE(test_an_acpi_mmio_window_is_granted_in_whole_pages),
        WASMOS_TEST_CASE(test_acpi_io_windows_are_granted_in_order),
        WASMOS_TEST_CASE(test_an_acpi_device_without_an_interrupt_gets_none),
        WASMOS_TEST_CASE(test_a_uart_the_firmware_passed_over_is_told_it_is_not_the_console),
        WASMOS_TEST_CASE(test_without_a_firmware_console_no_console_arg_is_given),
        WASMOS_TEST_CASE(test_a_pci_device_without_a_hint_is_granted_no_interrupt),
        WASMOS_TEST_CASE(test_compatibility_mode_ide_is_granted_lines_14_and_15),
        WASMOS_TEST_CASE(test_each_compatibility_channel_is_granted_its_own_line),
        WASMOS_TEST_CASE(test_a_hint_is_the_grant),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_device_manager_unit: ok\n");
    return 0;
}
