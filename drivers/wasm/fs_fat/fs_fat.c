#include <stdint.h>
#include "wasmos_driver_abi.h"

#if defined(__wasm__)
#define WASMOS_WASM_IMPORT(module_name, symbol_name) \
    __attribute__((import_module(module_name), import_name(symbol_name)))
#define WASMOS_WASM_EXPORT __attribute__((visibility("default")))
#else
#define WASMOS_WASM_IMPORT(module_name, import_name)
#define WASMOS_WASM_EXPORT
#endif

extern int32_t wasmos_ipc_create_endpoint(void)
    WASMOS_WASM_IMPORT("wasmos", "ipc_create_endpoint");
extern int32_t wasmos_ipc_send(int32_t destination_endpoint,
                               int32_t source_endpoint,
                               int32_t type,
                               int32_t request_id,
                               int32_t arg0,
                               int32_t arg1,
                               int32_t arg2,
                               int32_t arg3)
    WASMOS_WASM_IMPORT("wasmos", "ipc_send");
extern int32_t wasmos_ipc_recv(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_recv");
extern int32_t wasmos_ipc_last_field(int32_t field)
    WASMOS_WASM_IMPORT("wasmos", "ipc_last_field");
extern int32_t wasmos_console_write(int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "console_write");
extern int32_t wasmos_block_buffer_phys(void)
    WASMOS_WASM_IMPORT("wasmos", "block_buffer_phys");
extern int32_t wasmos_block_buffer_copy(int32_t phys, int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "block_buffer_copy");

#define FAT_SECTOR_SIZE 512u
#define FAT_MAX_SECTOR_BYTES 4096u

typedef enum {
    FAT_BOOT_INIT = 0,
    FAT_BOOT_WAIT,
    FAT_BOOT_READY,
    FAT_BOOT_FAILED
} fat_boot_phase_t;

typedef enum {
    FAT_OP_NONE = 0,
    FAT_OP_LIST,
    FAT_OP_CAT
} fat_op_t;

typedef enum {
    FAT_CAT_SCAN = 0,
    FAT_CAT_FILE
} fat_cat_stage_t;

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
static uint32_t g_root_dir_lba = 0;
static uint32_t g_root_dir_sectors = 0;

static fat_op_t g_op = FAT_OP_NONE;
static fat_cat_stage_t g_cat_stage = FAT_CAT_SCAN;
static uint32_t g_op_sector = 0;
static uint32_t g_op_entries_left = 0;
static uint32_t g_file_remaining = 0;
static uint32_t g_file_lba = 0;
static uint32_t g_file_sector = 0;
static char g_target_name[16];

static int32_t g_waiting = 0;
static uint32_t g_wait_lba = 0;
static uint32_t g_wait_count = 0;
static int32_t g_wait_req_id = 0;

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
    int32_t len = 0;
    while (s && s[len]) {
        len++;
    }
    return len;
}

static char
to_upper(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
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

static int
fat_send_block_read(uint32_t lba, uint32_t count)
{
    if (g_block_endpoint < 0 || g_reply_endpoint < 0 || g_block_buf_phys < 0) {
        return -1;
    }
    g_wait_lba = lba;
    g_wait_count = count;
    g_wait_req_id = g_block_req_id++;
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
fat_poll_block_read(void)
{
    if (!g_waiting) {
        return 0;
    }

    int32_t recv_rc = wasmos_ipc_recv(g_reply_endpoint);
    if (recv_rc == 0) {
        return WASMOS_WASM_STEP_BLOCKED;
    }
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
        fat_log("block read error\n");
        return -1;
    }
    if (resp_type != BLOCK_IPC_READ_RESP) {
        g_waiting = 0;
        fat_log("block resp type bad\n");
        return -1;
    }

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
    g_waiting = 0;
    return 0;
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
        fat_log("FAT32 detected\n");
    } else if (cluster_count < 4085) {
        fat_log("FAT12 detected\n");
    } else if (cluster_count < 65525) {
        fat_log("FAT16 detected\n");
    } else {
        fat_log("FAT32 detected\n");
    }

    fat_log("bytes/sector=0x0000000000000200\n");
    return 0;
}

static int
fat_ensure_ready(void)
{
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
        return WASMOS_WASM_STEP_BLOCKED;
    }

    if (g_boot_phase == FAT_BOOT_WAIT) {
        int rc = fat_poll_block_read();
        if (rc != 0) {
            return rc;
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
                return WASMOS_WASM_STEP_BLOCKED;
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

static uint32_t
fat_first_data_lba(void)
{
    return g_root_dir_lba + g_root_dir_sectors;
}

static int
fat_handle_list(void)
{
    if (g_root_entry_count == 0 || g_root_dir_sectors == 0) {
        fat_log("root listing unsupported\n");
        return -1;
    }

    if (g_op == FAT_OP_NONE) {
        g_op = FAT_OP_LIST;
        g_op_sector = 0;
        g_op_entries_left = g_root_entry_count;
        if (fat_send_block_read(g_root_dir_lba, 1) != 0) {
            fat_log("root read send failed\n");
            g_op = FAT_OP_NONE;
            return -1;
        }
        return WASMOS_WASM_STEP_BLOCKED;
    }

    if (g_op != FAT_OP_LIST) {
        return -1;
    }

    int rc = fat_poll_block_read();
    if (rc != 0) {
        return rc;
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
            return 0;
        }
        if (ent[0] == 0xE5) {
            continue;
        }
        if (ent[11] & 0x08) {
            continue;
        }
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
        console_write("\n");
    }

    if (g_op_entries_left <= entries_total) {
        g_op = FAT_OP_NONE;
        return 0;
    }

    g_op_entries_left -= entries_total;
    g_op_sector++;
    if (g_op_sector >= g_root_dir_sectors) {
        g_op = FAT_OP_NONE;
        return 0;
    }
    if (fat_send_block_read(g_root_dir_lba + g_op_sector, 1) != 0) {
        fat_log("root read send failed\n");
        g_op = FAT_OP_NONE;
        return -1;
    }
    return WASMOS_WASM_STEP_BLOCKED;
}

static int
fat_handle_cat(void)
{
    if (g_root_entry_count == 0 || g_root_dir_sectors == 0) {
        return -1;
    }

    if (g_op == FAT_OP_NONE) {
        g_op = FAT_OP_CAT;
        g_cat_stage = FAT_CAT_SCAN;
        g_op_sector = 0;
        g_op_entries_left = g_root_entry_count;
        if (fat_send_block_read(g_root_dir_lba, 1) != 0) {
            fat_log("root read send failed\n");
            g_op = FAT_OP_NONE;
            return -1;
        }
        return WASMOS_WASM_STEP_BLOCKED;
    }

    if (g_op != FAT_OP_CAT) {
        return -1;
    }

    int rc = fat_poll_block_read();
    if (rc != 0) {
        return rc;
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
                return -1;
            }
            if (ent[0] == 0xE5) {
                continue;
            }
            if (ent[11] & 0x08) {
                continue;
            }
            char name[13];
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

            if (pos == 0) {
                continue;
            }
            int match = 1;
            for (uint32_t k = 0; k <= pos; ++k) {
                if (to_upper(name[k]) != to_upper(g_target_name[k])) {
                    match = 0;
                    break;
                }
            }
            if (!match) {
                continue;
            }

            uint16_t first_cluster = (uint16_t)ent[26] | ((uint16_t)ent[27] << 8);
            if (first_cluster < 2) {
                g_op = FAT_OP_NONE;
                return -1;
            }
            uint32_t file_size = (uint32_t)ent[28] |
                                 ((uint32_t)ent[29] << 8) |
                                 ((uint32_t)ent[30] << 16) |
                                 ((uint32_t)ent[31] << 24);
            g_file_lba = fat_first_data_lba() +
                         (uint32_t)(first_cluster - 2) * g_sectors_per_cluster;
            g_file_remaining = file_size;
            g_file_sector = 0;
            g_cat_stage = FAT_CAT_FILE;
            if (fat_send_block_read(g_file_lba, 1) != 0) {
                fat_log("file read send failed\n");
                g_op = FAT_OP_NONE;
                return -1;
            }
            return WASMOS_WASM_STEP_BLOCKED;
        }

        if (g_op_entries_left <= entries_total) {
            g_op = FAT_OP_NONE;
            return -1;
        }

        g_op_entries_left -= entries_total;
        g_op_sector++;
        if (g_op_sector >= g_root_dir_sectors) {
            g_op = FAT_OP_NONE;
            return -1;
        }
        if (fat_send_block_read(g_root_dir_lba + g_op_sector, 1) != 0) {
            fat_log("root read send failed\n");
            g_op = FAT_OP_NONE;
            return -1;
        }
        return WASMOS_WASM_STEP_BLOCKED;
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

        if (g_file_remaining == 0 || g_file_sector >= g_sectors_per_cluster) {
            console_write("\n");
            g_op = FAT_OP_NONE;
            return 0;
        }

        if (fat_send_block_read(g_file_lba + g_file_sector, 1) != 0) {
            fat_log("file read send failed\n");
            g_op = FAT_OP_NONE;
            return -1;
        }
        return WASMOS_WASM_STEP_BLOCKED;
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
    (void)arg1;

    int rc = fat_ensure_ready();
    if (rc != 0) {
        return rc;
    }

    if (type == FS_IPC_LIST_ROOT_REQ) {
        return fat_handle_list();
    }
    if (type == FS_IPC_CAT_ROOT_REQ) {
        if (g_op == FAT_OP_NONE) {
            fat_unpack_name((uint32_t)arg0, (uint32_t)arg1, (uint32_t)arg2, (uint32_t)arg3,
                            g_target_name, sizeof(g_target_name));
        }
        return fat_handle_cat();
    }

    return -1;
}

static int
fat_send_fs_response(int32_t status)
{
    int32_t type = status == 0 ? FS_IPC_RESP : FS_IPC_ERROR;
    return wasmos_ipc_send(g_fs_req.source,
                           g_fs_endpoint,
                           type,
                           g_fs_req.request_id,
                           status,
                           0,
                           0,
                           0);
}

WASMOS_WASM_EXPORT int32_t
fat_step(int32_t ignored_type,
         int32_t block_endpoint,
         int32_t fs_endpoint,
         int32_t ignored_arg2,
         int32_t ignored_arg3)
{
    (void)ignored_type;
    (void)ignored_arg2;
    (void)ignored_arg3;

    if (g_fs_endpoint < 0) {
        g_block_endpoint = block_endpoint;
        g_fs_endpoint = fs_endpoint;
        g_reply_endpoint = wasmos_ipc_create_endpoint();
        if (g_reply_endpoint < 0) {
            return WASMOS_WASM_STEP_FAILED;
        }
        g_block_buf_phys = wasmos_block_buffer_phys();
        if (g_block_buf_phys < 0) {
            return WASMOS_WASM_STEP_FAILED;
        }
        g_boot_phase = FAT_BOOT_INIT;
        g_op = FAT_OP_NONE;
        g_waiting = 0;
        g_fs_req.in_use = 0;
        return WASMOS_WASM_STEP_YIELDED;
    }

    if (g_fs_req.in_use) {
        int rc = fat_ipc_dispatch(g_fs_req.type,
                                  g_fs_req.arg0,
                                  g_fs_req.arg1,
                                  g_fs_req.arg2,
                                  g_fs_req.arg3);
        if (rc == WASMOS_WASM_STEP_BLOCKED) {
            return WASMOS_WASM_STEP_BLOCKED;
        }
        fat_send_fs_response(rc);
        g_fs_req.in_use = 0;
        return WASMOS_WASM_STEP_YIELDED;
    }

    int32_t recv_rc = wasmos_ipc_recv(g_fs_endpoint);
    if (recv_rc == 0) {
        return WASMOS_WASM_STEP_BLOCKED;
    }
    if (recv_rc < 0) {
        return WASMOS_WASM_STEP_FAILED;
    }

    g_fs_req.type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    g_fs_req.request_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    g_fs_req.arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    g_fs_req.arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    g_fs_req.arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
    g_fs_req.arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
    g_fs_req.source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
    g_fs_req.in_use = 1;

    int rc = fat_ipc_dispatch(g_fs_req.type,
                              g_fs_req.arg0,
                              g_fs_req.arg1,
                              g_fs_req.arg2,
                              g_fs_req.arg3);
    if (rc == WASMOS_WASM_STEP_BLOCKED) {
        return WASMOS_WASM_STEP_BLOCKED;
    }
    fat_send_fs_response(rc);
    g_fs_req.in_use = 0;
    return WASMOS_WASM_STEP_YIELDED;
}
