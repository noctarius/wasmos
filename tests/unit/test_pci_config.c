/* Host unit test for the kernel's PCI configuration access and the quiesce of a
 * dead driver's function (src/kernel/pci_config.c), over an emulated
 * mechanism-1 config space: outl to 0xCF8 latches the address, the data port
 * then reaches that function's register.
 *
 * The quiesce is what makes a DMA driver restartable: until it runs, a device
 * whose driver died keeps mastering into frames the reap is about to recycle,
 * and keeps signalling a vector that is about to go to someone else.
 *
 * Each case returns 0 to pass or __LINE__ to fail (test_shuffle.h). */
#include "pci_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "capability.h"
#include "test_shuffle.h"

#define CFG_FUNCTIONS 2u
#define BDF_DEV 0x0018u  /* 00:03.0 */
#define BDF_BARE 0x0020u /* 00:04.0: no capability list */

static uint8_t g_cfg[CFG_FUNCTIONS][256];
static uint16_t g_bdfs[CFG_FUNCTIONS] = {BDF_DEV, BDF_BARE};
static uint32_t g_address;
static uint32_t g_writes;

static uint8_t* cfg_of(uint16_t bdf) {
    for (uint32_t i = 0; i < CFG_FUNCTIONS; ++i) {
        if (g_bdfs[i] == bdf) {
            return g_cfg[i];
        }
    }
    return 0;
}

void outl(uint16_t port, uint32_t value) {
    if (port == 0xCF8u) {
        g_address = value;
        return;
    }
    uint8_t* cfg = cfg_of((uint16_t)((g_address >> 8) & 0xFFFFu));
    if (port == 0xCFCu && (g_address & 0x80000000u) != 0u && cfg) {
        memcpy(&cfg[g_address & 0xFCu], &value, 4);
        g_writes++;
    }
}

uint32_t inl(uint16_t port) {
    uint32_t value = 0xFFFFFFFFu;
    uint8_t* cfg = cfg_of((uint16_t)((g_address >> 8) & 0xFFFFu));
    if (port == 0xCFCu && (g_address & 0x80000000u) != 0u && cfg) {
        memcpy(&value, &cfg[g_address & 0xFCu], 4);
    }
    return value;
}

static void put32(uint8_t* cfg, uint8_t reg, uint32_t value) {
    memcpy(&cfg[reg], &value, 4);
}

static uint32_t get32(const uint8_t* cfg, uint8_t reg) {
    uint32_t value;
    memcpy(&value, &cfg[reg], 4);
    return value;
}

/* A bus-mastering device with MSI at 0x50 and MSI-X at 0x40, both enabled, its
 * status register carrying a write-1-to-clear error bit. */
static void reset_devices(void) {
    memset(g_cfg, 0, sizeof(g_cfg));
    g_writes = 0;
    uint8_t* dev = cfg_of(BDF_DEV);
    put32(dev, 0x00, 0x10051AF4u);               /* vendor 1AF4, device 1005 */
    put32(dev, 0x04, (0x2010u << 16) | 0x0007u); /* cap list + error; io, mem, BME */
    put32(dev, 0x34, 0x40u);                     /* first capability */
    put32(dev, 0x40, (0x8000u | 0x0003u) << 16 | 0x50u << 8 | 0x11u); /* MSI-X, enabled */
    put32(dev, 0x50, (0x0081u << 16) | 0x00u << 8 | 0x05u); /* MSI, 64-bit, enabled, last */
    uint8_t* bare = cfg_of(BDF_BARE);
    put32(bare, 0x00, 0x29228086u);
    put32(bare, 0x04, 0x0005u); /* io + BME, no capability list */
}

/* The address encoding pci-bus relies on through the host calls:
 * bus << 16 | device << 11 | function << 8 | register. */
static int test_an_access_reaches_its_functions_register(void) {
    reset_devices();
    if (pci_config_read32(BDF_DEV, 0x00) != 0x10051AF4u ||
        pci_config_read32(BDF_BARE, 0x00) != 0x29228086u) {
        return __LINE__;
    }
    if (pci_config_read32(0x0028u, 0x00) != 0xFFFFFFFFu) {
        return __LINE__; /* an absent function reads all-ones */
    }
    pci_config_write32(BDF_DEV, 0x3C, 0x0000010Bu);
    if (get32(cfg_of(BDF_DEV), 0x3C) != 0x0000010Bu) {
        return __LINE__;
    }
    return 0;
}

static int test_the_quiesce_stops_mastering_and_messages(void) {
    reset_devices();
    if (pci_quiesce_function(BDF_DEV) != 1) {
        return __LINE__;
    }
    const uint8_t* dev = cfg_of(BDF_DEV);
    uint32_t cmd = get32(dev, 0x04);
    if ((cmd & (1u << 2)) != 0u) {
        return __LINE__; /* bus mastering off */
    }
    if ((cmd & (1u << 10)) == 0u) {
        return __LINE__; /* INTx off, so it cannot fall back to the shared wire */
    }
    if ((cmd & 0x3u) != 0x3u) {
        return __LINE__; /* decoding left alone: the next driver reaches its BARs */
    }
    if ((cmd >> 16) != 0u) {
        return __LINE__; /* status written as zero: no write-1-to-clear bit is set */
    }
    uint32_t msix = get32(dev, 0x40);
    if ((msix >> 16) != 0x0003u || (msix & 0xFFFFu) != 0x5011u) {
        return __LINE__; /* MSI-X enable cleared, table size and header kept */
    }
    uint32_t msi = get32(dev, 0x50);
    if ((msi >> 16) != 0x0080u || (msi & 0xFFFFu) != 0x0005u) {
        return __LINE__; /* MSI enable cleared, 64-bit flag and header kept */
    }
    return 0;
}

static int test_a_function_without_capabilities_loses_only_mastering(void) {
    reset_devices();
    if (pci_quiesce_function(BDF_BARE) != 1) {
        return __LINE__;
    }
    if (get32(cfg_of(BDF_BARE), 0x04) != (0x0001u | (1u << 10))) {
        return __LINE__;
    }
    return 0;
}

static int test_an_absent_function_is_not_written(void) {
    reset_devices();
    if (pci_quiesce_function(0x0028u) != 0 || g_writes != 0u) {
        return __LINE__;
    }
    return 0;
}

/* A capability list that points at itself must not hang the reap. */
static int test_a_looping_capability_list_ends(void) {
    reset_devices();
    uint8_t* dev = cfg_of(BDF_DEV);
    put32(dev, 0x40, 0x4000u | 0x11u); /* MSI-X pointing back at itself */
    if (pci_quiesce_function(BDF_DEV) != 1) {
        return __LINE__;
    }
    return 0;
}

/* Contexts whose spawn profile named a function, in the stubbed table. */
#define CTX_WITH_DEVICE 7u
#define CTX_WITHOUT 8u

int capability_pci_function(uint32_t context_id, uint16_t* out_bdf) {
    if (context_id != CTX_WITH_DEVICE || !out_bdf) {
        return -1;
    }
    *out_bdf = BDF_DEV;
    return 0;
}

static int test_the_reap_quiesces_only_a_profiled_context(void) {
    reset_devices();
    pci_quiesce_context(CTX_WITHOUT, 40);
    if (g_writes != 0u) {
        return __LINE__;
    }
    pci_quiesce_context(CTX_WITH_DEVICE, 41);
    if ((get32(cfg_of(BDF_DEV), 0x04) & (1u << 2)) != 0u) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_an_access_reaches_its_functions_register),
        WASMOS_TEST_CASE(test_the_quiesce_stops_mastering_and_messages),
        WASMOS_TEST_CASE(test_a_function_without_capabilities_loses_only_mastering),
        WASMOS_TEST_CASE(test_an_absent_function_is_not_written),
        WASMOS_TEST_CASE(test_a_looping_capability_list_ends),
        WASMOS_TEST_CASE(test_the_reap_quiesces_only_a_profiled_context),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    printf("test_pci_config: ok\n");
    return 0;
}
