#include <stdint.h>
#include "ctype.h"
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

/*
 * fs-fat is the first filesystem service in the stack. Its scope is pragmatic:
 * mount the ESP over the ATA block driver, expose enough IPC to list/cat/cd,
 * feed PM with app blobs, and back the current read-only libc file API.
 */

#define FAT_SECTOR_SIZE 512u
#define FAT_MAX_SECTOR_BYTES 4096u
#define FAT_LFN_MAX 255u
#define FAT_MAX_OPEN_FILES 16u
#define FAT_MAX_PATH 128u
#define FAT_WAITING 1

typedef enum {
    FAT_BOOT_INIT = 0,
    FAT_BOOT_WAIT,
    FAT_BOOT_READY,
    FAT_BOOT_FAILED
} fat_boot_phase_t;

typedef enum {
    FAT_TYPE_UNKNOWN = 0,
    FAT_TYPE_12,
    FAT_TYPE_16,
    FAT_TYPE_32
} fat_type_t;

typedef enum {
    FAT_OP_NONE = 0,
    FAT_OP_LIST,
    FAT_OP_CAT,
    FAT_OP_LIST_DIR,
    FAT_OP_CAT_DIR,
    FAT_OP_CHDIR,
    FAT_OP_READ_APP
} fat_op_t;

typedef enum {
    FAT_CAT_SCAN = 0,
    FAT_CAT_FILE
} fat_cat_stage_t;

typedef enum {
    FAT_READ_FIND_APPS = 0,
    FAT_READ_FIND_FILE,
    FAT_READ_FILE
} fat_read_stage_t;

static int32_t g_block_endpoint = -1;
static int32_t g_fs_endpoint = -1;
static int32_t g_reply_endpoint = -1;
static int32_t g_block_req_id = 1;
static int32_t g_block_buf_phys = -1;
static uint8_t g_sector_buf[FAT_MAX_SECTOR_BYTES];

static fat_boot_phase_t g_boot_phase = FAT_BOOT_INIT;
static uint32_t g_boot_lba = 0;
static uint8_t g_tried_mbr = 0;

static uint16_t g_bytes_per_sector = 0;
static uint8_t g_sectors_per_cluster = 0;
static uint16_t g_reserved_sectors = 0;
static uint8_t g_fat_count = 0;
static uint16_t g_root_entry_count = 0;
static uint32_t g_fat_size = 0;
static uint32_t g_total_sectors = 0;
static fat_type_t g_fat_type = FAT_TYPE_UNKNOWN;
static uint32_t g_root_dir_lba = 0;
static uint32_t g_root_dir_sectors = 0;
static uint32_t g_dir_lba = 0;
static uint32_t g_dir_sectors = 0;
static int32_t g_cwd_source = -1;
static uint16_t g_cwd_cluster = 0;
static uint8_t g_cwd_root = 1;

static fat_op_t g_op = FAT_OP_NONE;
static fat_cat_stage_t g_cat_stage = FAT_CAT_SCAN;
static uint32_t g_op_sector = 0;
static uint32_t g_op_entries_left = 0;
static uint32_t g_file_remaining = 0;
static uint32_t g_file_lba = 0;
static uint32_t g_file_sector = 0;
static uint16_t g_file_cluster = 0;
static char g_target_name[16];
static char g_dir_name[16];
static char g_chdir_path[32];
static uint32_t g_chdir_pos = 0;
static char g_chdir_name[16];
static uint16_t g_chdir_cluster = 0;
static uint8_t g_chdir_root = 1;
static uint32_t g_chdir_dir_lba = 0;
static uint32_t g_chdir_dir_sectors = 0;
static char g_read_name[32];
static char g_read_name_ext[32];
static char g_read_name_alt[32];
static char g_read_name_alt_ext[32];
static uint32_t g_read_dir_lba = 0;
static uint32_t g_read_dir_sectors = 0;
static uint32_t g_read_sector = 0;
static uint32_t g_read_entries_left = 0;
static uint32_t g_read_offset = 0;
static uint32_t g_read_size = 0;
static uint32_t g_read_max = 0;
static fat_read_stage_t g_read_stage = FAT_READ_FIND_APPS;
static char g_lfn_buf[FAT_LFN_MAX + 1u];
static uint8_t g_lfn_total = 0;
static uint8_t g_lfn_seen = 0;
static uint8_t g_lfn_valid = 0;

static int32_t g_waiting = 0;
static uint32_t g_wait_lba = 0;
static uint32_t g_wait_count = 0;
static int32_t g_wait_req_id = 0;
static int32_t g_wait_resp_type = 0;
static uint8_t g_wait_copy_into_sector = 0;
static int32_t g_fs_resp_override = 0;
static int32_t g_fs_resp_arg0 = 0;
static int32_t g_fs_resp_arg1 = 0;

typedef struct {
    uint8_t in_use;
    int32_t owner;
    int32_t flags;
    uint16_t first_cluster;
    uint16_t current_cluster;
    uint32_t current_sector;
    uint32_t file_lba;
    uint32_t size;
    uint32_t offset;
} fat_open_file_t;

typedef struct {
    uint8_t valid;
    uint8_t attr;
    uint16_t cluster;
    uint32_t size;
} fat_dir_entry_info_t;

static fat_open_file_t g_open_files[FAT_MAX_OPEN_FILES];

static void fat_lfn_reset(void);
static void fat_lfn_finalize(void);
static void fat_lfn_collect(const uint8_t *ent);
static int fat_name_eq(const char *a, const char *b);
static uint32_t fat_lba_for_cluster(uint16_t cluster);
static int fat_next_cluster(uint16_t cluster, uint16_t *out_next);
static int fat_reposition_open_file(fat_open_file_t *file, uint32_t offset);
static int fat_sync_block_write(uint32_t lba);

typedef struct {
    uint8_t in_use;
    int32_t type;
    int32_t arg0;
    int32_t arg1;
    int32_t arg2;
    int32_t arg3;
    int32_t source;
    int32_t request_id;
} fat_fs_request_t;

static fat_fs_request_t g_fs_req;

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

static int32_t
str_len(const char *s)
{
    return (int32_t)strlen(s);
}

static char
to_upper(char c)
{
    return (char)toupper((unsigned char)c);
}

static void
console_write(const char *s)
{
    int32_t len = str_len(s);
    if (len <= 0) {
        return;
    }
    wasmos_console_write((int32_t)(uintptr_t)s, len);
}

static void
fat_log(const char *msg)
{
    console_write("[fat] ");
    console_write(msg);
}

static void
fat_stall(void)
{
    int32_t endpoint = wasmos_ipc_create_endpoint();
    for (;;) {
        if (endpoint >= 0) {
            (void)wasmos_ipc_recv(endpoint);
        }
    }
}

static int
fat_send_block_read(uint32_t lba, uint32_t count)
{
    /* Block reads are asynchronous at the protocol level even though the driver
     * currently processes one filesystem operation at a time. */
    if (g_block_endpoint < 0 || g_reply_endpoint < 0 || g_block_buf_phys < 0) {
        return -1;
    }
    g_wait_lba = lba;
    g_wait_count = count;
    g_wait_req_id = g_block_req_id++;
    g_wait_resp_type = BLOCK_IPC_READ_RESP;
    g_wait_copy_into_sector = 1;
    g_waiting = 1;

    if (wasmos_ipc_send(g_block_endpoint,
                        g_reply_endpoint,
                        BLOCK_IPC_READ_REQ,
                        g_wait_req_id,
                        g_block_buf_phys,
                        (int32_t)lba,
                        (int32_t)count,
                        0) != 0) {
        g_waiting = 0;
        return -1;
    }
    return 0;
}

static int
fat_send_block_write(uint32_t lba, uint32_t count)
{
    if (g_block_endpoint < 0 || g_reply_endpoint < 0 || g_block_buf_phys < 0) {
        return -1;
    }
    g_wait_lba = lba;
    g_wait_count = count;
    g_wait_req_id = g_block_req_id++;
    g_wait_resp_type = BLOCK_IPC_WRITE_RESP;
    g_wait_copy_into_sector = 0;
    g_waiting = 1;

    if (wasmos_ipc_send(g_block_endpoint,
                        g_reply_endpoint,
                        BLOCK_IPC_WRITE_REQ,
                        g_wait_req_id,
                        g_block_buf_phys,
                        (int32_t)lba,
                        (int32_t)count,
                        0) != 0) {
        g_waiting = 0;
        return -1;
    }
    return 0;
}

static int
fat_poll_block_io(void)
{
    if (!g_waiting) {
        return 0;
    }

    /* The FAT state machines reuse a single reply endpoint and complete one
     * block request at a time, which keeps memory use and control flow simple. */
    int32_t recv_rc = wasmos_ipc_recv(g_reply_endpoint);
    if (recv_rc < 0) {
        g_waiting = 0;
        fat_log("block recv failed\n");
        return -1;
    }

    int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    int32_t resp_status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    if (resp_req != g_wait_req_id) {
        g_waiting = 0;
        fat_log("block req mismatch\n");
        return -1;
    }
    if (resp_type == BLOCK_IPC_ERROR || resp_status != 0) {
        g_waiting = 0;
        fat_log("block io error\n");
        return -1;
    }
    if (resp_type != g_wait_resp_type) {
        g_waiting = 0;
        fat_log("block resp type bad\n");
        return -1;
    }

    if (g_wait_copy_into_sector) {
        uint32_t bytes = g_wait_count * FAT_SECTOR_SIZE;
        if (bytes > FAT_MAX_SECTOR_BYTES) {
            g_waiting = 0;
            return -1;
        }
        if (wasmos_block_buffer_copy(g_block_buf_phys,
                                     (int32_t)(uintptr_t)g_sector_buf,
                                     (int32_t)bytes,
                                     0) != 0) {
            g_waiting = 0;
            fat_log("block copy failed\n");
            return -1;
        }
    }
    g_waiting = 0;
    return 0;
}

static int
fat_poll_block_read(void)
{
    return fat_poll_block_io();
}

static int
fat_try_parse_mbr(uint32_t *out_lba)
{
    uint16_t sig = (uint16_t)g_sector_buf[510] | ((uint16_t)g_sector_buf[511] << 8);
    if (sig != 0xAA55) {
        return -1;
    }

    fat_mbr_entry_t *entries = (fat_mbr_entry_t *)(uintptr_t)(g_sector_buf + 446);
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

static int
fat_parse_boot(void)
{
    fat_bpb_t *bpb = (fat_bpb_t *)(uintptr_t)g_sector_buf;
    uint16_t sig = (uint16_t)g_sector_buf[510] | ((uint16_t)g_sector_buf[511] << 8);
    uint32_t bytes_per_sector = bpb->bytes_per_sector;
    if (sig != 0xAA55 ||
        (bytes_per_sector != 512 && bytes_per_sector != 1024 &&
         bytes_per_sector != 2048 && bytes_per_sector != 4096)) {
        fat_log("invalid bytes_per_sector\n");
        return -1;
    }

    uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    uint32_t fat_size = bpb->fat_size_16;
    if (fat_size == 0) {
        fat_size = ((uint16_t *)bpb->ext)[4];
    }

    uint32_t root_dir_sectors = ((bpb->root_entry_count * 32u) + (bytes_per_sector - 1u)) / bytes_per_sector;
    uint32_t data_sectors = total_sectors - (bpb->reserved_sectors + (bpb->fat_count * fat_size) + root_dir_sectors);
    uint32_t cluster_count = data_sectors / bpb->sectors_per_cluster;

    g_bytes_per_sector = (uint16_t)bytes_per_sector;
    g_sectors_per_cluster = bpb->sectors_per_cluster;
    g_reserved_sectors = bpb->reserved_sectors;
    g_fat_count = bpb->fat_count;
    g_root_entry_count = bpb->root_entry_count;
    g_fat_size = fat_size;
    g_total_sectors = total_sectors;
    g_root_dir_sectors = root_dir_sectors;
    g_root_dir_lba = g_boot_lba + bpb->reserved_sectors + (bpb->fat_count * fat_size);

    if (bpb->root_entry_count == 0) {
        g_fat_type = FAT_TYPE_32;
        fat_log("FAT32 detected\n");
    } else if (cluster_count < 4085) {
        g_fat_type = FAT_TYPE_12;
        fat_log("FAT12 detected\n");
    } else if (cluster_count < 65525) {
        g_fat_type = FAT_TYPE_16;
        fat_log("FAT16 detected\n");
    } else {
        g_fat_type = FAT_TYPE_32;
        fat_log("FAT32 detected\n");
    }

    fat_log("bytes/sector=0x0000000000000200\n");
    return 0;
}

static int
fat_ensure_ready(void)
{
    for (;;) {
        if (g_boot_phase == FAT_BOOT_READY) {
            return 0;
        }
        if (g_boot_phase == FAT_BOOT_FAILED) {
            return -1;
        }

        if (g_boot_phase == FAT_BOOT_INIT) {
            g_boot_lba = 0;
            g_tried_mbr = 0;
            if (fat_send_block_read(g_boot_lba, 1) != 0) {
                fat_log("boot read send failed\n");
                g_boot_phase = FAT_BOOT_FAILED;
                return -1;
            }
            g_boot_phase = FAT_BOOT_WAIT;
            continue;
        }

        if (g_boot_phase == FAT_BOOT_WAIT) {
            int rc = fat_poll_block_read();
            if (rc != 0) {
                g_op = FAT_OP_NONE;
                return -1;
            }

            uint16_t sig = (uint16_t)g_sector_buf[510] | ((uint16_t)g_sector_buf[511] << 8);
            uint16_t bytes_per_sector = (uint16_t)g_sector_buf[11] | ((uint16_t)g_sector_buf[12] << 8);
            if (sig != 0xAA55 || bytes_per_sector == 0) {
                uint32_t lba = 0;
                if (!g_tried_mbr && fat_try_parse_mbr(&lba) == 0) {
                    g_tried_mbr = 1;
                    g_boot_lba = lba;
                    if (fat_send_block_read(g_boot_lba, 1) != 0) {
                        fat_log("partition read send failed\n");
                        g_boot_phase = FAT_BOOT_FAILED;
                        return -1;
                    }
                    continue;
                }
                fat_log("boot read failed\n");
                g_boot_phase = FAT_BOOT_FAILED;
                return -1;
            }

            if (fat_parse_boot() != 0) {
                fat_log("boot parse failed\n");
                g_boot_phase = FAT_BOOT_FAILED;
                return -1;
            }
            g_boot_phase = FAT_BOOT_READY;
            return 0;
        }
        return -1;
    }
}

static void
fat_unpack_name(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, char *out, uint32_t out_len)
{
    uint32_t args[4] = { arg0, arg1, arg2, arg3 };
    uint32_t pos = 0;
    for (uint32_t i = 0; i < 4 && pos + 1 < out_len; ++i) {
        uint32_t v = args[i];
        for (uint32_t b = 0; b < 4 && pos + 1 < out_len; ++b) {
            char c = (char)(v & 0xFF);
            if (c == '\0') {
                out[pos] = '\0';
                return;
            }
            out[pos++] = to_upper(c);
            v >>= 8;
        }
    }
    out[pos] = '\0';
}

static void
fat_write_name(const char *name, uint32_t len)
{
    char buf[16];
    uint32_t out = 0;
    for (uint32_t i = 0; i < len && out + 1 < sizeof(buf); ++i) {
        buf[out++] = name[i];
    }
    buf[out] = '\0';
    console_write(buf);
}

static void
fat_write_full(const char *name)
{
    char buf[64];
    uint32_t pos = 0;
    if (!name) {
        return;
    }
    for (uint32_t i = 0; name[i]; ++i) {
        buf[pos++] = name[i];
        if (pos == sizeof(buf) - 1) {
            buf[pos] = '\0';
            console_write(buf);
            pos = 0;
        }
    }
    if (pos > 0) {
        buf[pos] = '\0';
        console_write(buf);
    }
}

static uint32_t
fat_str_len(const char *s)
{
    uint32_t len = 0;
    if (!s) {
        return 0;
    }
    while (s[len]) {
        len++;
    }
    return len;
}

static int
fat_sync_block_read(uint32_t lba)
{
    if (fat_send_block_read(lba, 1) != 0) {
        return -1;
    }
    return fat_poll_block_read();
}

static int
fat_sync_block_write(uint32_t lba)
{
    if (wasmos_block_buffer_write(g_block_buf_phys,
                                  (int32_t)(uintptr_t)g_sector_buf,
                                  FAT_SECTOR_SIZE,
                                  0) != 0) {
        return -1;
    }
    if (fat_send_block_write(lba, 1) != 0) {
        return -1;
    }
    return fat_poll_block_io();
}

static int
fat_entry_name_from_dirent(const uint8_t *ent, char *out, uint32_t out_len)
{
    uint32_t pos = 0;

    if (!ent || !out || out_len == 0) {
        return -1;
    }

    if (g_lfn_valid && g_lfn_seen == g_lfn_total && g_lfn_buf[0]) {
        fat_lfn_finalize();
        while (g_lfn_buf[pos] && pos + 1 < out_len) {
            out[pos] = g_lfn_buf[pos];
            pos++;
        }
        out[pos] = '\0';
        return 0;
    }

    for (uint32_t i = 0; i < 8 && pos + 1 < out_len; ++i) {
        if (ent[i] != ' ') {
            out[pos++] = (char)ent[i];
        }
    }
    if (ent[8] != ' ' && pos + 1 < out_len) {
        out[pos++] = '.';
        for (uint32_t i = 0; i < 3 && pos + 1 < out_len; ++i) {
            if (ent[8 + i] != ' ') {
                out[pos++] = (char)ent[8 + i];
            }
        }
    }
    out[pos] = '\0';
    return 0;
}

static uint32_t
fat_dir_entry_limit(uint8_t root, uint32_t dir_sectors)
{
    if (root) {
        return g_root_entry_count;
    }
    return (dir_sectors * g_bytes_per_sector) / 32u;
}

static int
fat_find_in_dir(uint32_t dir_lba,
                uint32_t dir_sectors,
                uint32_t entry_limit,
                const char *target,
                fat_dir_entry_info_t *out)
{
    uint32_t entries_left = entry_limit;

    if (!target || !out) {
        return -1;
    }

    fat_lfn_reset();
    for (uint32_t sector = 0; sector < dir_sectors && entries_left > 0; ++sector) {
        uint32_t entries_per_sector = g_bytes_per_sector / 32u;
        uint32_t entries_total = entries_left < entries_per_sector ? entries_left : entries_per_sector;

        if (fat_sync_block_read(dir_lba + sector) != 0) {
            fat_lfn_reset();
            return -1;
        }

        for (uint32_t i = 0; i < entries_total; ++i) {
            uint8_t *ent = g_sector_buf + i * 32u;
            char entry_name[FAT_LFN_MAX + 1u];

            if (ent[0] == 0x00) {
                fat_lfn_reset();
                return -1;
            }
            if (ent[0] == 0xE5) {
                fat_lfn_reset();
                continue;
            }
            if ((ent[11] & 0x0F) == 0x0F) {
                fat_lfn_collect(ent);
                continue;
            }
            if (ent[11] & 0x08) {
                fat_lfn_reset();
                continue;
            }

            fat_entry_name_from_dirent(ent, entry_name, sizeof(entry_name));
            if (!fat_name_eq(entry_name, target)) {
                fat_lfn_reset();
                continue;
            }

            out->valid = 1;
            out->attr = ent[11];
            out->cluster = (uint16_t)ent[26] | ((uint16_t)ent[27] << 8);
            out->size = (uint32_t)ent[28] |
                        ((uint32_t)ent[29] << 8) |
                        ((uint32_t)ent[30] << 16) |
                        ((uint32_t)ent[31] << 24);
            fat_lfn_reset();
            return 0;
        }

        entries_left -= entries_total;
    }

    fat_lfn_reset();
    return -1;
}

static int
fat_path_next_component(const char *path, uint32_t *pos, char *component, uint32_t component_len)
{
    uint32_t out = 0;

    if (!path || !pos || !component || component_len < 2) {
        return -1;
    }

    while (path[*pos] == '/') {
        (*pos)++;
    }
    if (path[*pos] == '\0') {
        component[0] = '\0';
        return 0;
    }

    while (path[*pos] && path[*pos] != '/') {
        if (out + 1 >= component_len) {
            return -1;
        }
        component[out++] = path[*pos];
        (*pos)++;
    }
    component[out] = '\0';
    return 1;
}

static int
fat_path_has_more(const char *path, uint32_t pos)
{
    while (path[pos] == '/') {
        pos++;
    }
    return path[pos] != '\0';
}

static int
fat_resolve_path(const char *path, fat_dir_entry_info_t *out)
{
    uint8_t current_root = 1;
    uint32_t current_lba = g_root_dir_lba;
    uint32_t current_sectors = g_root_dir_sectors;
    uint32_t pos = 0;

    if (!path || !out || g_root_entry_count == 0 || g_root_dir_sectors == 0) {
        return -1;
    }

    if (path[0] != '\0' && path[0] != '/' && g_fs_req.source == g_cwd_source && !g_cwd_root && g_dir_lba != 0) {
        current_root = 0;
        current_lba = g_dir_lba;
        current_sectors = g_dir_sectors;
    }

    for (;;) {
        char component[32];
        fat_dir_entry_info_t entry = { 0 };
        int rc = fat_path_next_component(path, &pos, component, sizeof(component));
        if (rc <= 0) {
            return -1;
        }
        if (component[0] == '.' && component[1] == '\0') {
            if (!fat_path_has_more(path, pos)) {
                return -1;
            }
            continue;
        }
        if (component[0] == '.' && component[1] == '.' && component[2] == '\0') {
            current_root = 1;
            current_lba = g_root_dir_lba;
            current_sectors = g_root_dir_sectors;
            if (!fat_path_has_more(path, pos)) {
                return -1;
            }
            continue;
        }
        if (fat_find_in_dir(current_lba,
                            current_sectors,
                            fat_dir_entry_limit(current_root, current_sectors),
                            component,
                            &entry) != 0) {
            return -1;
        }
        if (!fat_path_has_more(path, pos)) {
            *out = entry;
            return 0;
        }
        if (!(entry.attr & 0x10) || entry.cluster < 2) {
            return -1;
        }
        current_root = 0;
        current_lba = fat_lba_for_cluster(entry.cluster);
        current_sectors = g_sectors_per_cluster;
        if (current_lba == 0 || current_sectors == 0) {
            return -1;
        }
    }
}

static fat_open_file_t *
fat_open_file_for_fd(int32_t source, int32_t fd)
{
    int32_t index = fd - 3;

    if (index < 0 || (uint32_t)index >= FAT_MAX_OPEN_FILES) {
        return 0;
    }
    if (!g_open_files[index].in_use || g_open_files[index].owner != source) {
        return 0;
    }
    return &g_open_files[index];
}

static int
fat_open_file_alloc(int32_t source, int32_t *out_fd)
{
    for (uint32_t i = 0; i < FAT_MAX_OPEN_FILES; ++i) {
        if (!g_open_files[i].in_use) {
            g_open_files[i].in_use = 1;
            g_open_files[i].owner = source;
            g_open_files[i].flags = 0;
            g_open_files[i].first_cluster = 0;
            g_open_files[i].current_cluster = 0;
            g_open_files[i].current_sector = 0;
            g_open_files[i].file_lba = 0;
            g_open_files[i].size = 0;
            g_open_files[i].offset = 0;
            *out_fd = (int32_t)i + 3;
            return 0;
        }
    }
    return -1;
}

static int
fat_handle_open(void)
{
    char path[FAT_MAX_PATH];
    fat_dir_entry_info_t entry = { 0 };
    int32_t fd = -1;
    uint32_t path_len = (uint32_t)g_fs_req.arg0;

    if ((g_fs_req.arg1 != 0 && g_fs_req.arg1 != 1) || path_len == 0 || path_len >= sizeof(path)) {
        return -1;
    }
    if (path_len + 1u > (uint32_t)wasmos_fs_buffer_size()) {
        return -1;
    }
    if (wasmos_fs_buffer_copy((int32_t)(uintptr_t)path, (int32_t)path_len, 0) != 0) {
        return -1;
    }
    path[path_len] = '\0';

    if (fat_resolve_path(path, &entry) != 0 || !entry.valid || (entry.attr & 0x10)) {
        return -1;
    }

    if (entry.size > 0 && entry.cluster < 2) {
        return -1;
    }
    if (fat_open_file_alloc(g_fs_req.source, &fd) != 0) {
        return -1;
    }

    fat_open_file_t *file = fat_open_file_for_fd(g_fs_req.source, fd);
    if (!file) {
        return -1;
    }
    file->flags = g_fs_req.arg1;
    file->first_cluster = entry.size == 0 ? 0 : entry.cluster;
    file->current_cluster = entry.size == 0 ? 0 : entry.cluster;
    file->current_sector = 0;
    file->file_lba = entry.size == 0 ? 0 : fat_lba_for_cluster(entry.cluster);
    file->size = entry.size;
    file->offset = 0;

    g_fs_resp_override = 1;
    g_fs_resp_arg0 = fd;
    return 0;
}

static int
fat_handle_read_open_file(void)
{
    fat_open_file_t *file = fat_open_file_for_fd(g_fs_req.source, g_fs_req.arg0);
    uint32_t max_buffer = (uint32_t)wasmos_fs_buffer_size();
    uint32_t remaining;
    uint32_t requested;
    uint32_t done = 0;

    if (!file || file->flags != 0 || g_fs_req.arg1 < 0) {
        return -1;
    }

    remaining = file->offset < file->size ? file->size - file->offset : 0;
    requested = (uint32_t)g_fs_req.arg1;
    if (requested > max_buffer) {
        requested = max_buffer;
    }
    if (requested > remaining) {
        requested = remaining;
    }

    while (done < requested) {
        uint32_t sector_offset = file->offset % g_bytes_per_sector;
        uint32_t chunk = g_bytes_per_sector - sector_offset;
        uint16_t next_cluster = 0;

        if (chunk > requested - done) {
            chunk = requested - done;
        }
        if (file->current_cluster < 2 || file->file_lba == 0) {
            return -1;
        }
        if (fat_sync_block_read(file->file_lba + file->current_sector) != 0) {
            return -1;
        }
        if (wasmos_fs_buffer_write((int32_t)(uintptr_t)(g_sector_buf + sector_offset),
                                   (int32_t)chunk,
                                   (int32_t)done) != 0) {
            return -1;
        }
        file->offset += chunk;
        done += chunk;

        if (file->offset >= file->size || sector_offset + chunk < g_bytes_per_sector) {
            continue;
        }

        file->current_sector++;
        if (file->current_sector < g_sectors_per_cluster) {
            continue;
        }
        if (fat_next_cluster(file->current_cluster, &next_cluster) != 0) {
            return -1;
        }
        file->current_cluster = next_cluster;
        file->current_sector = 0;
        file->file_lba = fat_lba_for_cluster(next_cluster);
    }

    g_fs_resp_override = 1;
    g_fs_resp_arg0 = (int32_t)done;
    return 0;
}

static int
fat_handle_write_open_file(void)
{
    fat_open_file_t *file = fat_open_file_for_fd(g_fs_req.source, g_fs_req.arg0);
    uint32_t max_buffer = (uint32_t)wasmos_fs_buffer_size();
    uint32_t remaining;
    uint32_t requested;
    uint32_t done = 0;

    if (!file || file->flags != 1 || g_fs_req.arg1 < 0) {
        return -1;
    }

    /* TODO: Support FAT allocation and directory-entry updates so writes can
     * grow files instead of stopping at the current file size. */
    remaining = file->offset < file->size ? file->size - file->offset : 0;
    requested = (uint32_t)g_fs_req.arg1;
    if (requested > max_buffer) {
        requested = max_buffer;
    }
    if (requested > remaining) {
        requested = remaining;
    }

    while (done < requested) {
        uint32_t sector_offset = file->offset % g_bytes_per_sector;
        uint32_t chunk = g_bytes_per_sector - sector_offset;
        uint16_t next_cluster = 0;

        if (chunk > requested - done) {
            chunk = requested - done;
        }
        if (file->current_cluster < 2 || file->file_lba == 0) {
            return -1;
        }
        if (sector_offset != 0 || chunk != g_bytes_per_sector) {
            if (fat_sync_block_read(file->file_lba + file->current_sector) != 0) {
                return -1;
            }
        }
        if (wasmos_fs_buffer_copy((int32_t)(uintptr_t)(g_sector_buf + sector_offset),
                                  (int32_t)chunk,
                                  (int32_t)done) != 0) {
            return -1;
        }
        if (fat_sync_block_write(file->file_lba + file->current_sector) != 0) {
            return -1;
        }
        file->offset += chunk;
        done += chunk;

        if (file->offset >= file->size || sector_offset + chunk < g_bytes_per_sector) {
            continue;
        }

        file->current_sector++;
        if (file->current_sector < g_sectors_per_cluster) {
            continue;
        }
        if (fat_next_cluster(file->current_cluster, &next_cluster) != 0) {
            return -1;
        }
        file->current_cluster = next_cluster;
        file->current_sector = 0;
        file->file_lba = fat_lba_for_cluster(next_cluster);
    }

    g_fs_resp_override = 1;
    g_fs_resp_arg0 = (int32_t)done;
    return 0;
}

static int
fat_handle_stat(void)
{
    char path[FAT_MAX_PATH];
    fat_dir_entry_info_t entry = { 0 };
    uint32_t path_len = (uint32_t)g_fs_req.arg0;

    if (g_fs_req.arg1 != 0 || path_len == 0 || path_len >= sizeof(path)) {
        return -1;
    }
    if (path_len + 1u > (uint32_t)wasmos_fs_buffer_size()) {
        return -1;
    }
    if (wasmos_fs_buffer_copy((int32_t)(uintptr_t)path, (int32_t)path_len, 0) != 0) {
        return -1;
    }
    path[path_len] = '\0';

    if (fat_resolve_path(path, &entry) != 0 || !entry.valid) {
        return -1;
    }

    g_fs_resp_override = 1;
    g_fs_resp_arg0 = (int32_t)entry.size;
    g_fs_resp_arg1 = (entry.attr & 0x10) ? 0x4000 : 0x8000;
    return 0;
}

static int
fat_reposition_open_file(fat_open_file_t *file, uint32_t offset)
{
    uint32_t cluster_bytes;
    uint32_t cluster_skip;

    if (!file || offset > file->size) {
        return -1;
    }

    file->offset = offset;
    if (file->size == 0 || offset == file->size) {
        file->current_cluster = file->first_cluster;
        file->current_sector = 0;
        file->file_lba = file->first_cluster >= 2 ? fat_lba_for_cluster(file->first_cluster) : 0;
        return 0;
    }
    if (file->first_cluster < 2 || g_sectors_per_cluster == 0 || g_bytes_per_sector == 0) {
        return -1;
    }

    cluster_bytes = (uint32_t)g_sectors_per_cluster * g_bytes_per_sector;
    cluster_skip = offset / cluster_bytes;
    file->current_cluster = file->first_cluster;
    file->current_sector = (offset % cluster_bytes) / g_bytes_per_sector;

    while (cluster_skip > 0) {
        uint16_t next_cluster = 0;
        if (fat_next_cluster(file->current_cluster, &next_cluster) != 0) {
            return -1;
        }
        file->current_cluster = next_cluster;
        cluster_skip--;
    }

    file->file_lba = fat_lba_for_cluster(file->current_cluster);
    return file->file_lba == 0 ? -1 : 0;
}

static int
fat_handle_seek_open_file(void)
{
    fat_open_file_t *file = fat_open_file_for_fd(g_fs_req.source, g_fs_req.arg0);
    int32_t base;
    int64_t target;

    if (!file) {
        return -1;
    }

    if (g_fs_req.arg2 == 0) {
        base = 0;
    } else if (g_fs_req.arg2 == 1) {
        base = (int32_t)file->offset;
    } else if (g_fs_req.arg2 == 2) {
        base = (int32_t)file->size;
    } else {
        return -1;
    }

    target = (int64_t)base + (int64_t)g_fs_req.arg1;
    if (target < 0 || (uint64_t)target > file->size) {
        return -1;
    }
    if (fat_reposition_open_file(file, (uint32_t)target) != 0) {
        return -1;
    }

    g_fs_resp_override = 1;
    g_fs_resp_arg0 = (int32_t)target;
    return 0;
}

static int
fat_handle_close_open_file(void)
{
    fat_open_file_t *file = fat_open_file_for_fd(g_fs_req.source, g_fs_req.arg0);

    if (!file) {
        return -1;
    }

    file->in_use = 0;
    file->owner = -1;
    file->flags = 0;
    file->first_cluster = 0;
    file->current_cluster = 0;
    file->current_sector = 0;
    file->file_lba = 0;
    file->size = 0;
    file->offset = 0;
    return 0;
}

static void
fat_lfn_reset(void)
{
    g_lfn_total = 0;
    g_lfn_seen = 0;
    g_lfn_valid = 0;
    for (uint32_t i = 0; i < sizeof(g_lfn_buf); ++i) {
        g_lfn_buf[i] = '\0';
    }
}

static void
fat_lfn_store_char(uint32_t pos, uint16_t ch)
{
    if (pos >= FAT_LFN_MAX) {
        return;
    }
    if (ch == 0x0000 || ch == 0xFFFF) {
        if (g_lfn_buf[pos] == '\0') {
            return;
        }
        g_lfn_buf[pos] = '\0';
        return;
    }
    if ((ch & 0xFF00u) != 0) {
        g_lfn_buf[pos] = '?';
        return;
    }
    g_lfn_buf[pos] = (char)(ch & 0xFFu);
}

static void
fat_lfn_finalize(void)
{
    if (!g_lfn_valid || g_lfn_total == 0) {
        return;
    }
    uint32_t max_len = (uint32_t)g_lfn_total * 13u;
    if (max_len > FAT_LFN_MAX) {
        max_len = FAT_LFN_MAX;
    }
    for (uint32_t i = 0; i < max_len; ++i) {
        if (g_lfn_buf[i] == '\0') {
            return;
        }
    }
    if (max_len < sizeof(g_lfn_buf)) {
        g_lfn_buf[max_len] = '\0';
    } else {
        g_lfn_buf[sizeof(g_lfn_buf) - 1] = '\0';
    }
}

static void
fat_lfn_collect(const uint8_t *ent)
{
    uint8_t ord = ent[0];
    if (ord == 0xE5) {
        fat_lfn_reset();
        return;
    }
    ord &= 0x1Fu;
    if (ord == 0) {
        fat_lfn_reset();
        return;
    }
    if (ent[0] & 0x40) {
        fat_lfn_reset();
        g_lfn_valid = 1;
        g_lfn_total = ord;
    }
    if (!g_lfn_valid) {
        return;
    }
    if (ord > g_lfn_total) {
        fat_lfn_reset();
        return;
    }

    uint32_t base = (uint32_t)(ord - 1u) * 13u;
    for (uint32_t i = 0; i < 5; ++i) {
        uint16_t ch = (uint16_t)ent[1 + i * 2] | ((uint16_t)ent[2 + i * 2] << 8);
        fat_lfn_store_char(base + i, ch);
    }
    for (uint32_t i = 0; i < 6; ++i) {
        uint16_t ch = (uint16_t)ent[14 + i * 2] | ((uint16_t)ent[15 + i * 2] << 8);
        fat_lfn_store_char(base + 5 + i, ch);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        uint16_t ch = (uint16_t)ent[28 + i * 2] | ((uint16_t)ent[29 + i * 2] << 8);
        fat_lfn_store_char(base + 11 + i, ch);
    }
    g_lfn_seen++;
}

static void
fat_emit_bytes(const uint8_t *data, uint32_t len)
{
    char buf[64];
    uint32_t pos = 0;
    for (uint32_t i = 0; i < len; ++i) {
        char c = (char)data[i];
        if (c < 0x20 || c > 0x7E) {
            c = '.';
        }
        buf[pos++] = c;
        if (pos == sizeof(buf) - 1) {
            buf[pos] = '\0';
            console_write(buf);
            pos = 0;
        }
    }
    if (pos > 0) {
        buf[pos] = '\0';
        console_write(buf);
    }
}

static int
fat_name_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (to_upper(a[i]) != to_upper(b[i])) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int
fat_name_has_dot(const char *name)
{
    uint32_t i = 0;
    if (!name) {
        return 0;
    }
    while (name[i]) {
        if (name[i] == '.') {
            return 1;
        }
        i++;
    }
    return 0;
}

static void
fat_build_read_names(void)
{
    g_read_name_ext[0] = '\0';
    g_read_name_alt[0] = '\0';
    g_read_name_alt_ext[0] = '\0';
    if (g_read_name[0] == '\0') {
        return;
    }
    if (fat_name_has_dot(g_read_name)) {
        return;
    }
    uint32_t has_dash = 0;
    uint32_t pos = 0;
    while (g_read_name[pos] && pos + 1 < sizeof(g_read_name_ext)) {
        char c = g_read_name[pos];
        if (c == '-') {
            has_dash = 1;
        }
        g_read_name_ext[pos] = g_read_name[pos];
        pos++;
    }
    g_read_name_ext[pos] = '\0';
    if (has_dash) {
        pos = 0;
        while (g_read_name[pos] && pos + 1 < sizeof(g_read_name_alt)) {
            char c = g_read_name[pos];
            if (c == '-') {
                c = '_';
            }
            g_read_name_alt[pos] = c;
            pos++;
        }
        g_read_name_alt[pos] = '\0';
    }
    const char *suffix = ".WASMOSAPP";
    uint32_t s = 0;
    while (suffix[s] && pos + 1 < sizeof(g_read_name_ext)) {
        g_read_name_ext[pos++] = suffix[s++];
    }
    g_read_name_ext[pos] = '\0';
    if (g_read_name_alt[0]) {
        pos = 0;
        while (g_read_name_alt[pos] && pos + 1 < sizeof(g_read_name_alt_ext)) {
            g_read_name_alt_ext[pos] = g_read_name_alt[pos];
            pos++;
        }
        s = 0;
        while (suffix[s] && pos + 1 < sizeof(g_read_name_alt_ext)) {
            g_read_name_alt_ext[pos++] = suffix[s++];
        }
        g_read_name_alt_ext[pos] = '\0';
    }
}

static uint32_t
fat_first_data_lba(void)
{
    return g_root_dir_lba + g_root_dir_sectors;
}

static uint32_t
fat_lba_for_cluster(uint16_t cluster)
{
    if (cluster < 2) {
        return 0;
    }
    return fat_first_data_lba() + (uint32_t)(cluster - 2) * g_sectors_per_cluster;
}

static int
fat_load_fat_bytes(uint32_t fat_offset, uint8_t *out_lo, uint8_t *out_hi)
{
    uint32_t fat_lba = g_boot_lba + g_reserved_sectors + (fat_offset / g_bytes_per_sector);
    uint32_t sector_offset = fat_offset % g_bytes_per_sector;

    if (!out_lo || !out_hi) {
        return -1;
    }
    if (fat_sync_block_read(fat_lba) != 0) {
        return -1;
    }
    *out_lo = g_sector_buf[sector_offset];
    if (sector_offset + 1u < g_bytes_per_sector) {
        *out_hi = g_sector_buf[sector_offset + 1u];
        return 0;
    }
    if (fat_sync_block_read(fat_lba + 1u) != 0) {
        return -1;
    }
    *out_hi = g_sector_buf[0];
    return 0;
}

static int
fat_next_cluster(uint16_t cluster, uint16_t *out_next)
{
    uint8_t lo = 0;
    uint8_t hi = 0;
    uint16_t value = 0;
    uint32_t fat_offset = 0;

    if (!out_next || cluster < 2) {
        return -1;
    }

    if (g_fat_type == FAT_TYPE_12) {
        fat_offset = cluster + (cluster / 2u);
        if (fat_load_fat_bytes(fat_offset, &lo, &hi) != 0) {
            return -1;
        }
        value = (uint16_t)lo | ((uint16_t)hi << 8);
        if (cluster & 1u) {
            value >>= 4;
        } else {
            value &= 0x0FFFu;
        }
        if (value >= 0x0FF8u) {
            return -1;
        }
        if (value < 2u) {
            return -1;
        }
        *out_next = value;
        return 0;
    }

    if (g_fat_type == FAT_TYPE_16) {
        fat_offset = (uint32_t)cluster * 2u;
        if (fat_load_fat_bytes(fat_offset, &lo, &hi) != 0) {
            return -1;
        }
        value = (uint16_t)lo | ((uint16_t)hi << 8);
        if (value >= 0xFFF8u) {
            return -1;
        }
        if (value < 2u) {
            return -1;
        }
        *out_next = value;
        return 0;
    }

    /* TODO: FAT32 needs a wider cluster type and root-directory handling
     * before cluster-chain walking can be extended beyond the current FAT12/16
     * scaffold. */
    return -1;
}

static int
fat_chdir_next_component(void)
{
    while (g_chdir_path[g_chdir_pos] == '/') {
        g_chdir_pos++;
    }
    if (!g_chdir_path[g_chdir_pos]) {
        return 0;
    }
    uint32_t len = 0;
    while (g_chdir_path[g_chdir_pos] && g_chdir_path[g_chdir_pos] != '/') {
        if (len + 1 >= sizeof(g_chdir_name)) {
            return -1;
        }
        g_chdir_name[len++] = g_chdir_path[g_chdir_pos++];
    }
    g_chdir_name[len] = '\0';
    if (g_chdir_name[0] == '.' && g_chdir_name[1] == '\0') {
        return fat_chdir_next_component();
    }
    if (g_chdir_name[0] == '.' && g_chdir_name[1] == '.' && g_chdir_name[2] == '\0') {
        g_chdir_root = 1;
        g_chdir_cluster = 0;
        g_chdir_dir_lba = 0;
        g_chdir_dir_sectors = 0;
        return fat_chdir_next_component();
    }
    return 1;
}

static int
fat_chdir_begin_dir(uint8_t root, uint16_t cluster)
{
    if (root) {
        g_chdir_dir_lba = g_root_dir_lba;
        g_chdir_dir_sectors = g_root_dir_sectors;
        g_op_sector = 0;
        g_op_entries_left = g_root_entry_count;
    } else {
        g_chdir_dir_lba = fat_lba_for_cluster(cluster);
        if (g_chdir_dir_lba == 0) {
            return -1;
        }
        g_chdir_dir_sectors = g_sectors_per_cluster;
        g_op_sector = 0;
        g_op_entries_left = (g_chdir_dir_sectors * g_bytes_per_sector) / 32u;
    }
    fat_lfn_reset();
    if (fat_send_block_read(g_chdir_dir_lba, 1) != 0) {
        return -1;
    }
    return 0;
}

static int
fat_handle_read_app(void)
{
    if (g_root_entry_count == 0 || g_root_dir_sectors == 0) {
        return -1;
    }

    if (g_op == FAT_OP_NONE) {
        g_op = FAT_OP_READ_APP;
        g_read_stage = FAT_READ_FIND_APPS;
        g_read_dir_lba = g_root_dir_lba;
        g_read_dir_sectors = g_root_dir_sectors;
        g_read_sector = 0;
        g_read_entries_left = g_root_entry_count;
        g_read_offset = 0;
        g_read_size = 0;
        g_read_max = (uint32_t)wasmos_fs_buffer_size();
        if (g_read_max == 0) {
            g_op = FAT_OP_NONE;
            return -1;
        }
        fat_build_read_names();
        fat_lfn_reset();
        if (fat_send_block_read(g_read_dir_lba, 1) != 0) {
            g_op = FAT_OP_NONE;
            return -1;
        }
        return FAT_WAITING;
    }

    if (g_op != FAT_OP_READ_APP) {
        return -1;
    }

    int rc = fat_poll_block_read();
    if (rc == FAT_WAITING) {
        return rc;
    }
    if (rc != 0) {
        g_op = FAT_OP_NONE;
        return -1;
    }

    if (g_read_stage == FAT_READ_FIND_APPS || g_read_stage == FAT_READ_FIND_FILE) {
        uint32_t entries_per_sector = g_bytes_per_sector / 32u;
        uint32_t entries_total = entries_per_sector;
        if (g_read_entries_left < entries_total) {
            entries_total = g_read_entries_left;
        }

        for (uint32_t i = 0; i < entries_total; ++i) {
            uint8_t *ent = g_sector_buf + i * 32u;
            if (ent[0] == 0x00) {
                g_op = FAT_OP_NONE;
                fat_lfn_reset();
                return -1;
            }
            if (ent[0] == 0xE5) {
                fat_lfn_reset();
                continue;
            }
            if ((ent[11] & 0x0F) == 0x0F) {
                fat_lfn_collect(ent);
                continue;
            }
            const char *entry_name = 0;
            if (g_lfn_valid && g_lfn_seen == g_lfn_total && g_lfn_buf[0]) {
                fat_lfn_finalize();
                entry_name = g_lfn_buf;
            }
            if (ent[11] & 0x08) {
                fat_lfn_reset();
                continue;
            }

            if (!entry_name) {
                char short_name[13];
                uint32_t pos = 0;
                for (int j = 0; j < 8; ++j) {
                    if (ent[j] != ' ') {
                        short_name[pos++] = (char)ent[j];
                    }
                }
                if (ent[8] != ' ') {
                    short_name[pos++] = '.';
                    for (int j = 0; j < 3; ++j) {
                        if (ent[8 + j] != ' ') {
                            short_name[pos++] = (char)ent[8 + j];
                        }
                    }
                }
                short_name[pos] = '\0';
                entry_name = short_name;
            }

            if (g_read_stage == FAT_READ_FIND_APPS) {
                if (!(ent[11] & 0x10)) {
                    fat_lfn_reset();
                    continue;
                }
                if (!fat_name_eq(entry_name, "APPS")) {
                    fat_lfn_reset();
                    continue;
                }
                uint16_t cluster = (uint16_t)ent[26] | ((uint16_t)ent[27] << 8);
                if (cluster < 2) {
                    g_op = FAT_OP_NONE;
                    fat_lfn_reset();
                    return -1;
                }
                g_read_dir_lba = fat_lba_for_cluster(cluster);
                g_read_dir_sectors = g_sectors_per_cluster;
                g_read_entries_left = (g_read_dir_sectors * g_bytes_per_sector) / 32u;
                g_read_sector = 0;
                g_read_stage = FAT_READ_FIND_FILE;
                fat_lfn_reset();
                if (fat_send_block_read(g_read_dir_lba, 1) != 0) {
                    g_op = FAT_OP_NONE;
                    return -1;
                }
                return FAT_WAITING;
            }

            if (ent[11] & 0x10) {
                fat_lfn_reset();
                continue;
            }
            if (!fat_name_eq(entry_name, g_read_name) &&
                !fat_name_eq(entry_name, g_read_name_ext) &&
                (!g_read_name_alt[0] || !fat_name_eq(entry_name, g_read_name_alt)) &&
                (!g_read_name_alt_ext[0] || !fat_name_eq(entry_name, g_read_name_alt_ext))) {
                fat_lfn_reset();
                continue;
            }
            uint16_t cluster = (uint16_t)ent[26] | ((uint16_t)ent[27] << 8);
            if (cluster < 2) {
                g_op = FAT_OP_NONE;
                fat_lfn_reset();
                return -1;
            }
            uint32_t file_size = (uint32_t)ent[28] |
                                 ((uint32_t)ent[29] << 8) |
                                 ((uint32_t)ent[30] << 16) |
                                 ((uint32_t)ent[31] << 24);
            if (file_size == 0 || file_size > g_read_max) {
                g_op = FAT_OP_NONE;
                fat_lfn_reset();
                return -1;
            }
            g_file_cluster = cluster;
            g_file_lba = fat_lba_for_cluster(cluster);
            g_file_remaining = file_size;
            g_file_sector = 0;
            g_read_offset = 0;
            g_read_size = file_size;
            g_read_stage = FAT_READ_FILE;
            fat_lfn_reset();
            if (fat_send_block_read(g_file_lba, 1) != 0) {
                g_op = FAT_OP_NONE;
                return -1;
            }
            return FAT_WAITING;
        }

        if (g_read_entries_left <= entries_total) {
            g_op = FAT_OP_NONE;
            fat_lfn_reset();
            return -1;
        }
        g_read_entries_left -= entries_total;
        g_read_sector++;
        if (g_read_sector >= g_read_dir_sectors) {
            g_op = FAT_OP_NONE;
            fat_lfn_reset();
            return -1;
        }
        if (fat_send_block_read(g_read_dir_lba + g_read_sector, 1) != 0) {
            g_op = FAT_OP_NONE;
            return -1;
        }
        return FAT_WAITING;
    }

    if (g_read_stage == FAT_READ_FILE) {
        if (g_file_remaining == 0) {
            g_fs_resp_override = 1;
            g_fs_resp_arg0 = (int32_t)g_read_size;
            g_op = FAT_OP_NONE;
            return 0;
        }
        uint32_t bytes = g_file_remaining > g_bytes_per_sector ? g_bytes_per_sector : g_file_remaining;
        if (g_read_offset + bytes > g_read_max) {
            g_op = FAT_OP_NONE;
            return -1;
        }
        if (wasmos_fs_buffer_write((int32_t)(uintptr_t)g_sector_buf,
                                   (int32_t)bytes,
                                   (int32_t)g_read_offset) != 0) {
            g_op = FAT_OP_NONE;
            return -1;
        }
        g_read_offset += bytes;
        g_file_remaining -= bytes;
        g_file_sector++;
        if (g_file_remaining == 0) {
            g_fs_resp_override = 1;
            g_fs_resp_arg0 = (int32_t)g_read_size;
            g_op = FAT_OP_NONE;
            return 0;
        }
        if (g_file_sector >= g_sectors_per_cluster) {
            uint16_t next_cluster = 0;
            if (fat_next_cluster(g_file_cluster, &next_cluster) != 0) {
                g_op = FAT_OP_NONE;
                return -1;
            }
            g_file_cluster = next_cluster;
            g_file_lba = fat_lba_for_cluster(next_cluster);
            g_file_sector = 0;
        }
        if (fat_send_block_read(g_file_lba + g_file_sector, 1) != 0) {
            g_op = FAT_OP_NONE;
            return -1;
        }
        return FAT_WAITING;
    }

    g_op = FAT_OP_NONE;
    return -1;
}

static int
fat_handle_chdir(void)
{
    if (g_root_entry_count == 0 || g_root_dir_sectors == 0) {
        return -1;
    }

    if (g_op == FAT_OP_NONE) {
        uint32_t i = 0;
        while (i + 1 < sizeof(g_chdir_path) && g_dir_name[i]) {
            g_chdir_path[i] = g_dir_name[i];
            i++;
        }
        g_chdir_path[i] = '\0';
        g_chdir_pos = 0;
        if (g_chdir_path[0] == '/') {
            g_chdir_root = 1;
            g_chdir_cluster = 0;
            g_chdir_pos = 1;
        } else {
            g_chdir_root = g_cwd_root;
            g_chdir_cluster = g_cwd_cluster;
        }

        int next = fat_chdir_next_component();
        if (next < 0) {
            return -1;
        }
        if (next == 0) {
            g_cwd_root = g_chdir_root;
            g_cwd_cluster = g_chdir_cluster;
            if (g_cwd_root) {
                g_dir_lba = 0;
                g_dir_sectors = 0;
            } else {
                g_dir_lba = fat_lba_for_cluster(g_cwd_cluster);
                g_dir_sectors = g_sectors_per_cluster;
            }
            g_cwd_source = g_fs_req.source;
            return 0;
        }

        g_op = FAT_OP_CHDIR;
        if (fat_chdir_begin_dir(g_chdir_root, g_chdir_cluster) != 0) {
            g_op = FAT_OP_NONE;
            return -1;
        }
        return FAT_WAITING;
    }

    if (g_op != FAT_OP_CHDIR) {
        return -1;
    }

    int rc = fat_poll_block_read();
    if (rc == FAT_WAITING) {
        return rc;
    }
    if (rc != 0) {
        g_op = FAT_OP_NONE;
        return -1;
    }

    uint32_t entries_per_sector = g_bytes_per_sector / 32u;
    uint32_t entries_total = entries_per_sector;
    if (g_op_entries_left < entries_total) {
        entries_total = g_op_entries_left;
    }

    for (uint32_t i = 0; i < entries_total; ++i) {
        uint8_t *ent = g_sector_buf + i * 32u;
        if (ent[0] == 0x00) {
            g_op = FAT_OP_NONE;
            fat_lfn_reset();
            return -1;
        }
        if (ent[0] == 0xE5) {
            fat_lfn_reset();
            continue;
        }
        if ((ent[11] & 0x0F) == 0x0F) {
            fat_lfn_collect(ent);
            continue;
        }
        const char *entry_name = 0;
    if (g_lfn_valid && g_lfn_seen == g_lfn_total && g_lfn_buf[0]) {
        fat_lfn_finalize();
        entry_name = g_lfn_buf;
    }
        if (!(ent[11] & 0x10)) {
            fat_lfn_reset();
            continue;
        }
        char entry[13];
        if (!entry_name) {
            uint32_t pos = 0;
            for (int j = 0; j < 8; ++j) {
                if (ent[j] != ' ') {
                    entry[pos++] = (char)ent[j];
                }
            }
            entry[pos] = '\0';
            entry_name = entry;
        }
        if (!fat_name_eq(entry_name, g_chdir_name)) {
            fat_lfn_reset();
            continue;
        }
        uint16_t cluster = (uint16_t)ent[26] | ((uint16_t)ent[27] << 8);
        if (cluster < 2) {
            g_op = FAT_OP_NONE;
            fat_lfn_reset();
            return -1;
        }
        g_chdir_root = 0;
        g_chdir_cluster = cluster;
        int next = fat_chdir_next_component();
        if (next < 0) {
            g_op = FAT_OP_NONE;
            fat_lfn_reset();
            return -1;
        }
        if (next == 0) {
            g_cwd_cluster = g_chdir_cluster;
            g_dir_lba = fat_lba_for_cluster(cluster);
            g_dir_sectors = g_sectors_per_cluster;
            g_cwd_root = 0;
            g_cwd_source = g_fs_req.source;
            g_op = FAT_OP_NONE;
            fat_lfn_reset();
            return 0;
        }
        if (fat_chdir_begin_dir(0, cluster) != 0) {
            g_op = FAT_OP_NONE;
            fat_lfn_reset();
            return -1;
        }
        return FAT_WAITING;
    }

    if (g_op_entries_left <= entries_total) {
        g_op = FAT_OP_NONE;
        return -1;
    }
    g_op_entries_left -= entries_total;
    g_op_sector++;
    if (g_op_sector >= g_chdir_dir_sectors) {
        g_op = FAT_OP_NONE;
        return -1;
    }
    if (fat_send_block_read(g_chdir_dir_lba + g_op_sector, 1) != 0) {
        g_op = FAT_OP_NONE;
        return -1;
    }
    return FAT_WAITING;
}

static int
fat_handle_list(void)
{
    if (g_root_entry_count == 0 || g_root_dir_sectors == 0) {
        fat_log("root listing unsupported\n");
        return -1;
    }
    if (!g_cwd_root && g_dir_lba == 0) {
        fat_log("cwd invalid\n");
        return -1;
    }

    if (g_op == FAT_OP_NONE) {
        g_op = g_cwd_root ? FAT_OP_LIST : FAT_OP_LIST_DIR;
        g_op_sector = 0;
        g_op_entries_left = g_cwd_root ? g_root_entry_count :
                           (g_dir_sectors * g_bytes_per_sector) / 32u;
        fat_lfn_reset();
        uint32_t start_lba = g_cwd_root ? g_root_dir_lba : g_dir_lba;
        if (fat_send_block_read(start_lba, 1) != 0) {
            fat_log("root read send failed\n");
            g_op = FAT_OP_NONE;
            return -1;
        }
        return FAT_WAITING;
    }

    if (g_op != FAT_OP_LIST && g_op != FAT_OP_LIST_DIR) {
        return -1;
    }

    int rc = fat_poll_block_read();
    if (rc == FAT_WAITING) {
        return rc;
    }
    if (rc != 0) {
        g_op = FAT_OP_NONE;
        return -1;
    }

    uint32_t entries_per_sector = g_bytes_per_sector / 32u;
    uint32_t entries_total = entries_per_sector;
    if (g_op_entries_left < entries_total) {
        entries_total = g_op_entries_left;
    }

    for (uint32_t i = 0; i < entries_total; ++i) {
        uint8_t *ent = g_sector_buf + i * 32u;
        if (ent[0] == 0x00) {
            g_op = FAT_OP_NONE;
            fat_lfn_reset();
            return 0;
        }
        if (ent[0] == 0xE5) {
            fat_lfn_reset();
            continue;
        }
        if ((ent[11] & 0x0F) == 0x0F) {
            fat_lfn_collect(ent);
            continue;
        }
        const char *entry_name = 0;
    if (g_lfn_valid && g_lfn_seen == g_lfn_total && g_lfn_buf[0]) {
        fat_lfn_finalize();
        entry_name = g_lfn_buf;
    }
        if (ent[11] & 0x08) {
            fat_lfn_reset();
            continue;
        }
        if (entry_name) {
            fat_write_full(entry_name);
        } else {
            char name[12];
            for (int j = 0; j < 8; ++j) {
                name[j] = (char)ent[j];
            }
            name[8] = '\0';
            char ext[4];
            for (int j = 0; j < 3; ++j) {
                ext[j] = (char)ent[8 + j];
            }
            ext[3] = '\0';

            uint32_t name_len = 8;
            while (name_len > 0 && name[name_len - 1] == ' ') {
                name_len--;
            }
            uint32_t ext_len = 3;
            while (ext_len > 0 && ext[ext_len - 1] == ' ') {
                ext_len--;
            }

            fat_write_name(name, name_len);
            if (ext_len > 0) {
                console_write(".");
                fat_write_name(ext, ext_len);
            }
        }
        console_write("\n");
        fat_lfn_reset();
    }

    if (g_op_entries_left <= entries_total) {
        g_op = FAT_OP_NONE;
        return 0;
    }

    g_op_entries_left -= entries_total;
    g_op_sector++;
    uint32_t limit = g_cwd_root ? g_root_dir_sectors : g_dir_sectors;
    if (g_op_sector >= limit) {
        g_op = FAT_OP_NONE;
        return 0;
    }
    uint32_t base = g_cwd_root ? g_root_dir_lba : g_dir_lba;
    if (fat_send_block_read(base + g_op_sector, 1) != 0) {
        fat_log("root read send failed\n");
        g_op = FAT_OP_NONE;
        return -1;
    }
    return FAT_WAITING;
}

static int
fat_handle_cat(void)
{
    if (g_root_entry_count == 0 || g_root_dir_sectors == 0) {
        return -1;
    }
    if (!g_cwd_root && g_dir_lba == 0) {
        return -1;
    }

    if (g_op == FAT_OP_NONE) {
        g_op = g_cwd_root ? FAT_OP_CAT : FAT_OP_CAT_DIR;
        g_cat_stage = FAT_CAT_SCAN;
        g_op_sector = 0;
        g_op_entries_left = g_cwd_root ? g_root_entry_count :
                           (g_dir_sectors * g_bytes_per_sector) / 32u;
        fat_lfn_reset();
        uint32_t start_lba = g_cwd_root ? g_root_dir_lba : g_dir_lba;
        if (fat_send_block_read(start_lba, 1) != 0) {
            fat_log("root read send failed\n");
            g_op = FAT_OP_NONE;
            return -1;
        }
        return FAT_WAITING;
    }

    if (g_op != FAT_OP_CAT && g_op != FAT_OP_CAT_DIR) {
        return -1;
    }

    int rc = fat_poll_block_read();
    if (rc == FAT_WAITING) {
        return rc;
    }
    if (rc != 0) {
        g_op = FAT_OP_NONE;
        return -1;
    }

    if (g_cat_stage == FAT_CAT_SCAN) {
        uint32_t entries_per_sector = g_bytes_per_sector / 32u;
        uint32_t entries_total = entries_per_sector;
        if (g_op_entries_left < entries_total) {
            entries_total = g_op_entries_left;
        }

        for (uint32_t i = 0; i < entries_total; ++i) {
            uint8_t *ent = g_sector_buf + i * 32u;
            if (ent[0] == 0x00) {
                g_op = FAT_OP_NONE;
                fat_lfn_reset();
                return -1;
            }
            if (ent[0] == 0xE5) {
                fat_lfn_reset();
                continue;
            }
            if ((ent[11] & 0x0F) == 0x0F) {
                fat_lfn_collect(ent);
                continue;
            }
            const char *entry_name = 0;
            if (g_lfn_valid && g_lfn_seen == g_lfn_total && g_lfn_buf[0]) {
                fat_lfn_finalize();
                entry_name = g_lfn_buf;
            }
            if (ent[11] & 0x08) {
                fat_lfn_reset();
                continue;
            }
            char name[13];
            if (!entry_name) {
                uint32_t pos = 0;
                for (int j = 0; j < 8; ++j) {
                    if (ent[j] != ' ') {
                        name[pos++] = (char)ent[j];
                    }
                }
                if (ent[8] != ' ') {
                    name[pos++] = '.';
                    for (int j = 0; j < 3; ++j) {
                        if (ent[8 + j] != ' ') {
                            name[pos++] = (char)ent[8 + j];
                        }
                    }
                }
                name[pos] = '\0';
                entry_name = name;
            }
            if (!entry_name || entry_name[0] == '\0') {
                fat_lfn_reset();
                continue;
            }
            int match = fat_name_eq(entry_name, g_target_name);
            if (!match) {
                fat_lfn_reset();
                continue;
            }

            if (ent[11] & 0x10) {
                fat_lfn_reset();
                continue;
            }
            uint16_t first_cluster = (uint16_t)ent[26] | ((uint16_t)ent[27] << 8);
            if (first_cluster < 2) {
                g_op = FAT_OP_NONE;
                fat_lfn_reset();
                return -1;
            }
            uint32_t file_size = (uint32_t)ent[28] |
                                 ((uint32_t)ent[29] << 8) |
                                 ((uint32_t)ent[30] << 16) |
                                 ((uint32_t)ent[31] << 24);
            if (file_size == 0) {
                console_write("\n");
                g_op = FAT_OP_NONE;
                fat_lfn_reset();
                return 0;
            }
            g_file_cluster = first_cluster;
            g_file_lba = fat_lba_for_cluster(first_cluster);
            g_file_remaining = file_size;
            g_file_sector = 0;
            g_cat_stage = FAT_CAT_FILE;
            if (fat_send_block_read(g_file_lba, 1) != 0) {
                fat_log("file read send failed\n");
                g_op = FAT_OP_NONE;
                fat_lfn_reset();
                return -1;
            }
            fat_lfn_reset();
            return FAT_WAITING;
        }

        if (g_op_entries_left <= entries_total) {
            g_op = FAT_OP_NONE;
            fat_lfn_reset();
            return -1;
        }

        g_op_entries_left -= entries_total;
        g_op_sector++;
        uint32_t limit = g_cwd_root ? g_root_dir_sectors : g_dir_sectors;
        if (g_op_sector >= limit) {
            g_op = FAT_OP_NONE;
            return -1;
        }
        uint32_t base = g_cwd_root ? g_root_dir_lba : g_dir_lba;
        if (fat_send_block_read(base + g_op_sector, 1) != 0) {
            fat_log("root read send failed\n");
            g_op = FAT_OP_NONE;
            return -1;
        }
        return FAT_WAITING;
    }

    if (g_cat_stage == FAT_CAT_FILE) {
        if (g_file_remaining == 0) {
            console_write("\n");
            g_op = FAT_OP_NONE;
            return 0;
        }

        uint32_t bytes = g_file_remaining > g_bytes_per_sector ? g_bytes_per_sector : g_file_remaining;
        fat_emit_bytes(g_sector_buf, bytes);
        g_file_remaining -= bytes;
        g_file_sector++;

        if (g_file_remaining == 0) {
            console_write("\n");
            g_op = FAT_OP_NONE;
            return 0;
        }
        if (g_file_sector >= g_sectors_per_cluster) {
            uint16_t next_cluster = 0;
            if (fat_next_cluster(g_file_cluster, &next_cluster) != 0) {
                fat_log("file chain invalid\n");
                g_op = FAT_OP_NONE;
                return -1;
            }
            g_file_cluster = next_cluster;
            g_file_lba = fat_lba_for_cluster(next_cluster);
            g_file_sector = 0;
        }

        if (fat_send_block_read(g_file_lba + g_file_sector, 1) != 0) {
            fat_log("file read send failed\n");
            g_op = FAT_OP_NONE;
            return -1;
        }
        return FAT_WAITING;
    }

    g_op = FAT_OP_NONE;
    return -1;
}

WASMOS_WASM_EXPORT int32_t
fat_ipc_dispatch(int32_t type,
                 int32_t arg0,
                 int32_t arg1,
                 int32_t arg2,
                 int32_t arg3)
{
    int rc = fat_ensure_ready();
    if (rc != 0) {
        return rc;
    }

    if (type == FS_IPC_LIST_ROOT_REQ) {
        if (g_fs_req.source != g_cwd_source) {
            g_cwd_root = 1;
        }
        return fat_handle_list();
    }
    if (type == FS_IPC_CAT_ROOT_REQ) {
        if (g_op == FAT_OP_NONE) {
            fat_unpack_name((uint32_t)arg0, (uint32_t)arg1, (uint32_t)arg2, (uint32_t)arg3,
                            g_target_name, sizeof(g_target_name));
        }
        if (g_fs_req.source != g_cwd_source) {
            g_cwd_root = 1;
        }
        return fat_handle_cat();
    }
    if (type == FS_IPC_CHDIR_REQ) {
        if (g_op != FAT_OP_NONE && g_op != FAT_OP_CHDIR) {
            return -1;
        }
        if (g_op == FAT_OP_NONE) {
            fat_unpack_name((uint32_t)arg0, (uint32_t)arg1, (uint32_t)arg2, (uint32_t)arg3,
                            g_dir_name, sizeof(g_dir_name));
            if (g_dir_name[0] == '\0' || (g_dir_name[0] == '/' && g_dir_name[1] == '\0')) {
                g_cwd_root = 1;
                g_cwd_source = g_fs_req.source;
                return 0;
            }
        }
        return fat_handle_chdir();
    }
    if (type == FS_IPC_READ_APP_REQ) {
        if (g_op != FAT_OP_NONE && g_op != FAT_OP_READ_APP) {
            return -1;
        }
        if (g_op == FAT_OP_NONE) {
            fat_unpack_name((uint32_t)arg0, (uint32_t)arg1, (uint32_t)arg2, (uint32_t)arg3,
                            g_read_name, sizeof(g_read_name));
            g_read_name_ext[0] = '\0';
        }
        return fat_handle_read_app();
    }
    if (type == FS_IPC_OPEN_REQ) {
        if (g_op != FAT_OP_NONE) {
            return -1;
        }
        return fat_handle_open();
    }
    if (type == FS_IPC_STAT_REQ) {
        if (g_op != FAT_OP_NONE) {
            return -1;
        }
        return fat_handle_stat();
    }
    if (type == FS_IPC_READY_REQ) {
        if (g_op != FAT_OP_NONE) {
            return -1;
        }
        return 0;
    }
    if (type == FS_IPC_READ_REQ) {
        if (g_op != FAT_OP_NONE) {
            return -1;
        }
        return fat_handle_read_open_file();
    }
    if (type == FS_IPC_WRITE_REQ) {
        if (g_op != FAT_OP_NONE) {
            return -1;
        }
        return fat_handle_write_open_file();
    }
    if (type == FS_IPC_CLOSE_REQ) {
        if (g_op != FAT_OP_NONE) {
            return -1;
        }
        return fat_handle_close_open_file();
    }
    if (type == FS_IPC_SEEK_REQ) {
        if (g_op != FAT_OP_NONE) {
            return -1;
        }
        return fat_handle_seek_open_file();
    }

    return -1;
}

static int
fat_send_fs_response(int32_t status)
{
    int32_t type = status == 0 ? FS_IPC_RESP : FS_IPC_ERROR;
    int32_t arg0 = status;
    int32_t arg1 = 0;
    if (g_fs_resp_override && type == FS_IPC_RESP) {
        arg0 = g_fs_resp_arg0;
        arg1 = g_fs_resp_arg1;
    }
    g_fs_resp_override = 0;
    g_fs_resp_arg0 = 0;
    g_fs_resp_arg1 = 0;
    return wasmos_ipc_send(g_fs_req.source,
                           g_fs_endpoint,
                           type,
                           g_fs_req.request_id,
                           arg0,
                           arg1,
                           0,
                           0);
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t block_endpoint,
           int32_t fs_endpoint,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
    (void)ignored_arg2;
    (void)ignored_arg3;

    g_block_endpoint = block_endpoint;
    g_fs_endpoint = fs_endpoint;
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_reply_endpoint < 0) {
        fat_log("failed to create reply endpoint\n");
        fat_stall();
    }
    g_block_buf_phys = wasmos_block_buffer_phys();
    if (g_block_buf_phys < 0) {
        fat_log("block buffer missing\n");
        fat_stall();
    }
    g_boot_phase = FAT_BOOT_INIT;
    g_op = FAT_OP_NONE;
    g_waiting = 0;
    g_wait_resp_type = 0;
    g_wait_copy_into_sector = 0;
    g_fs_req.in_use = 0;
    g_fs_resp_override = 0;
    g_fs_resp_arg0 = 0;
    g_fs_resp_arg1 = 0;
    g_cwd_root = 1;
    g_cwd_source = -1;
    g_cwd_cluster = 0;
    g_dir_lba = 0;
    g_dir_sectors = 0;
    g_read_name[0] = '\0';
    g_read_name_ext[0] = '\0';
    g_read_name_alt[0] = '\0';
    g_read_name_alt_ext[0] = '\0';
    g_file_cluster = 0;
    for (uint32_t i = 0; i < FAT_MAX_OPEN_FILES; ++i) {
        g_open_files[i].in_use = 0;
        g_open_files[i].owner = -1;
        g_open_files[i].flags = 0;
        g_open_files[i].first_cluster = 0;
        g_open_files[i].current_cluster = 0;
        g_open_files[i].current_sector = 0;
        g_open_files[i].file_lba = 0;
        g_open_files[i].size = 0;
        g_open_files[i].offset = 0;
    }

    for (;;) {
        if (g_fs_req.in_use) {
            int rc = fat_ipc_dispatch(g_fs_req.type,
                                      g_fs_req.arg0,
                                      g_fs_req.arg1,
                                      g_fs_req.arg2,
                                      g_fs_req.arg3);
            if (rc == FAT_WAITING) {
                continue;
            }
            fat_send_fs_response(rc);
            g_fs_req.in_use = 0;
            continue;
        }

        int32_t recv_rc = wasmos_ipc_recv(g_fs_endpoint);
        if (recv_rc < 0) {
            continue;
        }

        g_fs_req.type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        g_fs_req.request_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        g_fs_req.arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        g_fs_req.arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        g_fs_req.arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        g_fs_req.arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
        g_fs_req.source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
        g_fs_req.in_use = 1;
    }
    return 0;
}
