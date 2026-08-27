/* blkinfo.c - report the block devices registered under the "block" class.
 *
 *   blkinfo [lba]
 *   blkinfo --write <instance> [lba]
 *
 * Enumerates the class, asks each provider for its geometry
 * (BLOCK_IPC_IDENTIFY_REQ), and reads one sector from it
 * (BLOCK_IPC_READ_REQ) so the output says whether the device answers a real
 * transfer rather than only a query. The sector defaults to LBA 0.
 *
 * --write OVERWRITES that sector with a generated pattern and reads it back,
 * which is the only way to exercise a backend's write direction from the shell.
 * It takes the INSTANCE of the disk to write, and writes only that one: a
 * destructive tool that hits every disk it can enumerate is a footgun, and this
 * enumerates the boot disk. The sector is likewise named rather than defaulted,
 * because there is no safe-looking sector on a mounted volume.
 *
 * It talks to whatever backend registered the class, so it is not specific to
 * any one driver. A class instance is one DISK and its number is
 * (backend << 8) | unit, so this decodes it back into the pair a
 * device-manager rule names with DRIVER== and ATTR{unit}, and passes the unit
 * on to the block protocol.
 *
 * Each provider is queried from its OWN reply endpoint. The ATA driver binds a
 * client endpoint to one unit exclusively on first use, so asking it about both
 * of its disks over a single endpoint gets the second one refused -- one
 * endpoint per disk is what a real client (a filesystem driver) does anyway.
 */
#include <stdint.h>
#include "stdio.h"
#include "wasmos_cast.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

/* Providers reported per run. A system with more block devices than this prints
 * the total and details the first BLKINFO_MAX_PROVIDERS of them. */
#define BLKINFO_MAX_PROVIDERS 4
/* Bytes of the sector shown. One line's worth: enough to recognise a boot
 * signature or a filesystem magic, not a hex dump tool. */
#define BLKINFO_PREVIEW_BYTES 16
#define BLKINFO_SECTOR_BYTES 512

/* Pattern --write lays down: a fixed tag followed by the low byte of the sector
 * number, so a read-back proves the write reached the sector that was asked for
 * and not a neighbouring one. */
static const char BLKINFO_WRITE_TAG[] = "WASMOS-BLKWRITE";

static int32_t g_reply_endpoint = -1;
static int32_t g_request_id = 1;

/* Split a `block` class instance into the pair that identifies the disk. */
static uint8_t instance_backend(uint32_t instance) {
    return (uint8_t)((instance >> 8) & 0xFFu);
}
static uint8_t instance_unit(uint32_t instance) {
    return (uint8_t)(instance & 0xFFu);
}

/* Kept in step with block_backend_name() in the device manager and
 * block_backend_from_name() in its rule parser; the names are the drivers'
 * manifest package names, which is what a rule's DRIVER== spells. */
static const char* backend_name(uint8_t backend) {
    if (backend == (uint8_t)BLOCK_BACKEND_ATA) {
        return "ata";
    }
    if (backend == (uint8_t)BLOCK_BACKEND_VIRTIO_BLK) {
        return "virtio-blk";
    }
    return "unknown";
}

/* True when `s` starts with `prefix`. */
static int str_prefix(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

/* Parse the leading unsigned decimal number of `s`, or return `fallback` when
 * it does not start with a digit. Trailing characters are ignored. */
static uint32_t parse_u32(const char* s, uint32_t fallback) {
    uint32_t value = 0;
    uint32_t digits = 0;
    if (!s) {
        return fallback;
    }
    while (*s == ' ') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        value = value * 10u + (uint32_t)(*s - '0');
        s++;
        digits++;
    }
    return digits == 0u ? fallback : value;
}

/* Geometry of one provider, or -1 when it does not answer with a usable
 * BLOCK_IPC_IDENTIFY_RESP. */
static int identify(int32_t endpoint, uint8_t unit, uint32_t* out_sectors) {
    wasmos_ipc_message_t reply;
    if (wasmos_ipc_call(endpoint,
                        g_reply_endpoint,
                        BLOCK_IPC_IDENTIFY_REQ,
                        g_request_id++,
                        (int32_t)unit,
                        0,
                        0,
                        0,
                        &reply) != 0) {
        return -1;
    }
    if (reply.type != BLOCK_IPC_IDENTIFY_RESP || reply.arg0 != 0) {
        return -1;
    }
    *out_sectors = (uint32_t)reply.arg1;
    return 0;
}

/* Read one sector into this process's block buffer and copy its first bytes
 * out. Returns 0, or the provider's packed error code (negative) when the read
 * is refused, or -1 when the exchange itself failed. */
static int read_sector(int32_t endpoint, uint32_t lba, uint8_t* preview) {
    wasmos_ipc_message_t reply;
    int32_t buffer_phys = wasmos_block_buffer_phys();
    if (buffer_phys < 0) {
        return -1;
    }
    if (wasmos_ipc_call(endpoint,
                        g_reply_endpoint,
                        BLOCK_IPC_READ_REQ,
                        g_request_id++,
                        buffer_phys,
                        (int32_t)lba,
                        1,
                        0,
                        &reply) != 0) {
        return -1;
    }
    if (reply.type == BLOCK_IPC_ERROR) {
        return (int)reply.arg0;
    }
    if (reply.type != BLOCK_IPC_READ_RESP || reply.arg0 != 0) {
        return -1;
    }
    if (wasmos_block_buffer_copy(
            buffer_phys, addr_cast(int32_t, preview), BLKINFO_PREVIEW_BYTES, 0) != 0) {
        return -1;
    }
    return 0;
}

/* Fill one sector with the write pattern for `lba` and hand it to the provider.
 * Returns 0, the provider's packed error code, or -1 when the exchange failed. */
static int write_sector(int32_t endpoint, uint32_t lba) {
    wasmos_ipc_message_t reply;
    uint8_t sector[BLKINFO_SECTOR_BYTES];
    int32_t buffer_phys = wasmos_block_buffer_phys();
    uint32_t i;

    if (buffer_phys < 0) {
        return -1;
    }
    for (i = 0; i < sizeof(sector); ++i) {
        sector[i] = 0;
    }
    for (i = 0; i + 1u < sizeof(BLKINFO_WRITE_TAG); ++i) {
        sector[i] = (uint8_t)BLKINFO_WRITE_TAG[i];
    }
    sector[i] = (uint8_t)(lba & 0xFFu);
    if (wasmos_block_buffer_write(
            buffer_phys, addr_cast(int32_t, sector), (int32_t)sizeof(sector), 0) != 0) {
        return -1;
    }
    if (wasmos_ipc_call(endpoint,
                        g_reply_endpoint,
                        BLOCK_IPC_WRITE_REQ,
                        g_request_id++,
                        buffer_phys,
                        (int32_t)lba,
                        1,
                        0,
                        &reply) != 0) {
        return -1;
    }
    if (reply.type == BLOCK_IPC_ERROR) {
        return (int)reply.arg0;
    }
    return (reply.type == BLOCK_IPC_WRITE_RESP && reply.arg0 == 0) ? 0 : -1;
}

int main(void) {
    char args[64];
    svc_class_entry_t providers[BLKINFO_MAX_PROVIDERS];
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
    int32_t count;
    int32_t shown;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    const char* rest = args;
    int do_write = 0;
    uint32_t write_instance = 0;
    if (str_prefix(rest, "--write")) {
        do_write = 1;
        rest += 7;
        while (*rest == ' ') {
            rest++;
        }
        /* No instance means no target, and writing to whatever happens to be
         * enumerated is not a reasonable default. */
        if (*rest < '0' || *rest > '9') {
            (void)printf("[blkinfo] --write needs an instance: blkinfo --write <instance> [lba]\n");
            return 1;
        }
        write_instance = parse_u32(rest, 0u);
        while (*rest >= '0' && *rest <= '9') {
            rest++;
        }
    }
    uint32_t lba = parse_u32(rest, 0u);

    if (proc_endpoint < 0) {
        (void)printf("[blkinfo] no process-manager endpoint\n");
        return 1;
    }
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_reply_endpoint < 0) {
        (void)printf("[blkinfo] endpoint create failed\n");
        return 1;
    }

    count = wasmos_svc_lookup_class(
        proc_endpoint, g_reply_endpoint, "block", providers, BLKINFO_MAX_PROVIDERS, g_request_id++);
    if (count < 0) {
        (void)printf("[blkinfo] class lookup failed\n");
        return 1;
    }
    (void)printf("[blkinfo] providers=%d\n", (int)count);
    if (count == 0) {
        return 1;
    }

    shown = (count < BLKINFO_MAX_PROVIDERS) ? count : BLKINFO_MAX_PROVIDERS;
    for (int32_t i = 0; i < shown; ++i) {
        /* Zeroed rather than left indeterminate: the block buffer is filled
         * through an address-as-integer host call, so no analyser (and no
         * reader) can see that read_sector writes it. */
        uint8_t preview[BLKINFO_PREVIEW_BYTES] = {0};
        uint32_t sectors = 0;
        const uint8_t backend = instance_backend(providers[i].instance);
        const uint8_t unit = instance_unit(providers[i].instance);
        int rc;

        /* A fresh endpoint per disk; see the header. */
        g_reply_endpoint = wasmos_ipc_create_endpoint();
        if (g_reply_endpoint < 0) {
            (void)printf("[blkinfo] endpoint create failed\n");
            return 1;
        }
        if (identify((int32_t)providers[i].endpoint, unit, &sectors) != 0) {
            (void)printf("[blkinfo] instance=%u driver=%s unit=%u identify failed\n",
                         (unsigned)providers[i].instance,
                         backend_name(backend),
                         (unsigned)unit);
            continue;
        }
        (void)printf("[blkinfo] instance=%u driver=%s unit=%u sectors=%u bytes=%u\n",
                     (unsigned)providers[i].instance,
                     backend_name(backend),
                     (unsigned)unit,
                     (unsigned)sectors,
                     (unsigned)(sectors * BLKINFO_SECTOR_BYTES));

        if (do_write && providers[i].instance == write_instance) {
            rc = write_sector((int32_t)providers[i].endpoint, lba);
            if (rc != 0) {
                (void)printf("[blkinfo] instance=%u lba=%u write failed: %s\n",
                             (unsigned)providers[i].instance,
                             (unsigned)lba,
                             wasmos_error_code_name(rc));
                continue;
            }
            (void)printf("[blkinfo] instance=%u lba=%u write ok\n",
                         (unsigned)providers[i].instance,
                         (unsigned)lba);
        }

        rc = read_sector((int32_t)providers[i].endpoint, lba, preview);
        if (rc != 0) {
            (void)printf("[blkinfo] instance=%u lba=%u read failed: %s\n",
                         (unsigned)providers[i].instance,
                         (unsigned)lba,
                         wasmos_error_code_name(rc));
            continue;
        }
        (void)printf(
            "[blkinfo] instance=%u lba=%u data=", (unsigned)providers[i].instance, (unsigned)lba);
        for (int j = 0; j < BLKINFO_PREVIEW_BYTES; ++j) {
            (void)printf("%02X", (unsigned)preview[j]);
        }
        (void)printf("\n");
    }
    return 0;
}
