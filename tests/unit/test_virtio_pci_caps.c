/* Host unit test for the virtio-modern capability walk
 * (src/libsys/wasm/include/wasmos/virtio_pci_caps.h) over an emulated config
 * space laid out the way QEMU's virtio-pci presents it: MSI-X first, then the
 * vendor-specific virtio capabilities, all structures in BAR 4.
 *
 * Each case returns 0 to pass or __LINE__ to fail (test_shuffle.h). */
#include "wasmos/virtio_pci_caps.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

static uint8_t g_cfg[256];
static int g_refuse;

static int32_t cfg_read(void* user, uint32_t reg, uint32_t* out) {
    (void)user;
    if (g_refuse || reg > 0xFCu || (reg & 3u) != 0u) {
        return -1;
    }
    memcpy(out, &g_cfg[reg], sizeof(*out));
    return 0;
}

/* One virtio_pci_cap at `at`, chaining to `next`. `extra` is the
 * notify_off_multiplier that follows a notify structure (virtio_pci_notify_cap);
 * the other structures are 16 bytes and have none. */
static void put_cap(uint8_t at, uint8_t next, uint8_t type, uint8_t bar, uint32_t offset,
                    uint32_t length, uint32_t extra) {
    g_cfg[at + 0] = 0x09u;
    g_cfg[at + 1] = next;
    g_cfg[at + 2] = type == 2u ? 20u : 16u;
    g_cfg[at + 3] = type;
    g_cfg[at + 4] = bar;
    memcpy(&g_cfg[at + 8], &offset, 4);
    memcpy(&g_cfg[at + 12], &length, 4);
    if (type == 2u) {
        memcpy(&g_cfg[at + 16], &extra, 4);
    }
}

static void qemu_layout(void) {
    memset(g_cfg, 0, sizeof(g_cfg));
    g_refuse = 0;
    g_cfg[0x06] = 0x10u; /* status: capability list */
    g_cfg[0x34] = 0x98u;
    g_cfg[0x98] = 0x11u; /* MSI-X */
    g_cfg[0x99] = 0x84u;
    put_cap(0x84, 0x74, 5u, 0u, 0, 0, 0); /* PCI-cfg access: unknown here, skipped */
    put_cap(0x74, 0x64, 4u, 4u, 0x3000, 0x1000, 0);
    put_cap(0x64, 0x50, 3u, 4u, 0x1000, 0x1000, 0);
    put_cap(0x50, 0x40, 2u, 4u, 0x2000, 0x1000, 4); /* 20 bytes: 0x50..0x63 */
    put_cap(0x40, 0x00, 1u, 4u, 0x0000, 0x1000, 0);
}

static int test_qemus_layout_is_found(void) {
    virtio_pci_layout_t l;
    qemu_layout();
    if (virtio_pci_find_layout(cfg_read, 0, &l) != 0) {
        return __LINE__;
    }
    if (l.common.bar != 4u || l.common.offset != 0u || l.common.length != 0x1000u ||
        l.isr.offset != 0x1000u || l.notify.offset != 0x2000u || l.device.offset != 0x3000u ||
        l.notify_off_multiplier != 4u) {
        return __LINE__;
    }
    if (l.found != ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4))) {
        return __LINE__;
    }
    return 0;
}

/* The first structure of a type wins; a later duplicate does not replace it. */
static int test_the_first_structure_of_a_type_wins(void) {
    virtio_pci_layout_t l;
    qemu_layout();
    g_cfg[0x41] = 0xB0u;
    put_cap(0xB0, 0x00, 1u, 2u, 0x8000, 0x100, 0);
    if (virtio_pci_find_layout(cfg_read, 0, &l) != 0 || l.common.bar != 4u ||
        l.common.offset != 0u) {
        return __LINE__;
    }
    return 0;
}

/* Without a common, notify or ISR structure the device cannot be driven. */
static int test_a_missing_structure_is_refused(void) {
    virtio_pci_layout_t l;
    qemu_layout();
    g_cfg[0x51] = 0x00u; /* the list ends at the notify structure: no common cfg */
    if (virtio_pci_find_layout(cfg_read, 0, &l) != -1) {
        return __LINE__;
    }
    return 0;
}

/* A legacy-only device, a refused read and a looping list all fail cleanly. */
static int test_no_list_a_refused_read_and_a_loop_fail(void) {
    virtio_pci_layout_t l;
    qemu_layout();
    g_cfg[0x06] = 0x00u;
    if (virtio_pci_find_layout(cfg_read, 0, &l) != -1) {
        return __LINE__;
    }
    qemu_layout();
    g_refuse = 1;
    if (virtio_pci_find_layout(cfg_read, 0, &l) != -1) {
        return __LINE__;
    }
    qemu_layout();
    g_cfg[0x65] = 0x74u;
    g_cfg[0x75] = 0x64u; /* 0x64 <-> 0x74, the common cfg is never reached */
    if (virtio_pci_find_layout(cfg_read, 0, &l) != -1) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_qemus_layout_is_found),
        WASMOS_TEST_CASE(test_the_first_structure_of_a_type_wins),
        WASMOS_TEST_CASE(test_a_missing_structure_is_refused),
        WASMOS_TEST_CASE(test_no_list_a_refused_read_and_a_loop_fail),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    printf("test_virtio_pci_caps: ok\n");
    return 0;
}
