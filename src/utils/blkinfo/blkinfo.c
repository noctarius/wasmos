/* blkinfo.c - report the block devices registered under the "block" class.
 *
 *   blkinfo [lba]
 *   blkinfo --write <id> <lba>
 *
 * Enumerates the class, asks each provider to describe itself
 * (BLOCK_IPC_IDENTIFY_REQ), and reads one sector from it
 * (BLOCK_IPC_READ_REQ) so the output says whether the device answers a real
 * transfer rather than only a query. The sector defaults to LBA 0.
 *
 * --write OVERWRITES that sector with a generated pattern and reads it back,
 * which is the only way to exercise a backend's write direction from the shell.
 * BOTH the device and the sector must be given, and neither defaults: this
 * enumerates the boot disk, so a tool that wrote to whatever it found, or to
 * sector 0 because none was named, would be a footgun. There is no
 * safe-looking sector on a mounted volume.
 *
 * It talks to whatever backend registered the class, so it is not specific to
 * any one driver. A device is named by its CANONICAL ID (`block:ata:0`), which
 * is what every line here prints: the class instance is an opaque fingerprint of
 * that string and carries nothing a reader could decode or retype. Attributes --
 * backend, unit, capacity -- come out of the descriptor the provider returns.
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
#include "wasmos/libsys.h" /* wasmos_sys_streq */
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
/* Transfer buffer holding the wasmos_block_request_t of one transfer, acquired
 * and lent to the backend for the request and released after it. blkinfo issues
 * one transfer at a time, so one slot is enough. */
static int32_t g_req_bid = -1;

/* Send a block transfer request described by `req` and wait for its reply.
 * Returns the reply type, or 0 when the exchange failed. */
static int32_t block_transfer(int32_t endpoint, int32_t req_type, const wasmos_block_request_t* req,
                              wasmos_ipc_message_t* reply) {
    int32_t rc;
    g_req_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(*req));
    if (g_req_bid < 0) {
        return 0;
    }
    if (wasmos_xfer_buffer_write(g_req_bid, req, (int32_t)sizeof(*req), 0) != 0 ||
        wasmos_xfer_buffer_borrow(endpoint, g_req_bid, WASMOS_BUFFER_GRANT_READ) < 0) {
        (void)wasmos_xfer_buffer_release(g_req_bid);
        g_req_bid = -1;
        return 0;
    }
    rc = wasmos_ipc_call(endpoint,
                         g_reply_endpoint,
                         req_type,
                         g_request_id++,
                         g_req_bid,
                         0,
                         (int32_t)sizeof(*req),
                         0,
                         reply);
    (void)wasmos_xfer_buffer_release(g_req_bid);
    g_req_bid = -1;
    return rc == 0 ? reply->type : 0;
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

/* Describe one provider into *out_desc, or -1 when it does not answer with a
 * usable BLOCK_IPC_IDENTIFY_RESP.
 *
 * The instance argument is what a backend serving several disks needs to know
 * which one is meant -- several class instances may share one endpoint. The
 * descriptor is written into a buffer THIS process owns and lends to the backend
 * for the request, and every attribute printed below comes out of it. */
static int identify(int32_t endpoint, uint32_t instance, wasmos_block_descriptor_t* out_desc) {
    wasmos_ipc_message_t reply;
    int32_t bid;
    int rc = WASMOS_ERR_BLOCK_DEV_NOT_READY;
    if (!out_desc) {
        return WASMOS_ERR_BLOCK_DEV_BAD_REQUEST;
    }
    /* The client owns the buffer and holds its lifecycle; release cascade-revokes
     * the backend's grant on every path out of here. */
    bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(*out_desc));
    if (bid < 0) {
        return WASMOS_ERR_BLOCK_DEV_NO_DESCRIPTOR;
    }
    if (wasmos_xfer_buffer_borrow(endpoint, bid, WASMOS_BUFFER_GRANT_WRITE) < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return WASMOS_ERR_BLOCK_DEV_NO_DESCRIPTOR;
    }
    if (wasmos_ipc_call(endpoint,
                        g_reply_endpoint,
                        BLOCK_IPC_IDENTIFY_REQ,
                        g_request_id++,
                        (int32_t)instance,
                        bid,
                        0,
                        0,
                        &reply) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return WASMOS_ERR_BLOCK_DEV_NOT_READY;
    }
    /* Report the backend's own reason. "identify failed" with nothing behind it
     * cannot distinguish a disk that refused from one that answered something
     * unreadable, and the two have opposite causes. */
    if (reply.type == BLOCK_IPC_ERROR) {
        rc = (int)reply.arg0;
    } else if (reply.type != BLOCK_IPC_IDENTIFY_RESP || reply.arg0 != 0) {
        rc = WASMOS_ERR_BLOCK_DEV_UNSUPPORTED_REQUEST;
    } else if (reply.arg1 < (int32_t)sizeof(*out_desc) ||
               wasmos_xfer_buffer_read(bid, out_desc, (int32_t)sizeof(*out_desc), 0) != 0) {
        rc = WASMOS_ERR_BLOCK_DEV_DESCRIPTOR_MALFORMED;
    } else {
        rc = 0;
    }
    (void)wasmos_xfer_buffer_release(bid);
    if (rc != 0) {
        return rc;
    }
    if (out_desc->version != BLOCK_DESCRIPTOR_VERSION) {
        return WASMOS_ERR_BLOCK_DEV_DESCRIPTOR_VERSION;
    }
    /* Untrusted input from another process: an unterminated id would run off the
     * end of the field when printed. */
    out_desc->canonical_id[sizeof(out_desc->canonical_id) - 1u] = '\0';
    return 0;
}

/* Read one sector into this process's block buffer and copy its first bytes
 * out. Returns 0, or the provider's packed error code (negative) when the read
 * is refused, or -1 when the exchange itself failed. */
static int read_sector(int32_t endpoint, uint32_t instance, uint32_t lba, uint8_t* preview) {
    wasmos_ipc_message_t reply;
    wasmos_block_request_t req = {0};
    int32_t buffer_phys = wasmos_block_buffer_phys();
    if (buffer_phys < 0) {
        return -1;
    }
    req.version = BLOCK_REQUEST_VERSION;
    req.target = instance;
    req.lba = lba;
    req.sector_count = 1u;
    req.dst_kind = BLOCK_DST_BLOCK_BUFFER;
    req.dst_phys = (uint32_t)buffer_phys;
    if (block_transfer(endpoint, BLOCK_IPC_READ_REQ, &req, &reply) == 0) {
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
static int write_sector(int32_t endpoint, uint32_t instance, uint32_t lba) {
    wasmos_ipc_message_t reply;
    wasmos_block_request_t req = {0};
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
    req.version = BLOCK_REQUEST_VERSION;
    req.target = instance;
    req.lba = lba;
    req.sector_count = 1u;
    req.dst_kind = BLOCK_DST_BLOCK_BUFFER;
    req.dst_phys = (uint32_t)buffer_phys;
    if (block_transfer(endpoint, BLOCK_IPC_WRITE_REQ, &req, &reply) == 0) {
        return -1;
    }
    if (reply.type == BLOCK_IPC_ERROR) {
        return (int)reply.arg0;
    }
    return (reply.type == BLOCK_IPC_WRITE_RESP && reply.arg0 == 0) ? 0 : -1;
}

int main(void) {
    char args[128];
    svc_class_entry_t providers[BLKINFO_MAX_PROVIDERS];
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
    int32_t count;
    int32_t shown;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    const char* rest = args;
    int do_write = 0;
    char write_id[BLOCK_DESCRIPTOR_ID_MAX];
    write_id[0] = '\0';
    if (str_prefix(rest, "--write")) {
        uint32_t n = 0;
        do_write = 1;
        rest += 7;
        while (*rest == ' ') {
            rest++;
        }
        /* The device is named by its canonical id -- the string blkinfo prints
         * for every disk -- because the class instance is now an opaque
         * fingerprint that nobody can read off a screen and retype meaningfully.
         *
         * No id means no target, and writing to whatever happens to be
         * enumerated is not a reasonable default. */
        while (n + 1u < sizeof(write_id) && *rest != '\0' && *rest != ' ') {
            write_id[n++] = *rest++;
        }
        write_id[n] = '\0';
        if (n == 0u || (*rest != '\0' && *rest != ' ')) {
            (void)printf("[blkinfo] --write needs a device id: blkinfo --write <id> <lba>\n");
            return 1;
        }
        while (*rest == ' ') {
            rest++;
        }
        /* And no default sector either: falling back to 0 would put the most
         * destructive invocation one omitted argument away from a boot sector. */
        if (*rest < '0' || *rest > '9') {
            (void)printf("[blkinfo] --write needs a sector: blkinfo --write <id> <lba>\n");
            return 1;
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
        /* Zeroed for the same reason as `preview` above: identify() fills it
         * through wasmos_xfer_buffer_read, an address-as-integer host call whose
         * write no analyser can see. */
        wasmos_block_descriptor_t desc = {0};
        uint32_t sectors = 0;
        int rc;

        /* A fresh endpoint per disk; see the header. */
        g_reply_endpoint = wasmos_ipc_create_endpoint();
        if (g_reply_endpoint < 0) {
            (void)printf("[blkinfo] endpoint create failed\n");
            return 1;
        }
        /* The class instance is what names the disk to its backend: several
         * instances may share one endpoint, and nothing can be decoded out of
         * the number itself. */
        rc = identify((int32_t)providers[i].endpoint, providers[i].instance, &desc);
        if (rc != 0) {
            (void)printf("[blkinfo] instance=%u identify failed: %s\n",
                         (unsigned)providers[i].instance,
                         wasmos_error_code_name(rc));
            continue;
        }
        sectors = (uint32_t)desc.lba_count;
        (void)printf("[blkinfo] id=%s driver=%s unit=%u sectors=%u bytes=%u\n",
                     desc.canonical_id,
                     backend_name((uint8_t)desc.backend),
                     (unsigned)desc.unit,
                     (unsigned)sectors,
                     (unsigned)(sectors * BLKINFO_SECTOR_BYTES));

        if (do_write && wasmos_sys_streq(desc.canonical_id, write_id)) {
            rc = write_sector((int32_t)providers[i].endpoint, providers[i].instance, lba);
            if (rc != 0) {
                (void)printf("[blkinfo] id=%s lba=%u write failed: %s\n",
                             desc.canonical_id,
                             (unsigned)lba,
                             wasmos_error_code_name(rc));
                continue;
            }
            (void)printf("[blkinfo] id=%s lba=%u write ok\n", desc.canonical_id, (unsigned)lba);
        }

        rc = read_sector((int32_t)providers[i].endpoint, providers[i].instance, lba, preview);
        if (rc != 0) {
            (void)printf("[blkinfo] id=%s lba=%u read failed: %s\n",
                         desc.canonical_id,
                         (unsigned)lba,
                         wasmos_error_code_name(rc));
            continue;
        }
        (void)printf("[blkinfo] id=%s lba=%u data=", desc.canonical_id, (unsigned)lba);
        for (int j = 0; j < BLKINFO_PREVIEW_BYTES; ++j) {
            (void)printf("%02X", (unsigned)preview[j]);
        }
        (void)printf("\n");
    }
    return 0;
}
