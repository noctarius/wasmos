/* ata.c - ATA/IDE block device WASM driver.
 * Implements PIO-mode ATA read/write for the first IDE device and exposes a
 * block-device IPC interface (BLOCK_IPC_READ_REQ / BLOCK_IPC_WRITE_REQ).
 * Runs inside the WASM runtime; all I/O port accesses go through capability-
 * checked host-call imports. */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

/*
 * Minimal PIO ATA driver used for the early storage bootstrap path. It now
 * supports identify plus small read/write requests, which is enough for the FAT
 * driver to mount the ESP and service the current overwrite-only write path.
 */

/* The driver names no absolute port. Its spawn profile grants I/O windows in the
 * order its manifest declares them; region 0 is the task-file window, which on a
 * legacy IDE controller spans the fixed ISA ports [0x1F0, 0x3F7] and therefore
 * covers the device-control register at 0x3F6 as well. Offsets below are
 * relative to that window's base, so where firmware actually put it -- or
 * whether it moves -- is the kernel's problem, not this file's. */
#define ATA_IO_REGION 0u
#define ATA_REG_CTRL 0x206u /* 0x3F6 - 0x1F0: device control / alternate status */

#define ATA_REG_DATA 0x00
#define ATA_REG_ERROR 0x01
#define ATA_REG_FEATURES 0x01
#define ATA_REG_SECCOUNT0 0x02
#define ATA_REG_LBA0 0x03
#define ATA_REG_LBA1 0x04
#define ATA_REG_LBA2 0x05
#define ATA_REG_HDDEVSEL 0x06
#define ATA_REG_COMMAND 0x07
#define ATA_REG_STATUS 0x07

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

/* Device Control (0x3F6). nIEN set = the drive never asserts INTRQ. Nothing had
 * ever written this register, so device interrupts were masked at the drive the
 * whole time and polling was the only thing that could have worked. */
#define ATA_CTRL_NIEN (1u << 1)

/* Primary channel legacy line. The PIIX IDE function reports no PCI interrupt
 * pin (config 0x3D = 0, irq_hint 0xFF), so the line is not discoverable from the
 * device record — it is the fixed ISA assignment for the primary channel, and
 * the spawn profile grants exactly 14|15 for this driver. */
#define ATA_IRQ_LINE 14u

#define ATA_SECTOR_SIZE 512u
#define ATA_MAX_READ_SECTORS 8u
#define ATA_UNIT_COUNT 2u
#define ATA_CLIENT_MAP_CAP 8u

/* Wait budgets. The polled bound is the historical spin count; the interrupt
 * bound is much smaller because each attempt sleeps rather than spinning
 * (200 x 10 ms = a ~2 s ceiling before a transfer is declared failed). */
#define ATA_POLL_ATTEMPTS 100000u
#define ATA_IRQ_ATTEMPTS 200u
#define ATA_IRQ_WAIT_MS 10
/* Consecutive sleeps that produce no interrupt before the driver stops trusting
 * one. Without this, a line that is routed but silently undelivered would cost
 * the full timeout on every sector for the rest of the boot. */
#define ATA_IRQ_PROBE_LIMIT 8u

static int32_t g_block_endpoint = -1;
static int32_t g_devmgr_endpoint = -1;
static uint32_t g_sector_count = 0;
static uint8_t g_present = 0;
static uint32_t g_unit_sectors[ATA_UNIT_COUNT];
static uint8_t g_unit_present[ATA_UNIT_COUNT];
static uint8_t g_sector_buf[ATA_SECTOR_SIZE];
static uint8_t g_dma_read_ok_logged = 0;
static uint8_t g_dma_write_ok_logged = 0;
static uint8_t g_zc_logged = 0;
static int32_t g_client_owner[ATA_CLIENT_MAP_CAP];
static uint8_t g_client_unit[ATA_CLIENT_MAP_CAP];

/* Interrupt state. Events land on their own endpoint so draining them cannot
 * discard a queued block request (the failure mode that cost virtio-rng a
 * debugging session). g_irq_active means both halves are live: the line is
 * routed AND nIEN is clear at the drive. */
static int32_t g_irq_endpoint = -1;
static int32_t g_irq_select = -1;
static uint8_t g_irq_active;
static uint8_t g_irq_seen;       /* at least one interrupt has actually arrived */
static uint32_t g_irq_dry_waits; /* consecutive sleeps that produced nothing */

/* A refused read cannot be reported through the value (0xFF is a real status),
 * so a failure reads as 0 -- which has neither BSY nor DRQ set and so cannot be
 * mistaken for a ready drive by any caller below. */
static uint8_t ata_read_status(void) {
    uint32_t value = 0;
    if (wasmos_io_region_in8(ATA_IO_REGION, ATA_REG_STATUS, &value) != 0) {
        return 0u;
    }
    return (uint8_t)(value & 0xFFu);
}

static uint16_t ata_read_data16(void) {
    uint32_t value = 0;
    if (wasmos_io_region_in16(ATA_IO_REGION, ATA_REG_DATA, &value) != 0) {
        return 0u;
    }
    return (uint16_t)(value & 0xFFFFu);
}

/* Consume any pending interrupt events. Two things are owed and both matter:
 * reading the status register is what de-asserts the drive's INTRQ, and irq_ack
 * is what reopens the line the kernel masked on dispatch. An event that arrives
 * and is never acked leaves the line masked permanently — i.e. the disk dead.
 * Returns non-zero if an event was consumed. */
static int ata_service_irq(void) {
    int drained = 0;
    if (g_irq_endpoint < 0) {
        return 0;
    }
    while (wasmos_ipc_drain(g_irq_endpoint) > 0) {
        drained = 1;
    }
    if (drained) {
        (void)ata_read_status();
        (void)wasmos_irq_ack((int32_t)ATA_IRQ_LINE);
        if (!g_irq_seen) {
            g_irq_seen = 1;
            (void)printf("[ata] interrupt-driven transfers active\n");
        }
        g_irq_dry_waits = 0;
    }
    return drained;
}

/* Stop using the interrupt and go back to polling, leaving a clean state: quiet
 * the drive first, settle anything owed, then drop the route so the line is
 * masked rather than left asserted with nobody listening. */
static void ata_disable_interrupts(const char* why) {
    if (!g_irq_active) {
        return;
    }
    g_irq_active = 0;
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_CTRL, ATA_CTRL_NIEN);
    (void)ata_service_irq();
    (void)wasmos_irq_unroute((int32_t)ATA_IRQ_LINE);
    (void)printf("[ata] %s; falling back to polled transfers\n", why);
}

/* One wait step between status reads. With the interrupt live this blocks, so
 * waiting for a sector costs no CPU; otherwise it is the historical short I/O
 * delay. A routed-but-undelivered interrupt is detected here and abandoned once,
 * rather than being paid for on every sector. */
static void ata_wait_step(void) {
    if (!g_irq_active) {
        wasmos_io_wait();
        return;
    }
    (void)wasmos_ipc_select_wait_timeout(g_irq_select, ATA_IRQ_WAIT_MS);
    if (ata_service_irq() || g_irq_seen) {
        return;
    }
    if (++g_irq_dry_waits >= ATA_IRQ_PROBE_LIMIT) {
        ata_disable_interrupts("no interrupt from the drive");
    }
}

static uint32_t ata_wait_attempts(void) {
    return g_irq_active ? ATA_IRQ_ATTEMPTS : ATA_POLL_ATTEMPTS;
}

/* Status is read BEFORE waiting in both loops below: the condition is often
 * already true (a pre-command idle check never has an interrupt coming), and
 * sleeping first would trade a spin for a guaranteed timeout. */
static int ata_wait_not_busy(void) {
    for (uint32_t i = 0; i < ata_wait_attempts(); ++i) {
        if ((ata_read_status() & ATA_SR_BSY) == 0) {
            return 0;
        }
        ata_wait_step();
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < ata_wait_attempts(); ++i) {
        uint8_t status = ata_read_status();
        if (status & ATA_SR_ERR) {
            return -1;
        }
        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ)) {
            return 0;
        }
        ata_wait_step();
    }
    return -1;
}

static int ata_identify_unit(uint8_t unit, uint16_t* out_words) {
    if (!out_words) {
        return -1;
    }
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_HDDEVSEL, (uint8_t)(0xA0u | ((unit & 1u) << 4)));
    wasmos_io_wait();
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_SECCOUNT0, 0);
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_LBA0, 0);
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_LBA1, 0);
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_LBA2, 0);
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    if (ata_read_status() == 0) {
        return -1;
    }
    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (uint32_t i = 0; i < 256; ++i) {
        out_words[i] = ata_read_data16();
    }
    return 0;
}

static void ata_publish_block_device(uint8_t unit, uint32_t sectors, uint8_t present) {
    if (g_devmgr_endpoint < 0 || g_block_endpoint < 0) {
        return;
    }
    uint32_t flags = 0;
    if (present) {
        flags |= 1u;
    }
    if (unit == 0 && g_present) {
        flags |= 2u;
    }
    (void)wasmos_ipc_send(g_devmgr_endpoint, g_block_endpoint, DEVMGR_PUBLISH_BLOCK_DEVICE, 0,
                          (int32_t)unit, (int32_t)sectors, (int32_t)flags, 0);
}

/* Where a read deposits each sector. The block buffer is the caller's own
 * staging area addressed by physical address; the transfer buffer belongs to the
 * original client and reaches us as a reborrow, so the kernel admits the write
 * on the strength of that grant. Only the destination differs — the sector loop
 * is identical. */
typedef struct {
    uint8_t to_xfer;     /* 0 = block buffer (phys), 1 = client transfer buffer */
    int32_t id;          /* buffer_phys, or the transfer buffer's object id */
    uint32_t dst_offset; /* byte offset of sector 0 within the destination */
} ata_sink_t;

static int ata_sink_write(const ata_sink_t* sink, const uint8_t* src, uint32_t len,
                          uint32_t sector_offset) {
    uint32_t offset = sink->dst_offset + sector_offset;
    if (sink->to_xfer) {
        return wasmos_xfer_buffer_write(sink->id, addr_cast(int32_t, src), (int32_t)len,
                                        (int32_t)offset);
    }
    return wasmos_block_buffer_write(sink->id, addr_cast(int32_t, src), (int32_t)len,
                                     (int32_t)offset);
}

static int ata_read_lba28(uint8_t unit, uint32_t lba, uint8_t count, const ata_sink_t* sink) {
    if (count == 0 || count > ATA_MAX_READ_SECTORS || !sink || sink->id <= 0) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_HDDEVSEL,
                          (uint8_t)(0xE0u | ((unit & 1u) << 4) | ((lba >> 24) & 0x0Fu)));
    wasmos_io_wait();
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_SECCOUNT0, count);
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    /* Reads are staged through a local 512-byte sector buffer and then handed to
     * the sink, which is either the caller's block buffer or the original
     * client's transfer buffer. */
    for (uint8_t sector = 0; sector < count; ++sector) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        uint16_t* out = (uint16_t*)g_sector_buf;
        for (uint32_t i = 0; i < 256; ++i) {
            out[i] = ata_read_data16();
        }
        if (ata_sink_write(sink, g_sector_buf, ATA_SECTOR_SIZE, sector * ATA_SECTOR_SIZE) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ata_write_lba28(uint8_t unit, uint32_t lba, uint8_t count, uint32_t buffer_phys) {
    if (count == 0 || count > ATA_MAX_READ_SECTORS || buffer_phys == 0) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_HDDEVSEL,
                          (uint8_t)(0xE0u | ((unit & 1u) << 4) | ((lba >> 24) & 0x0Fu)));
    wasmos_io_wait();
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_SECCOUNT0, count);
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    for (uint8_t sector = 0; sector < count; ++sector) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        if (wasmos_block_buffer_copy((int32_t)buffer_phys, addr_cast(int32_t, g_sector_buf),
                                     ATA_SECTOR_SIZE, (int32_t)(sector * ATA_SECTOR_SIZE)) != 0) {
            return -1;
        }
        uint16_t* in = (uint16_t*)g_sector_buf;
        for (uint32_t i = 0; i < 256; ++i) {
            wasmos_io_region_out16(ATA_IO_REGION, ATA_REG_DATA, in[i]);
        }
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy();
}

static void ata_send_resp(int32_t reply_ep, int32_t req_id, int32_t type, int32_t status,
                          int32_t arg1) {
    wasmos_ipc_send(reply_ep, g_block_endpoint, type, req_id, status, arg1, 0, 0);
}

static void ata_log(const char* s) {
    if (!s) {
        return;
    }
    (void)printf("%s", s);
}

static void ata_log_dma_active(uint8_t is_write) {
    if (is_write) {
        if (!g_dma_write_ok_logged) {
            g_dma_write_ok_logged = 1;
            ata_log("[ata] dma write path active\n");
        }
    } else {
        if (!g_dma_read_ok_logged) {
            g_dma_read_ok_logged = 1;
            ata_log("[ata] dma read path active\n");
        }
    }
}

/* Reported once at startup rather than per request. It used to be logged as a
 * per-direction "dma fallback rc=-N", which reads like an intermittent runtime
 * failure; it is neither intermittent nor a failure — see ata_dma_prepare. */
static void ata_log_transfer_mode(void) {
    (void)printf("[ata] transfers are PIO (no bus-master DMA)\n");
}

/* TODO(xfer-buffer owner-push): the zero-copy borrow fast-path is disabled since
 * the migration to the owner-push capability ABI. The old path used
 * borrower-pull borrow + the pre-migration dma_map_borrow(kind, endpoint, ...)
 * signature. Under the new model the block client must own the buffer and grant
 * ata a borrow (borrow_id), which the block IPC protocol must carry; then ata
 * would dma_map_borrow(borrow_id, ...)/dma_sync_borrow/dma_unmap_borrow. Until
 * then this denies unconditionally and the transfer goes through the dedicated
 * block_buffer, which is unaffected by the migration. Note BLOCK_IPC_READ_ZC_REQ
 * now carries a reborrowed client buffer for whole-sector reads, so the read
 * path no longer needs this; what is left is the write direction.
 *
 * Note this was never IDE bus-master DMA either way: there is no BMIDE/PRD
 * programming in this driver, and the sector loop is PIO in both branches. Real
 * device DMA is a separate piece of work (and on QEMU's PIIX means bus-master
 * IDE; an AHCI controller would be the better target). */
static int ata_dma_prepare(int32_t source_endpoint, uint32_t offset, uint32_t length,
                           uint32_t direction_flags, int32_t* out_device_addr) {
    (void)source_endpoint;
    (void)offset;
    (void)length;
    (void)direction_flags;
    (void)out_device_addr;
    return WASMOS_ERR_DMA_DENY; /* force PIO fallback */
}

static int ata_dma_finish(int32_t source_endpoint, uint32_t offset, uint32_t length,
                          uint32_t direction_flags) {
    /* Unreachable while ata_dma_prepare always denies (callers guard on OK). */
    (void)source_endpoint;
    (void)offset;
    (void)length;
    (void)direction_flags;
    return 0;
}

static int ata_assign_unit_for_source(int32_t source, int32_t preferred_unit, uint8_t* out_unit) {
    if (!out_unit || source < 0) {
        return -1;
    }
    for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
        if (g_client_owner[i] == source) {
            *out_unit = g_client_unit[i];
            return 0;
        }
    }
    if (preferred_unit >= 0 && preferred_unit < (int32_t)ATA_UNIT_COUNT) {
        uint8_t unit = (uint8_t)preferred_unit;
        uint8_t claimed = 0;
        if (!g_unit_present[unit]) {
            return -1;
        }
        for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
            if (g_client_owner[i] >= 0 && g_client_unit[i] == unit) {
                claimed = 1;
                break;
            }
        }
        if (claimed) {
            return -1;
        }
        for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
            if (g_client_owner[i] < 0) {
                g_client_owner[i] = source;
                g_client_unit[i] = unit;
                *out_unit = unit;
                return 0;
            }
        }
        return -1;
    }
    for (uint32_t unit = 0; unit < ATA_UNIT_COUNT; ++unit) {
        uint8_t claimed = 0;
        if (!g_unit_present[unit]) {
            continue;
        }
        for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
            if (g_client_owner[i] >= 0 && g_client_unit[i] == unit) {
                claimed = 1;
                break;
            }
        }
        if (claimed) {
            continue;
        }
        for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
            if (g_client_owner[i] < 0) {
                g_client_owner[i] = source;
                g_client_unit[i] = (uint8_t)unit;
                *out_unit = (uint8_t)unit;
                return 0;
            }
        }
    }
    return -1;
}

static int ata_handle_ipc(int32_t type, int32_t source, int32_t req_id, int32_t arg0, int32_t arg1,
                          int32_t arg2, int32_t arg3) {
    uint8_t unit = 0;
    int32_t preferred_unit = -1;
    if (!g_present) {
        ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 1, 0);
        return 0;
    }
    if (type == BLOCK_IPC_IDENTIFY_REQ && arg0 >= 0 && arg0 < (int32_t)ATA_UNIT_COUNT) {
        preferred_unit = arg0;
    }
    if (ata_assign_unit_for_source(source, preferred_unit, &unit) != 0 || !g_unit_present[unit]) {
        ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 1, 0);
        return 0;
    }

    if (type == BLOCK_IPC_IDENTIFY_REQ) {
        wasmos_ipc_send(source, g_block_endpoint, BLOCK_IPC_IDENTIFY_RESP, req_id, 0,
                        (int32_t)g_unit_sectors[unit], (int32_t)unit, 0);
        return 0;
    }

    if (type == BLOCK_IPC_READ_REQ) {
        int32_t dma_rc = WASMOS_ERR_DMA_DENY;
        int32_t dma_addr = 0;
        uint32_t byte_count = 0;
        if (arg2 <= 0 || arg2 > (int32_t)ATA_MAX_READ_SECTORS || arg0 <= 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 2, 0);
            return 0;
        }
        byte_count = (uint32_t)arg2 * ATA_SECTOR_SIZE;
        dma_rc = ata_dma_prepare(source, 0u, byte_count, WASMOS_DMA_DIR_FROM_DEVICE, &dma_addr);
        if (dma_rc == WASMOS_ERR_NONE) {
            (void)dma_addr;
            ata_log_dma_active(0);
        }
        ata_sink_t sink = {0u, arg0, 0u};
        if (ata_read_lba28(unit, (uint32_t)arg1, (uint8_t)arg2, &sink) != 0) {
            if (dma_rc == WASMOS_ERR_NONE) {
                (void)ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_FROM_DEVICE);
            }
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 3, 0);
            return 0;
        }
        if (dma_rc == WASMOS_ERR_NONE &&
            ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_FROM_DEVICE) != 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 3, 0);
            return 0;
        }
        wasmos_ipc_send(source, g_block_endpoint, BLOCK_IPC_READ_RESP, req_id, 0, arg2, 0, 0);
        return 0;
    }

    /* Zero-copy read: land whole sectors straight in the client's transfer
     * buffer. The requester reborrowed it to us, so the kernel admits the write
     * on that grant; we never learn whose buffer it is or map it ourselves.
     * arg0 = buffer_id, arg1 = lba, arg2 = sector count, arg3 = dst byte offset. */
    if (type == BLOCK_IPC_READ_ZC_REQ) {
        if (arg2 <= 0 || arg2 > (int32_t)ATA_MAX_READ_SECTORS || arg0 <= 0 || arg3 < 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 2, 0);
            return 0;
        }
        ata_sink_t sink = {1u, arg0, (uint32_t)arg3};
        if (!g_zc_logged) {
            g_zc_logged = 1;
            (void)printf("[ata] zero-copy reads active\n");
        }
        if (ata_read_lba28(unit, (uint32_t)arg1, (uint8_t)arg2, &sink) != 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 3, 0);
            return 0;
        }
        wasmos_ipc_send(source, g_block_endpoint, BLOCK_IPC_READ_RESP, req_id, 0, arg2, 0, 0);
        return 0;
    }

    if (type == BLOCK_IPC_WRITE_REQ) {
        int32_t dma_rc = WASMOS_ERR_DMA_DENY;
        int32_t dma_addr = 0;
        uint32_t byte_count = 0;
        if (arg2 <= 0 || arg2 > (int32_t)ATA_MAX_READ_SECTORS || arg0 <= 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 2, 0);
            return 0;
        }
        byte_count = (uint32_t)arg2 * ATA_SECTOR_SIZE;
        dma_rc = ata_dma_prepare(source, 0u, byte_count, WASMOS_DMA_DIR_TO_DEVICE, &dma_addr);
        if (dma_rc == WASMOS_ERR_NONE) {
            (void)dma_addr;
            ata_log_dma_active(1);
        }
        if (ata_write_lba28(unit, (uint32_t)arg1, (uint8_t)arg2, (uint32_t)arg0) != 0) {
            if (dma_rc == WASMOS_ERR_NONE) {
                (void)ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_TO_DEVICE);
            }
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 5, 0);
            return 0;
        }
        if (dma_rc == WASMOS_ERR_NONE &&
            ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_TO_DEVICE) != 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 5, 0);
            return 0;
        }
        wasmos_ipc_send(source, g_block_endpoint, BLOCK_IPC_WRITE_RESP, req_id, 0, arg2, 0, 0);
        return 0;
    }

    ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 4, 0);
    return 0;
}

WASMOS_WASM_EXPORT int32_t initialize(int32_t proc_endpoint, int32_t ignored_arg1,
                                      int32_t ignored_arg2, int32_t ignored_arg3) {
    /* proc.endpoint now comes from the spawn-info contract, not an entry arg. */
    proc_endpoint = wasmos_startup_proc_endpoint();
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;

    g_block_endpoint = wasmos_ipc_create_endpoint();
    if (g_block_endpoint < 0) {
        return -1;
    }
    if (wasmos_svc_register(proc_endpoint, g_block_endpoint, "block", 1) != 0) {
        (void)printf("[ata] svc register failed\n");
        return -1;
    }
    g_devmgr_endpoint = -1;
    for (int32_t attempts = 0; attempts < 256; ++attempts) {
        g_devmgr_endpoint =
            wasmos_svc_lookup(proc_endpoint, g_block_endpoint, "devmgr.inv", 1 + attempts);
        if (g_devmgr_endpoint >= 0) {
            break;
        }
        (void)wasmos_sched_yield();
    }
    g_present = 0;
    g_sector_count = 0;
    for (uint32_t i = 0; i < ATA_UNIT_COUNT; ++i) {
        g_unit_present[i] = 0;
        g_unit_sectors[i] = 0;
    }
    for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
        g_client_owner[i] = -1;
        g_client_unit[i] = 0;
    }
    for (uint8_t unit = 0; unit < ATA_UNIT_COUNT; ++unit) {
        uint16_t identify_words[256];
        uint8_t unit_present = 0;
        uint32_t unit_sectors = 0;
        if (ata_identify_unit(unit, identify_words) == 0) {
            uint32_t lba28 = ((uint32_t)identify_words[61] << 16) | identify_words[60];
            unit_sectors = lba28;
            unit_present = 1;
            if (unit == 0) {
                g_sector_count = unit_sectors;
                g_present = 1;
            }
        }
        g_unit_present[unit] = unit_present;
        g_unit_sectors[unit] = unit_sectors;
        if (unit_present) {
            g_present = 1;
        }
        ata_publish_block_device(unit, unit_sectors, unit_present);
    }

    /* The driver addresses its device by region index and never sees a port, so
     * name what each region is for; device-manager logs the window it granted. */
    (void)printf("[ata] io region %u = task file (+%02X status, +%03X control), irq line %u\n",
                 (unsigned)ATA_IO_REGION, (unsigned)ATA_REG_STATUS, (unsigned)ATA_REG_CTRL,
                 (unsigned)ATA_IRQ_LINE);
    ata_log_transfer_mode();

    /* Identify ran polled above; from here transfers can be interrupt-driven.
     * Both halves are needed and in this order: route the line first so an
     * assertion has somewhere to go, then clear nIEN to let the drive assert at
     * all. If either fails the driver keeps working exactly as before — polled —
     * so this is an optimisation, never a dependency of the boot path. */
    if (g_present) {
        g_irq_endpoint = wasmos_ipc_create_endpoint();
        g_irq_select = (g_irq_endpoint >= 0) ? wasmos_ipc_select_create() : -1;
        if (g_irq_endpoint >= 0 && g_irq_select >= 0 &&
            wasmos_ipc_select_add(g_irq_select, g_irq_endpoint) == 0 &&
            wasmos_irq_route_ipc((int32_t)ATA_IRQ_LINE, g_irq_endpoint) == 0) {
            wasmos_io_region_out8(ATA_IO_REGION, ATA_REG_CTRL, 0); /* clear nIEN */
            g_irq_active = 1;
            (void)printf("[ata] irq routed line=%u\n", (unsigned)ATA_IRQ_LINE);
        } else {
            (void)printf("[ata] irq route failed line=%u; using polled transfers\n",
                         (unsigned)ATA_IRQ_LINE);
        }
    }

    /* Drivers are long-running processes: initialize once, then block in the
     * IPC loop forever. */
    wasmos_sys_notify_ready(proc_endpoint, g_block_endpoint);
    for (;;) {
        int32_t recv_rc = wasmos_ipc_select_one(g_block_endpoint);
        if (recv_rc < 0) {
            continue;
        }
        /* Capture the request BEFORE touching the interrupt endpoint: draining a
         * message overwrites the shared last-message fields, so servicing the
         * IRQ first would hand ata_handle_ipc the IRQ event's type and source
         * instead of the block request's. */
        int32_t req_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t req_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        int32_t arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        int32_t arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
        int32_t source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);

        /* Settle anything the previous command left behind. A completion that
         * lands just after a transfer returned would otherwise sit unacked, and
         * the line stays masked until someone acks it. */
        (void)ata_service_irq();

        ata_handle_ipc(req_type, source, req_id, arg0, arg1, arg2, arg3);
    }
    return 0;
}
