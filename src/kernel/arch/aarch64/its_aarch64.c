/* its_aarch64.c - GICv3 ITS (ARM IHI 0069, chapter 5) for message-signalled
 * interrupts on QEMU virt.
 *
 * Flat device and collection tables sized from GITS_TYPER and the GITS_BASER
 * entry sizes, a 4 KiB command queue (128 commands), one 4 KiB interrupt
 * translation table per device (256 EventIDs), and an LPI configuration table
 * for INTIDs below 2^LPI_ID_BITS. Every table is Normal write-back inner
 * shareable memory from below the direct-map window, which the ITS reads
 * coherently; it is never freed.
 *
 * TODO: take the ITS base from the ACPI MADT (GIC ITS entry) and the
 * DeviceID mapping from the IORT instead of assuming QEMU virt's address and
 * DeviceID == requester id (arm64 port, Phase 3); route collections to more
 * than one CPU (Phase 4). */
#include "its_aarch64.h"

#include "arch/arch_cpu.h"
#include "arch/arch_mmio.h"
#include "arch/vm_layout.h"
#include "gic_aarch64.h"
#include "klog.h"
#include "physmem.h"

#define GITS_PHYS 0x08080000ULL
#define GITS_BASE (KERNEL_HIGHER_HALF_BASE | GITS_PHYS)
#define GITS_CTLR 0x0000u
#define GITS_CTLR_ENABLED (1u << 0)
#define GITS_TYPER 0x0008u
#define GITS_CBASER 0x0080u
#define GITS_CWRITER 0x0088u
#define GITS_CREADR 0x0090u
#define GITS_BASER0 0x0100u
#define GITS_BASER_COUNT 8u
#define GITS_TRANSLATER_OFFSET 0x10040ULL

#define GITS_BASER_VALID (1ULL << 63)
#define GITS_BASER_CACHE_WB (7ULL << 59)
#define GITS_BASER_TYPE_SHIFT 56
#define GITS_BASER_TYPE_DEVICE 1u
#define GITS_BASER_TYPE_COLLECTION 4u
#define GITS_BASER_ESIZE_SHIFT 48
#define GITS_BASER_SHARE_INNER (1ULL << 10)
#define GITS_BASER_PAGE_SIZE_MASK (3ULL << 8)
#define GITS_BASER_MAX_PAGES 256u

#define ITS_CMD_SYNC 0x05u
#define ITS_CMD_MAPD 0x08u
#define ITS_CMD_MAPC 0x09u
#define ITS_CMD_MAPTI 0x0Au
#define ITS_CMD_INV 0x0Cu
#define ITS_CMD_DISCARD 0x0Fu
#define ITS_CMD_BYTES 32u
#define ITS_QUEUE_BYTES 4096u
#define ITS_COLLECTION 0u
/* EventID bits per device: a 4 KiB ITT holds 256 entries of up to 16 bytes. */
#define ITS_EVENT_ID_BITS 8u
#define ITS_DEVICES_MAX 32u

#define LPI_ID_BITS 14u
#define LPI_COUNT ((1u << LPI_ID_BITS) - GIC_LPI_BASE)
/* Priority 0xA0 (above the PMR threshold), enabled. */
#define LPI_CONFIG_ENABLED 0xA1u
#define PAGE 4096ULL

typedef struct {
    uint16_t bdf;
    uint8_t mapped;
} its_device_t;

static uint64_t* g_queue;
static uint32_t g_queue_offset;
static uint64_t g_rdbase;
static uint8_t* g_lpi_config;
static its_device_t g_devices[ITS_DEVICES_MAX];

static uint64_t gits_read64(uint32_t reg) {
    uint64_t lo = arch_mmio_read32((uintptr_t)(GITS_BASE + reg));
    uint64_t hi = arch_mmio_read32((uintptr_t)(GITS_BASE + reg + 4u));
    return lo | (hi << 32);
}

static void gits_write64(uint32_t reg, uint64_t value) {
    arch_mmio_write32((uintptr_t)(GITS_BASE + reg), (uint32_t)value);
    arch_mmio_write32((uintptr_t)(GITS_BASE + reg + 4u), (uint32_t)(value >> 32));
}

/* Zeroed frames below the direct-map window, or 0. */
static uint64_t alloc_zeroed(uint64_t pages) {
    uint64_t phys = pfa_alloc_pages_below(pages, KERNEL_HIGHER_HALF_WINDOW_BYTES);
    if (phys) {
        __builtin_memset(ptr_cast(void, (phys | KERNEL_HIGHER_HALF_BASE)), 0, pages * PAGE);
    }
    return phys;
}

/* Queue one command and wait until the ITS has consumed it. Returns 0, or -1
 * when the read pointer does not catch up. */
static int its_command(uint64_t dw0, uint64_t dw1, uint64_t dw2) {
    uint64_t* cmd = g_queue + g_queue_offset / 8u;
    cmd[0] = dw0;
    cmd[1] = dw1;
    cmd[2] = dw2;
    cmd[3] = 0;
    __asm__ volatile("dsb ishst" ::: "memory");
    g_queue_offset = (g_queue_offset + ITS_CMD_BYTES) % ITS_QUEUE_BYTES;
    gits_write64(GITS_CWRITER, g_queue_offset);
    for (uint32_t spin = 0; spin < 1000000u; ++spin) {
        if ((gits_read64(GITS_CREADR) & 0xFFFFFULL) == g_queue_offset) {
            return 0;
        }
        arch_cpu_relax();
    }
    klog_printf("[its] command %02llx not consumed\n", (unsigned long long)(dw0 & 0xFFu));
    return -1;
}

static int its_sync(void) {
    return its_command(ITS_CMD_SYNC, 0, g_rdbase << 16);
}

/* Allocate and install the table GITS_BASER<n> asks for. Returns 0 or -1. */
static int its_setup_baser(uint32_t n, uint32_t device_id_bits) {
    uint32_t reg = GITS_BASER0 + n * 8u;
    uint64_t baser = gits_read64(reg);
    uint32_t type = (uint32_t)((baser >> GITS_BASER_TYPE_SHIFT) & 7u);
    if (type != GITS_BASER_TYPE_DEVICE && type != GITS_BASER_TYPE_COLLECTION) {
        return 0;
    }
    uint64_t esize = ((baser >> GITS_BASER_ESIZE_SHIFT) & 0x1Fu) + 1u;
    uint64_t entries = type == GITS_BASER_TYPE_DEVICE ? (1ULL << device_id_bits) : 256u;
    uint64_t pages = (entries * esize + PAGE - 1u) / PAGE;
    if (pages > GITS_BASER_MAX_PAGES) {
        pages = GITS_BASER_MAX_PAGES;
    }
    uint64_t phys = alloc_zeroed(pages);
    if (!phys) {
        return -1;
    }
    uint64_t value =
        phys | GITS_BASER_VALID | GITS_BASER_CACHE_WB | ((uint64_t)type << GITS_BASER_TYPE_SHIFT) |
        ((esize - 1u) << GITS_BASER_ESIZE_SHIFT) | GITS_BASER_SHARE_INNER | (pages - 1u);
    gits_write64(reg, value);
    if ((gits_read64(reg) & GITS_BASER_PAGE_SIZE_MASK) != 0u) {
        klog_printf("[its] BASER%u refuses 4 KiB pages\n", (unsigned int)n);
        return -1;
    }
    return 0;
}

int its_init(void) {
    uint64_t typer = gits_read64(GITS_TYPER);
    if (typer == 0 || typer == 0xFFFFFFFFFFFFFFFFULL) {
        klog_write("[its] no ITS: message-signalled interrupts unavailable\n");
        return -1;
    }
    uint32_t device_id_bits = (uint32_t)((typer >> 13) & 0x1Fu) + 1u;
    if (device_id_bits > 16u) {
        device_id_bits = 16u;
    }
    for (uint32_t n = 0; n < GITS_BASER_COUNT; ++n) {
        if (its_setup_baser(n, device_id_bits) != 0) {
            return -1;
        }
    }
    uint64_t queue = alloc_zeroed(ITS_QUEUE_BYTES / PAGE);
    uint64_t config = alloc_zeroed((LPI_COUNT + PAGE - 1u) / PAGE);
    /* The pending table covers every INTID below 2^LPI_ID_BITS, one bit each,
     * and must be 64 KiB aligned: over-allocate and align inside. */
    uint64_t pend_raw = alloc_zeroed(32u);
    if (!queue || !config || !pend_raw) {
        klog_write("[its] table allocation failed\n");
        return -1;
    }
    uint64_t pend = (pend_raw + 0xFFFFULL) & ~0xFFFFULL;
    g_queue = ptr_cast(uint64_t, (queue | KERNEL_HIGHER_HALF_BASE));
    g_lpi_config = ptr_cast(uint8_t, (config | KERNEL_HIGHER_HALF_BASE));
    gits_write64(GITS_CBASER,
                 queue | GITS_BASER_VALID | GITS_BASER_CACHE_WB | GITS_BASER_SHARE_INNER |
                     (ITS_QUEUE_BYTES / PAGE - 1u));
    gits_write64(GITS_CWRITER, 0);
    g_queue_offset = 0;
    uint32_t ctlr = arch_mmio_read32((uintptr_t)(GITS_BASE + GITS_CTLR));
    arch_mmio_write32((uintptr_t)(GITS_BASE + GITS_CTLR), ctlr | GITS_CTLR_ENABLED);

    if (gic_lpi_enable(config, LPI_ID_BITS, pend) != 0) {
        klog_write("[its] redistributor implements no LPIs\n");
        return -1;
    }
    /* GITS_TYPER.PTA: collections target a redistributor by physical address
     * (in 64 KiB units) rather than by processor number. */
    g_rdbase = ((typer >> 19) & 1u) ? (gic_redistributor_phys() >> 16)
                                    : gic_redistributor_processor_number();
    if (its_command(ITS_CMD_MAPC, 0, (1ULL << 63) | (g_rdbase << 16) | ITS_COLLECTION) != 0 ||
        its_sync() != 0) {
        return -1;
    }
    klog_printf(
        "[its] ready devbits=%u lpis=%u\n", (unsigned int)device_id_bits, (unsigned int)LPI_COUNT);
    return 0;
}

/* The device slot for bdf, mapping it with a fresh ITT on first use, or 0. */
static its_device_t* its_device(uint16_t bdf) {
    its_device_t* free_slot = 0;
    for (uint32_t i = 0; i < ITS_DEVICES_MAX; ++i) {
        if (g_devices[i].mapped && g_devices[i].bdf == bdf) {
            return &g_devices[i];
        }
        if (!g_devices[i].mapped && !free_slot) {
            free_slot = &g_devices[i];
        }
    }
    if (!free_slot) {
        return 0;
    }
    uint64_t itt = alloc_zeroed(1);
    if (!itt) {
        return 0;
    }
    if (its_command(ITS_CMD_MAPD | ((uint64_t)bdf << 32),
                    ITS_EVENT_ID_BITS - 1u,
                    (1ULL << 63) | (itt & 0x000FFFFFFFFFFF00ULL)) != 0) {
        return 0;
    }
    free_slot->bdf = bdf;
    free_slot->mapped = 1;
    return free_slot;
}

int its_map_event(uint16_t bdf, uint32_t event, uint32_t lpi) {
    if (!g_queue || lpi < GIC_LPI_BASE || lpi - GIC_LPI_BASE >= LPI_COUNT ||
        event >= (1u << ITS_EVENT_ID_BITS) || !its_device(bdf)) {
        return -1;
    }
    if (its_command(ITS_CMD_MAPTI | ((uint64_t)bdf << 32),
                    (uint64_t)event | ((uint64_t)lpi << 32),
                    ITS_COLLECTION) != 0) {
        return -1;
    }
    g_lpi_config[lpi - GIC_LPI_BASE] = LPI_CONFIG_ENABLED;
    __asm__ volatile("dsb ishst" ::: "memory");
    if (its_command(ITS_CMD_INV | ((uint64_t)bdf << 32), event, 0) != 0) {
        return -1;
    }
    return its_sync();
}

void its_unmap_event(uint16_t bdf, uint32_t event, uint32_t lpi) {
    if (!g_queue || lpi < GIC_LPI_BASE || lpi - GIC_LPI_BASE >= LPI_COUNT) {
        return;
    }
    g_lpi_config[lpi - GIC_LPI_BASE] = 0;
    __asm__ volatile("dsb ishst" ::: "memory");
    (void)its_command(ITS_CMD_DISCARD | ((uint64_t)bdf << 32), event, 0);
    (void)its_sync();
}

uint64_t its_translater_phys(void) {
    return GITS_PHYS + GITS_TRANSLATER_OFFSET;
}
