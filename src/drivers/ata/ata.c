#include <stdint.h>
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

/*
 * Minimal PIO ATA driver used for the early storage bootstrap path. It only
 * supports identify and read requests, which is enough for the FAT driver to
 * mount the ESP and for the rest of the system to move from preloaded modules
 * to filesystem-backed loading.
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

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

#define ATA_SECTOR_SIZE 512u
#define ATA_MAX_READ_SECTORS 8u

static int32_t g_block_endpoint = -1;
static uint32_t g_sector_count = 0;
static uint8_t g_present = 0;
static uint8_t g_sector_buf[ATA_SECTOR_SIZE];

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
ata_identify(uint16_t *out_words)
{
    if (!out_words) {
        return -1;
    }
    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_HDDEVSEL, 0xA0);
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

static int
ata_read_lba28(uint32_t lba, uint8_t count, uint32_t buffer_phys)
{
    if (count == 0 || count > ATA_MAX_READ_SECTORS || buffer_phys == 0) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    wasmos_io_out8(ATA_PRIMARY_BASE + ATA_REG_HDDEVSEL, 0xE0 | ((lba >> 24) & 0x0F));
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

static int
ata_handle_ipc(int32_t type, int32_t source, int32_t req_id, int32_t arg0, int32_t arg1, int32_t arg2)
{
    if (!g_present) {
        ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 1, 0);
        return 0;
    }

    if (type == BLOCK_IPC_IDENTIFY_REQ) {
        ata_send_resp(source, req_id, BLOCK_IPC_IDENTIFY_RESP, 0, (int32_t)g_sector_count);
        return 0;
    }

    if (type == BLOCK_IPC_READ_REQ) {
        if (arg2 <= 0 || arg2 > (int32_t)ATA_MAX_READ_SECTORS || arg0 <= 0) {
            ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 2, 0);
            return 0;
        }
        if (ata_read_lba28((uint32_t)arg1, (uint8_t)arg2, (uint32_t)arg0) != 0) {
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

    ata_send_resp(source, req_id, BLOCK_IPC_ERROR, 4, 0);
    return 0;
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t block_endpoint,
           int32_t ignored_arg1,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;

    g_block_endpoint = block_endpoint;
    g_present = 0;
    g_sector_count = 0;
    uint16_t identify_words[256];
    if (ata_identify(identify_words) == 0) {
        uint32_t lba28 = ((uint32_t)identify_words[61] << 16) | identify_words[60];
        g_sector_count = lba28;
        g_present = 1;
    }

    /* Drivers are long-running processes: initialize once, then block in the
     * IPC loop forever. */
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
