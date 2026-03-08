#include "block_ata.h"
#include "io.h"
#include "serial.h"
#include "process.h"
#include "wasmos_driver_abi.h"

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

static uint32_t g_block_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_ata_sector_count = 0;
static uint8_t g_ata_present = 0;
static uint32_t g_block_owner_context = 0;

static uint8_t
ata_read_status(void)
{
    return inb(ATA_PRIMARY_BASE + ATA_REG_STATUS);
}

static int
ata_wait_not_busy(void)
{
    for (uint32_t i = 0; i < 100000; ++i) {
        if ((ata_read_status() & ATA_SR_BSY) == 0) {
            return 0;
        }
        io_wait();
    }
    return -1;
}

static int
ata_wait_drq(void)
{
    for (uint32_t i = 0; i < 100000; ++i) {
        uint8_t status = ata_read_status();
        if (status & ATA_SR_ERR) {
            return -1;
        }
        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ)) {
            return 0;
        }
        io_wait();
    }
    return -1;
}

static int
ata_identify(uint16_t *out_words)
{
    outb(ATA_PRIMARY_BASE + ATA_REG_HDDEVSEL, 0xA0);
    io_wait();
    outb(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT0, 0);
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA0, 0);
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA1, 0);
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA2, 0);
    outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    if (ata_read_status() == 0) {
        return -1;
    }
    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (uint32_t i = 0; i < 256; ++i) {
        out_words[i] = inw(ATA_PRIMARY_BASE + ATA_REG_DATA);
    }
    return 0;
}

static int
ata_read_lba28(uint32_t lba, uint8_t count, void *buffer)
{
    if (!buffer || count == 0) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    outb(ATA_PRIMARY_BASE + ATA_REG_HDDEVSEL, 0xE0 | ((lba >> 24) & 0x0F));
    io_wait();
    outb(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT0, count);
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    uint16_t *out = (uint16_t *)buffer;
    for (uint8_t sector = 0; sector < count; ++sector) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < 256; ++i) {
            *out++ = inw(ATA_PRIMARY_BASE + ATA_REG_DATA);
        }
    }

    return 0;
}

int
block_ata_init(uint32_t owner_context_id)
{
    g_block_owner_context = owner_context_id;
    if (ipc_endpoint_create(owner_context_id, &g_block_endpoint) != IPC_OK) {
        serial_write("[ata] endpoint create failed\n");
        return -1;
    }

    uint16_t identify_words[256];
    if (ata_identify(identify_words) != 0) {
        serial_write("[ata] identify failed\n");
        g_ata_present = 0;
        return -1;
    }

    uint32_t lba28 = ((uint32_t)identify_words[61] << 16) | identify_words[60];
    g_ata_sector_count = lba28;
    g_ata_present = 1;

    serial_write("[ata] present sectors=");
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    uint64_t value = g_ata_sector_count;
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
    return 0;
}

int
block_ata_endpoint(uint32_t *out_endpoint)
{
    if (!out_endpoint || g_block_endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    *out_endpoint = g_block_endpoint;
    return 0;
}

static void
block_send_error(uint32_t owner_context_id, uint32_t reply_ep, uint32_t req_id, uint32_t code)
{
    ipc_message_t resp;
    resp.type = BLOCK_IPC_ERROR;
    resp.source = g_block_endpoint;
    resp.destination = reply_ep;
    resp.request_id = req_id;
    resp.arg0 = code;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    ipc_send_from(owner_context_id, reply_ep, &resp);
}

int
block_ata_service_once(void)
{
    ipc_message_t msg;
    int rc = ipc_recv_for(g_block_owner_context, g_block_endpoint, &msg);
    if (rc == IPC_EMPTY) {
        return 1;
    }
    if (rc != IPC_OK) {
        return -1;
    }

    if (!g_ata_present) {
        block_send_error(g_block_owner_context, msg.source, msg.request_id, 1);
        return 0;
    }

    if (msg.type == BLOCK_IPC_IDENTIFY_REQ) {
        ipc_message_t resp;
        resp.type = BLOCK_IPC_IDENTIFY_RESP;
        resp.source = g_block_endpoint;
        resp.destination = msg.source;
        resp.request_id = msg.request_id;
        resp.arg0 = ATA_SECTOR_SIZE;
        resp.arg1 = g_ata_sector_count;
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(g_block_owner_context, msg.source, &resp);
        return 0;
    }

    if (msg.type == BLOCK_IPC_READ_REQ) {
        if (msg.arg2 == 0 || msg.arg2 > ATA_MAX_READ_SECTORS) {
            block_send_error(g_block_owner_context, msg.source, msg.request_id, 2);
            return 0;
        }

        uint32_t buffer_phys = msg.arg0;
        if (buffer_phys == 0) {
            block_send_error(g_block_owner_context, msg.source, msg.request_id, 3);
            return 0;
        }

        if (ata_read_lba28(msg.arg1, (uint8_t)msg.arg2, (void *)(uintptr_t)buffer_phys) != 0) {
            block_send_error(g_block_owner_context, msg.source, msg.request_id, 4);
            return 0;
        }

        ipc_message_t resp;
        resp.type = BLOCK_IPC_READ_RESP;
        resp.source = g_block_endpoint;
        resp.destination = msg.source;
        resp.request_id = msg.request_id;
        resp.arg0 = 0;
        resp.arg1 = msg.arg2;
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(g_block_owner_context, msg.source, &resp);
        return 0;
    }

    if (msg.type == BLOCK_IPC_WRITE_REQ) {
        block_send_error(g_block_owner_context, msg.source, msg.request_id, 5);
        return 0;
    }

    block_send_error(g_block_owner_context, msg.source, msg.request_id, 6);
    return 0;
}
