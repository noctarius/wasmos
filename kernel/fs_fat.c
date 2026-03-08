#include "fs_fat.h"
#include "physmem.h"
#include "process.h"
#include "serial.h"
#include "wasmos_driver_abi.h"

#define FAT_BUFFER_PAGES 2u
#define FAT_SECTOR_SIZE 512u

typedef enum {
    FS_PHASE_INIT = 0,
    FS_PHASE_WAIT_BOOT,
    FS_PHASE_READY
} fs_phase_t;

typedef struct {
    uint32_t fs_endpoint;
    uint32_t block_endpoint;
    uint32_t block_reply_endpoint;
    uint64_t buffer_phys;
    uint32_t owner_context_id;
    fs_phase_t phase;
    uint32_t boot_lba;
    uint8_t tried_mbr;
} fs_fat_state_t;

static fs_fat_state_t g_fat;

#pragma pack(push, 1)
typedef struct {
    uint8_t jump[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t ext[54];
} fat_bpb_t;
#pragma pack(pop)

typedef struct {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_start;
    uint32_t sectors;
} fat_mbr_entry_t;

static void
fat_log(const char *msg)
{
    serial_write("[fat] ");
    serial_write(msg);
}

static void
fat_log_hex(uint64_t value)
{
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
}

static int
fat_send_block_read(uint32_t lba, uint32_t count)
{
    ipc_message_t req;
    req.type = BLOCK_IPC_READ_REQ;
    req.source = g_fat.block_reply_endpoint;
    req.destination = g_fat.block_endpoint;
    req.request_id = 1;
    if (g_fat.buffer_phys > 0xFFFFFFFFULL) {
        return -1;
    }
    req.arg0 = (uint32_t)g_fat.buffer_phys;
    req.arg1 = lba;
    req.arg2 = count;
    req.arg3 = 0;
    return ipc_send_from(g_fat.owner_context_id, g_fat.block_endpoint, &req) == IPC_OK ? 0 : -1;
}

static int
fat_recv_block_read(void)
{
    ipc_message_t resp;
    int rc = ipc_recv_for(g_fat.owner_context_id, g_fat.block_reply_endpoint, &resp);
    if (rc == IPC_EMPTY) {
        return 1;
    }
    if (rc != IPC_OK) {
        return -1;
    }
    if (resp.type == BLOCK_IPC_ERROR) {
        return -1;
    }
    if (resp.type != BLOCK_IPC_READ_RESP) {
        return -1;
    }
    return 0;
}

static void
fat_parse_boot(void)
{
    fat_bpb_t *bpb = (fat_bpb_t *)(uintptr_t)g_fat.buffer_phys;
    uint16_t sig = *(uint16_t *)((uint8_t *)bpb + 510);
    uint32_t bytes_per_sector = bpb->bytes_per_sector;
    if (sig != 0xAA55 || (bytes_per_sector != 512 && bytes_per_sector != 1024 &&
                          bytes_per_sector != 2048 && bytes_per_sector != 4096)) {
        fat_log("invalid bytes_per_sector\n");
        return;
    }

    uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    uint32_t fat_size = bpb->fat_size_16;
    if (fat_size == 0) {
        fat_size = ((uint16_t *)bpb->ext)[4];
    }

    uint32_t root_dir_sectors = ((bpb->root_entry_count * 32u) + (bytes_per_sector - 1u)) / bytes_per_sector;
    uint32_t data_sectors = total_sectors - (bpb->reserved_sectors + (bpb->fat_count * fat_size) + root_dir_sectors);
    uint32_t cluster_count = data_sectors / bpb->sectors_per_cluster;

    if (bpb->root_entry_count == 0) {
        fat_log("FAT32 detected\n");
    } else if (cluster_count < 4085) {
        fat_log("FAT12 detected\n");
    } else if (cluster_count < 65525) {
        fat_log("FAT16 detected\n");
    } else {
        fat_log("FAT32 detected\n");
    }

    serial_write("[fat] bytes/sector=");
    fat_log_hex(bytes_per_sector);
}

static int
fat_read_boot_sector(uint32_t lba)
{
    if (fat_send_block_read(lba, 1) != 0) {
        fat_log("boot read send failed\n");
        return -1;
    }
    return 0;
}

static int
fat_try_parse_mbr(uint32_t *out_lba)
{
    uint8_t *buf = (uint8_t *)(uintptr_t)g_fat.buffer_phys;
    uint16_t sig = *(uint16_t *)(buf + 510);
    if (sig != 0xAA55) {
        return -1;
    }

    fat_mbr_entry_t *entries = (fat_mbr_entry_t *)(buf + 446);
    for (uint32_t i = 0; i < 4; ++i) {
        uint8_t type = entries[i].type;
        if (type == 0x00) {
            continue;
        }
        if (type == 0xEE) {
            fat_log("GPT detected (unsupported)\n");
            return -1;
        }
        if (type == 0x01 || type == 0x04 || type == 0x06 ||
            type == 0x0B || type == 0x0C || type == 0x0E) {
            *out_lba = entries[i].lba_start;
            return 0;
        }
    }
    return -1;
}

int
fs_fat_init(uint32_t owner_context_id, uint32_t block_endpoint)
{
    g_fat.fs_endpoint = IPC_ENDPOINT_NONE;
    g_fat.block_reply_endpoint = IPC_ENDPOINT_NONE;
    g_fat.block_endpoint = block_endpoint;
    g_fat.buffer_phys = 0;
    g_fat.owner_context_id = owner_context_id;
    g_fat.phase = FS_PHASE_INIT;
    g_fat.boot_lba = 0;
    g_fat.tried_mbr = 0;

    if (ipc_endpoint_create(owner_context_id, &g_fat.fs_endpoint) != IPC_OK) {
        fat_log("endpoint create failed\n");
        return -1;
    }

    if (ipc_endpoint_create(owner_context_id, &g_fat.block_reply_endpoint) != IPC_OK) {
        fat_log("reply endpoint create failed\n");
        return -1;
    }

    uint64_t phys = pfa_alloc_pages_below(FAT_BUFFER_PAGES, 0x100000000ULL);
    if (!phys) {
        fat_log("buffer alloc failed\n");
        return -1;
    }
    g_fat.buffer_phys = phys;
    return 0;
}

int
fs_fat_endpoint(uint32_t *out_endpoint)
{
    if (!out_endpoint || g_fat.fs_endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    *out_endpoint = g_fat.fs_endpoint;
    return 0;
}

static void
fs_send_error(uint32_t reply_ep, uint32_t req_id, uint32_t code)
{
    ipc_message_t resp;
    resp.type = FS_IPC_ERROR;
    resp.source = g_fat.fs_endpoint;
    resp.destination = reply_ep;
    resp.request_id = req_id;
    resp.arg0 = code;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    ipc_send_from(g_fat.owner_context_id, reply_ep, &resp);
}

int
fs_fat_service_once(void)
{
    if (g_fat.phase == FS_PHASE_INIT) {
        g_fat.boot_lba = 0;
        g_fat.tried_mbr = 0;
        if (fat_read_boot_sector(g_fat.boot_lba) != 0) {
            fat_log("boot read send failed\n");
            return -1;
        }
        g_fat.phase = FS_PHASE_WAIT_BOOT;
        return 1;
    }

    if (g_fat.phase == FS_PHASE_WAIT_BOOT) {
        uint32_t lba = 0;
        uint8_t *buf = (uint8_t *)(uintptr_t)g_fat.buffer_phys;
        uint16_t sig = *(uint16_t *)(buf + 510);
        uint16_t bytes_per_sector = *(uint16_t *)(buf + 11);
        int rc = fat_recv_block_read();
        if (rc == 1) {
            return 1;
        }
        if (rc != 0) {
            fat_log("boot read failed\n");
            g_fat.phase = FS_PHASE_READY;
            return 0;
        }

        if (sig != 0xAA55 || bytes_per_sector == 0) {
            if (fat_try_parse_mbr(&lba) != 0) {
                fat_log("boot read failed\n");
                g_fat.phase = FS_PHASE_READY;
                return 0;
            }
            if (g_fat.tried_mbr) {
                fat_log("partition read failed\n");
                g_fat.phase = FS_PHASE_READY;
                return 0;
            }
            g_fat.tried_mbr = 1;
            g_fat.boot_lba = lba;
            if (fat_read_boot_sector(g_fat.boot_lba) != 0) {
                fat_log("partition read send failed\n");
                g_fat.phase = FS_PHASE_READY;
                return 0;
            }
            return 1;
        }
        fat_parse_boot();
        g_fat.phase = FS_PHASE_READY;
        return 0;
    }

    ipc_message_t msg;
    int rc = ipc_recv_for(g_fat.owner_context_id, g_fat.fs_endpoint, &msg);
    if (rc == IPC_EMPTY) {
        return 1;
    }
    if (rc != IPC_OK) {
        return -1;
    }

    fs_send_error(msg.source, msg.request_id, 1);
    return 0;
}
