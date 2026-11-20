#include "pci_config.h"

#include "io.h"
#include "klog.h"
#include "sync/spinlock.h"
#include "wasmos_driver_abi.h"

#define PCI_CFG_ADDR_PORT 0xCF8u
#define PCI_CFG_DATA_PORT 0xCFCu

/* PCI Local Bus 3.0 §6.2 (command/status), §6.7 (capability list), §6.8 (MSI,
 * MSI-X). */
#define PCI_REG_ID 0x00u
#define PCI_REG_COMMAND 0x04u
#define PCI_REG_CAP_PTR 0x34u
#define PCI_CMD_BUS_MASTER (1u << 2)
#define PCI_CMD_INTX_DISABLE (1u << 10)
#define PCI_STATUS_CAP_LIST (1u << 4)
#define PCI_CAP_ID_MSI 0x05u
#define PCI_CAP_ID_MSIX 0x11u
#define PCI_MSI_CTRL_ENABLE (1u << 0)
#define PCI_MSIX_CTRL_ENABLE (1u << 15)
/* A malformed capability list can point at itself; 48 hops covers every
 * capability a 256-byte config space can hold. */
#define PCI_CAP_HOPS_MAX 48u

/* Serialises the address/data pair of every access. */
static ksync_spinlock_t g_pci_config_lock;

/* Functions a spawn profile named (pci_function_attach). Sized for every PCI
 * driver a system starts; a full table leaves the newest function unmanaged. */
#define PCI_FUNCTIONS_MAX 32u

typedef enum {
    PCI_FUNCTION_FREE = 0,
    PCI_FUNCTION_OWNED,
    PCI_FUNCTION_RETIRED
} pci_function_state_t;

typedef struct {
    uint8_t state; /* pci_function_state_t */
    uint16_t bdf;
    uint32_t owner_context; /* valid while OWNED */
} pci_function_t;

static pci_function_t g_pci_functions[PCI_FUNCTIONS_MAX];

/* Caller holds g_pci_config_lock. */
static pci_function_t* pci_function_by_bdf_locked(uint16_t bdf) {
    for (uint32_t i = 0; i < PCI_FUNCTIONS_MAX; ++i) {
        if (g_pci_functions[i].state != PCI_FUNCTION_FREE && g_pci_functions[i].bdf == bdf) {
            return &g_pci_functions[i];
        }
    }
    return 0;
}

static uint32_t pci_cfg_address(uint16_t bdf, uint8_t reg) {
    return 0x80000000u | ((uint32_t)bdf << 8) | ((uint32_t)reg & 0xFCu);
}

static uint32_t pci_cfg_read_locked(uint16_t bdf, uint8_t reg) {
    outl((uint16_t)PCI_CFG_ADDR_PORT, pci_cfg_address(bdf, reg));
    return inl((uint16_t)PCI_CFG_DATA_PORT);
}

static void pci_cfg_write_locked(uint16_t bdf, uint8_t reg, uint32_t value) {
    outl((uint16_t)PCI_CFG_ADDR_PORT, pci_cfg_address(bdf, reg));
    outl((uint16_t)PCI_CFG_DATA_PORT, value);
}

uint32_t pci_config_read32(uint16_t bdf, uint8_t reg) {
    ksync_spinlock_lock(&g_pci_config_lock);
    uint32_t value = pci_cfg_read_locked(bdf, reg);
    ksync_spinlock_unlock(&g_pci_config_lock);
    return value;
}

void pci_config_write32(uint16_t bdf, uint8_t reg, uint32_t value) {
    ksync_spinlock_lock(&g_pci_config_lock);
    pci_cfg_write_locked(bdf, reg, value);
    ksync_spinlock_unlock(&g_pci_config_lock);
}

/* Clears `enable` in the 16-bit message control word at cap + 2, keeping the
 * capability header in the low half. */
static void pci_cap_control_clear(uint16_t bdf, uint8_t cap, uint32_t enable) {
    uint32_t dword = pci_cfg_read_locked(bdf, cap);
    pci_cfg_write_locked(bdf, cap, dword & ~(enable << 16));
}

/* Caller holds g_pci_config_lock. */
static int pci_quiesce_function_locked(uint16_t bdf) {
    if ((pci_cfg_read_locked(bdf, PCI_REG_ID) & 0xFFFFu) == 0xFFFFu) {
        return 0;
    }
    /* INTx off as well: with MSI off the device would fall back to its pin,
     * and on a line shared with a live driver nobody deasserts it.
     * TODO: a driver that falls back to INTx after a restart finds INTx
     * disabled and bus mastering off; pci-bus re-enables both only through an
     * MSI bind, so such a driver gets no interrupts and no DMA. */
    uint32_t cmd_status = pci_cfg_read_locked(bdf, PCI_REG_COMMAND);
    uint32_t command = (cmd_status & 0xFFFFu & ~PCI_CMD_BUS_MASTER) | PCI_CMD_INTX_DISABLE;
    pci_cfg_write_locked(bdf, PCI_REG_COMMAND, command);
    if (((cmd_status >> 16) & PCI_STATUS_CAP_LIST) != 0u) {
        uint8_t offset = (uint8_t)(pci_cfg_read_locked(bdf, PCI_REG_CAP_PTR) & 0xFCu);
        for (uint32_t hops = 0; hops < PCI_CAP_HOPS_MAX && offset >= 0x40u; ++hops) {
            uint32_t header = pci_cfg_read_locked(bdf, offset);
            if ((header & 0xFFu) == PCI_CAP_ID_MSI) {
                pci_cap_control_clear(bdf, offset, PCI_MSI_CTRL_ENABLE);
            } else if ((header & 0xFFu) == PCI_CAP_ID_MSIX) {
                pci_cap_control_clear(bdf, offset, PCI_MSIX_CTRL_ENABLE);
            }
            offset = (uint8_t)((header >> 8) & 0xFCu);
        }
    }
    /* A config read completes only after the writes before it: the device has
     * stopped mastering when this returns. */
    (void)pci_cfg_read_locked(bdf, PCI_REG_COMMAND);
    return 1;
}

int pci_quiesce_function(uint16_t bdf) {
    ksync_spinlock_lock(&g_pci_config_lock);
    int rc = pci_quiesce_function_locked(bdf);
    ksync_spinlock_unlock(&g_pci_config_lock);
    return rc;
}

int pci_function_attach(uint32_t context_id, uint16_t bdf) {
    int rc = -1;
    ksync_spinlock_lock(&g_pci_config_lock);
    pci_function_t* fn = pci_function_by_bdf_locked(bdf);
    for (uint32_t i = 0; !fn && i < PCI_FUNCTIONS_MAX; ++i) {
        if (g_pci_functions[i].state == PCI_FUNCTION_FREE) {
            fn = &g_pci_functions[i];
        }
    }
    if (fn) {
        fn->bdf = bdf;
        fn->owner_context = context_id;
        fn->state = PCI_FUNCTION_OWNED;
        rc = 0;
    }
    ksync_spinlock_unlock(&g_pci_config_lock);
    return rc;
}

/* Caller holds g_pci_config_lock. */
static int32_t pci_function_check_locked(uint32_t context_id, uint16_t bdf) {
    const pci_function_t* fn = pci_function_by_bdf_locked(bdf);
    if (!fn) {
        return 0;
    }
    if (fn->state == PCI_FUNCTION_OWNED && fn->owner_context == context_id) {
        return 0;
    }
    return WASMOS_ERR_MSI_NOT_FUNCTION_DRIVER;
}

int32_t pci_function_check(uint32_t context_id, uint16_t bdf) {
    ksync_spinlock_lock(&g_pci_config_lock);
    int32_t rc = pci_function_check_locked(context_id, bdf);
    ksync_spinlock_unlock(&g_pci_config_lock);
    return rc;
}

int32_t pci_config_write_checked(uint32_t context_id, uint16_t bdf, uint8_t reg, uint32_t value) {
    ksync_spinlock_lock(&g_pci_config_lock);
    int32_t rc = pci_function_check_locked(context_id, bdf);
    if (rc == 0) {
        pci_cfg_write_locked(bdf, reg, value);
    }
    ksync_spinlock_unlock(&g_pci_config_lock);
    return rc;
}

void pci_quiesce_context(uint32_t context_id, uint32_t pid) {
    uint16_t bdf = 0;
    int quiesced = 0;
    ksync_spinlock_lock(&g_pci_config_lock);
    for (uint32_t i = 0; i < PCI_FUNCTIONS_MAX; ++i) {
        pci_function_t* fn = &g_pci_functions[i];
        if (fn->state == PCI_FUNCTION_OWNED && fn->owner_context == context_id) {
            bdf = fn->bdf;
            quiesced = pci_quiesce_function_locked(bdf);
            fn->state = PCI_FUNCTION_RETIRED;
            fn->owner_context = 0;
            break;
        }
    }
    ksync_spinlock_unlock(&g_pci_config_lock);
    if (quiesced) {
        klog_printf("[pci] quiesced %02x:%02x.%u: bus mastering and MSI off for pid %u\n",
                    (unsigned)(bdf >> 8),
                    (unsigned)((bdf >> 3) & 0x1Fu),
                    (unsigned)(bdf & 0x7u),
                    (unsigned)pid);
    }
}
