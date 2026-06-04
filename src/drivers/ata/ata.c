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
#include "wasmos_driver_abi.h"

/*
 * Minimal PIO ATA driver used for the early storage bootstrap path. It now
 * supports identify plus small read/write requests, which is enough for the FAT
 * driver to mount the ESP and service the current overwrite-only write path.
 */

#define ATA_PRIMARY_BASE 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6

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

#define ATA_SECTOR_SIZE 512u
#define ATA_MAX_READ_SECTORS 8u
#define ATA_UNIT_COUNT 2u
#define ATA_CLIENT_MAP_CAP 8u

static int32_t g_block_endpoint = -1;
static int32_t g_devmgr_endpoint = -1;
static uint32_t g_sector_count = 0;
static uint8_t g_present = 0;
static uint32_t g_unit_sectors[ATA_UNIT_COUNT];
static uint8_t g_unit_present[ATA_UNIT_COUNT];
static uint8_t g_sector_buf[ATA_SECTOR_SIZE];
static uint8_t g_dma_read_ok_logged = 0;
static uint8_t g_dma_write_ok_logged = 0;
static uint8_t g_dma_read_fallback_logged = 0;
static uint8_t g_dma_write_fallback_logged = 0;
static int32_t g_client_owner[ATA_CLIENT_MAP_CAP];
static uint8_t g_client_unit[ATA_CLIENT_MAP_CAP];

static uint8_t
ata_read_status(void)
{
    return (uint8_t)wasmos_io_in8(ATA_PRIMARY_BASE + ATA_REG_STATUS);
}

static int
ata_wait_not_busy(void)
{
    for (uint32_t i = 0; i < 100000; ++i) {
        if ((ata_read_status() & ATA_SR_BSY) == 0) {
            return 0;
        }
        wasmos_io_wait();
    }
    return -1;
}

static int
ata_wait_drq(void)
{
    /* Polling is acceptable here because the driver is intentionally tiny and
     * only used in the single-disk bootstrap path. */
    for (uint32_t i = 0; i < 100000; ++i) {
        uint8_t status = ata_read_status();
        if (status & ATA_SR_ERR) {
            return -1;
        }
        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ)) {
            return 0;
        }
        wasmos_io_wait();
    }
    return -1;
}

static int
ata_identify_unit(uint8_t unit, uint16_t *out_words)
{
    if (!out_words) {
        return -1;
    }
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_HDDEVSEL, (uint8_t)(0xA0u | ((unit & 1u) << 4)));
    wasmos_io_wait();
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT0, 0);
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_LBA0, 0);
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_LBA1, 0);
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_LBA2, 0);
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    if (ata_read_status() == 0) {
        return -1;
    }
    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (uint32_t i = 0; i < 256; ++i) {
        out_words[i] = (uint16_t)wasmos_io_in16(ATA_PRIMARY_BASE + ATA_REG_DATA);
    }
    return 0;
}

static void
ata_publish_block_device(uint8_t unit, uint32_t sectors, uint8_t present)
{
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
    (void)wasmos_ipc_send(g_devmgr_endpoint,
                          g_block_endpoint,
                          DEVMGR_PUBLISH_BLOCK_DEVICE,
                          0,
                          (int32_t)unit,
                          (int32_t)sectors,
                          (int32_t)flags,
                          0);
}

static int
ata_read_lba28(uint8_t unit, uint32_t lba, uint8_t count, uint32_t buffer_phys)
{
    if (count == 0 || count > ATA_MAX_READ_SECTORS || buffer_phys == 0) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_HDDEVSEL, (uint8_t)(0xE0u | ((unit & 1u) << 4) | ((lba >> 24) & 0x0Fu)));
    wasmos_io_wait();
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT0, count);
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    /* Reads are staged through a local 512-byte sector buffer and then copied
     * into the shared block buffer owned by the kernel/consumer side. */
    for (uint8_t sector = 0; sector < count; ++sector) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        uint16_t *out = (uint16_t *)g_sector_buf;
        for (uint32_t i = 0; i < 256; ++i) {
            out[i] = (uint16_t)wasmos_io_in16(ATA_PRIMARY_BASE + ATA_REG_DATA);
        }
        if (wasmos_block_buffer_write((int32_t)buffer_phys,
                                      (int32_t)(uintptr_t)g_sector_buf,
                                      ATA_SECTOR_SIZE,
                                      (int32_t)(sector * ATA_SECTOR_SIZE)) != 0) {
            return -1;
        }
    }

    return 0;
}

static int
ata_write_lba28(uint8_t unit, uint32_t lba, uint8_t count, uint32_t buffer_phys)
{
    if (count == 0 || count > ATA_MAX_READ_SECTORS || buffer_phys == 0) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_HDDEVSEL, (uint8_t)(0xE0u | ((unit & 1u) << 4) | ((lba >> 24) & 0x0Fu)));
    wasmos_io_wait();
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT0, count);
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    for (uint8_t sector = 0; sector < count; ++sector) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        if (wasmos_block_buffer_copy((int32_t)buffer_phys,
                                     (int32_t)(uintptr_t)g_sector_buf,
                                     ATA_SECTOR_SIZE,
                                     (int32_t)(sector * ATA_SECTOR_SIZE)) != 0) {
            return -1;
        }
        uint16_t *in = (uint16_t *)g_sector_buf;
        for (uint32_t i = 0; i < 256; ++i) {
            wasmos_io_out16(ATA_PRIMARY_BASE + ATA_REG_DATA, in[i]);
        }
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy();
}

static void
ata_send_resp(int32_t reply_ep, int32_t req_id, int32_t type, int32_t status, int32_t arg1)
{
    wasmos_ipc_send(reply_ep,
                    g_block_endpoint,
                    type,
                    req_id,
                    status,
                    arg1,
                    0,
                    0);
}

static void
ata_log(const char *s)
{
    if (!s) {
        return;
    }
    (void)printf("%s", s);
}

static void
ata_log_dma_active(uint8_t is_write)
{
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

static void
ata_log_dma_fallback(uint8_t is_write, int32_t rc)
{
    if (is_write) {
        if (!g_dma_write_fallback_logged) {
            g_dma_write_fallback_logged = 1;
            (void)printf("[ata] dma write fallback rc=%d\n", (int)rc);
        }
    } else {
        if (!g_dma_read_fallback_logged) {
            g_dma_read_fallback_logged = 1;
            (void)printf("[ata] dma read fallback rc=%d\n", (int)rc);
        }
    }
}

static int
ata_dma_prepare(int32_t source_endpoint,
                uint32_t offset,
                uint32_t length,
                uint32_t direction_flags,
                int32_t *out_device_addr)
{
    int32_t rc = 0;
    if (!out_device_addr || source_endpoint < 0 || length == 0) {
        return WASMOS_DMA_STATUS_INVALID;
    }
    rc = wasmos_buffer_borrow(WASMOS_BUFFER_KIND_FS,
                              source_endpoint,
                              (direction_flags & WASMOS_DMA_DIR_FROM_DEVICE) ? WASMOS_BUFFER_GRANT_WRITE : WASMOS_BUFFER_GRANT_READ);
    if (rc != 0) {
        return rc;
    }
    rc = wasmos_dma_map_borrow(WASMOS_BUFFER_KIND_FS,
                               source_endpoint,
                               (int32_t)offset,
                               (int32_t)length,
                               (int32_t)direction_flags);
    if (rc < 0) {
        (void)wasmos_buffer_release(WASMOS_BUFFER_KIND_FS);
        return rc;
    }
    *out_device_addr = rc;
    if ((direction_flags & WASMOS_DMA_DIR_TO_DEVICE) != 0) {
        rc = wasmos_dma_sync_borrow(WASMOS_BUFFER_KIND_FS,
                                    (int32_t)offset,
                                    (int32_t)length,
                                    WASMOS_DMA_SYNC_TO_DEVICE);
        if (rc != WASMOS_DMA_STATUS_OK) {
            (void)wasmos_dma_unmap_borrow(WASMOS_BUFFER_KIND_FS, source_endpoint);
            (void)wasmos_buffer_release(WASMOS_BUFFER_KIND_FS);
            return rc;
        }
    }
    return WASMOS_DMA_STATUS_OK;
}

static int
ata_dma_finish(int32_t source_endpoint,
               uint32_t offset,
               uint32_t length,
               uint32_t direction_flags)
{
    int rc = WASMOS_DMA_STATUS_OK;
    if ((direction_flags & WASMOS_DMA_DIR_FROM_DEVICE) != 0) {
        rc = wasmos_dma_sync_borrow(WASMOS_BUFFER_KIND_FS,
                                    (int32_t)offset,
                                    (int32_t)length,
                                    WASMOS_DMA_SYNC_FROM_DEVICE);
        if (rc != WASMOS_DMA_STATUS_OK) {
            (void)wasmos_dma_unmap_borrow(WASMOS_BUFFER_KIND_FS, source_endpoint);
            (void)wasmos_buffer_release(WASMOS_BUFFER_KIND_FS);
            return -1;
        }
    }
    if (wasmos_dma_unmap_borrow(WASMOS_BUFFER_KIND_FS, source_endpoint) != WASMOS_DMA_STATUS_OK) {
        (void)wasmos_buffer_release(WASMOS_BUFFER_KIND_FS);
        return -1;
    }
    if (wasmos_buffer_release(WASMOS_BUFFER_KIND_FS) != 0) {
        return -1;
    }
    return 0;
}

static int
ata_assign_unit_for_source(int32_t source, uint8_t *out_unit)
{
    if (!out_unit || source < 0) {
        return -1;
    }
    for (uint32_t i = 0; i < ATA_CLIENT_MAP_CAP; ++i) {
        if (g_client_owner[i] == source) {
            *out_unit = g_client_unit[i];
            return 0;
        }
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

static int
ata_handle_ipc(int32_t type, int32_t source, int32_t req_id, int32_t arg0, int32_t arg1, int32_t arg2)
{
    uint8_t unit = 0;
    if (!g_present) {
        ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 1, 0);
        return 0;
    }
    if (ata_assign_unit_for_source(source, &unit) != 0 || !g_unit_present[unit]) {
        ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 1, 0);
        return 0;
    }

    if (type == BLOCK_IPC_IDENTIFY_REQ) {
        wasmos_ipc_send(source,
                        g_block_endpoint,
                        BLOCK_IPC_IDENTIFY_RESP,
                        req_id,
                        0,
                        (int32_t)g_unit_sectors[unit],
                        (int32_t)unit,
                        0);
        return 0;
    }

    if (type == BLOCK_IPC_READ_REQ) {
        int32_t dma_rc = WASMOS_DMA_STATUS_DENY;
        int32_t dma_addr = 0;
        uint32_t byte_count = 0;
        if (arg2 <= 0 || arg2 > (int32_t)ATA_MAX_READ_SECTORS || arg0 <= 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 2, 0);
            return 0;
        }
        byte_count = (uint32_t)arg2 * ATA_SECTOR_SIZE;
        dma_rc = ata_dma_prepare(source,
                                 0u,
                                 byte_count,
                                 WASMOS_DMA_DIR_FROM_DEVICE,
                                 &dma_addr);
        if (dma_rc != WASMOS_DMA_STATUS_OK) {
            ata_log_dma_fallback(0, dma_rc);
        } else {
            (void)dma_addr;
            ata_log_dma_active(0);
        }
        if (ata_read_lba28(unit, (uint32_t)arg1, (uint8_t)arg2, (uint32_t)arg0) != 0) {
            if (dma_rc == WASMOS_DMA_STATUS_OK) {
                (void)ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_FROM_DEVICE);
            }
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 3, 0);
            return 0;
        }
        if (dma_rc == WASMOS_DMA_STATUS_OK &&
            ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_FROM_DEVICE) != 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 3, 0);
            return 0;
        }
        wasmos_ipc_send(source,
                        g_block_endpoint,
                        BLOCK_IPC_READ_RESP,
                        req_id,
                        0,
                        arg2,
                        0,
                        0);
        return 0;
    }

    if (type == BLOCK_IPC_WRITE_REQ) {
        int32_t dma_rc = WASMOS_DMA_STATUS_DENY;
        int32_t dma_addr = 0;
        uint32_t byte_count = 0;
        if (arg2 <= 0 || arg2 > (int32_t)ATA_MAX_READ_SECTORS || arg0 <= 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 2, 0);
            return 0;
        }
        byte_count = (uint32_t)arg2 * ATA_SECTOR_SIZE;
        dma_rc = ata_dma_prepare(source,
                                 0u,
                                 byte_count,
                                 WASMOS_DMA_DIR_TO_DEVICE,
                                 &dma_addr);
        if (dma_rc != WASMOS_DMA_STATUS_OK) {
            ata_log_dma_fallback(1, dma_rc);
        } else {
            (void)dma_addr;
            ata_log_dma_active(1);
        }
        if (ata_write_lba28(unit, (uint32_t)arg1, (uint8_t)arg2, (uint32_t)arg0) != 0) {
            if (dma_rc == WASMOS_DMA_STATUS_OK) {
                (void)ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_TO_DEVICE);
            }
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 5, 0);
            return 0;
        }
        if (dma_rc == WASMOS_DMA_STATUS_OK &&
            ata_dma_finish(source, 0u, byte_count, WASMOS_DMA_DIR_TO_DEVICE) != 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 5, 0);
            return 0;
        }
        wasmos_ipc_send(source,
                        g_block_endpoint,
                        BLOCK_IPC_WRITE_RESP,
                        req_id,
                        0,
                        arg2,
                        0,
                        0);
        return 0;
    }

    ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 4, 0);
    return 0;
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t ignored_arg1,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
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
        g_devmgr_endpoint = wasmos_svc_lookup(proc_endpoint,
                                              g_block_endpoint,
                                              "devmgr.inv",
                                              1 + attempts);
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

    /* Drivers are long-running processes: initialize once, then block in the
     * IPC loop forever. */
    wasmos_sys_notify_ready(proc_endpoint, g_block_endpoint);
    for (;;) {
        int32_t recv_rc = wasmos_ipc_recv(g_block_endpoint);
        if (recv_rc < 0) {
            continue;
        }

        int32_t req_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t req_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        int32_t arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        int32_t source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);

        ata_handle_ipc(req_type, source, req_id, arg0, arg1, arg2);
    }
    return 0;
}
