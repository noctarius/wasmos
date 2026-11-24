/* Host unit test for region-addressed register access (src/kernel/io_region.c),
 * the one access path behind the io_region_* host calls of both wasm runtimes
 * and the native driver API.
 *
 * Built against tests/unit/shadow_noports, so the I/O-port accessors are
 * aarch64's: an architecture without a port space, where every port access
 * fails. A memory-mapped window points at a host array.
 *
 * Each case returns 0 to pass or __LINE__ to fail (test_shuffle.h). */
#include "io_region.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "capability.h"
#include "test_shuffle.h"

/* capability.c matches manifest names with this; the kernel's definition lives
 * in libc.c, which a host binary cannot link. */
int str_eq_bytes(const uint8_t* bytes, size_t bytes_len, const char* lit) {
    size_t i = 0;
    if (!bytes || !lit) {
        return 0;
    }
    while (lit[i]) {
        if (i >= bytes_len || bytes[i] != (uint8_t)lit[i]) {
            return 0;
        }
        i++;
    }
    return i == bytes_len;
}

#define DRIVER 70u
#define UNTOUCHED 0xA5A5A5A5u

static uint32_t g_regs[4];

/* Region 0: eight I/O ports. Region 1: a 16-byte register window over g_regs. */
static int install_profile(void) {
    const capability_region_t regions[2] = {
        {WASMOS_REGION_GRANT_IO, 8, 0x1F0, 0},
        {WASMOS_REGION_GRANT_MMIO, sizeof(g_regs), 0xFEB00000u, (uint64_t)(uintptr_t)g_regs},
    };
    capability_init();
    return capability_set_spawn_profile(
        DRIVER, DEVMGR_CAP_IO_PORT | DEVMGR_CAP_MMIO_MAP, 2, regions, 0, 0, 0, 0, 0, 0);
}

static int test_a_register_window_reads_zero_extended(void) {
    uint32_t v = UNTOUCHED;
    if (install_profile() != 0) {
        return __LINE__;
    }
    g_regs[1] = 0x89ABCDEFu;
    if (io_region_read(DRIVER, 1, 4, 4, &v) != 0 || v != 0x89ABCDEFu) {
        return __LINE__;
    }
    v = UNTOUCHED;
    if (io_region_read(DRIVER, 1, 4, 1, &v) != 0 || v != 0xEFu) {
        return __LINE__;
    }
    return 0;
}

static int test_a_refused_read_leaves_the_output_alone(void) {
    uint32_t v = UNTOUCHED;
    if (install_profile() != 0) {
        return __LINE__;
    }
    if (io_region_read(DRIVER, 2, 0, 4, &v) != WASMOS_ERR_IO_BAD_REGION || v != UNTOUCHED) {
        return __LINE__;
    }
    if (io_region_read(DRIVER, 1, 2, 4, &v) != WASMOS_ERR_IO_MISALIGNED || v != UNTOUCHED) {
        return __LINE__;
    }
    return 0;
}

/* Regression: 2026-11-24-io-region-read-clobbers-output -- io_region_read let
 * the port accessor write into the caller's output before reporting that the
 * architecture has no port space, so a failed read on aarch64 overwrote it with
 * zero. The contract (io_region.h, wasmos_native_driver.h) is that a failed
 * read leaves it untouched, and a native driver that reads into a value it
 * still holds lost that value. */
static int test_a_port_read_without_port_space_leaves_the_output_alone(void) {
    uint32_t v = UNTOUCHED;
    if (install_profile() != 0) {
        return __LINE__;
    }
    if (io_region_read(DRIVER, 0, 0, 1, &v) != WASMOS_ERR_IO_NO_PORT_SPACE || v != UNTOUCHED) {
        return __LINE__;
    }
    if (io_region_read(DRIVER, 0, 0, 2, &v) != WASMOS_ERR_IO_NO_PORT_SPACE || v != UNTOUCHED) {
        return __LINE__;
    }
    if (io_region_read(DRIVER, 0, 4, 4, &v) != WASMOS_ERR_IO_NO_PORT_SPACE || v != UNTOUCHED) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_register_window_reads_zero_extended),
        WASMOS_TEST_CASE(test_a_refused_read_leaves_the_output_alone),
        WASMOS_TEST_CASE(test_a_port_read_without_port_space_leaves_the_output_alone),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    printf("test_io_region: ok\n");
    return 0;
}
