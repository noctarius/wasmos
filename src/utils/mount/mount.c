/* mount.c - report the filesystems making up the VFS namespace.
 *
 *   mount
 *
 * Prints fs-manager's mount table: one line per mount, giving the absolute path
 * it occupies and the filesystem serving it, plus the PCI address, class and
 * unit of the block device behind a disk-backed one. The table is fs-manager's
 * and is asked for whole (FSMGR_IPC_QUERY_MOUNTS_REQ) rather than assembled
 * here, so what this prints is what routing actually uses -- a listing built
 * from any other source could disagree with it.
 *
 * The listing travels in a transfer buffer this tool OWNS and grants fs-manager
 * WRITE, which is the owner-push shape every buffer-carrying request uses: the
 * client of an exchange owns the buffer and the server is a transient grantee
 * (docs/architecture/12-dma-transfers.md).
 *
 * Failures print the error's SYMBOLIC name ("fs.NO_BACKEND"), not its
 * description: the descriptions in abi/errors.yaml are reference documentation
 * several sentences long, which is not a shell message.
 *
 * TODO: establishing a mount is not yet a request -- a filesystem is placed by
 * whoever spawns its driver (a device-manager rule's ENV{MOUNT}), so this tool
 * reports mounts and `umount` removes them, but nothing creates one at runtime.
 */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"

/* Largest listing accepted in one reply. fs-manager assembles the table into a
 * buffer of its own before writing it out, and refusing to grow past that
 * costs nothing: a bigger buffer here would be written short anyway. */
#define MOUNT_LISTING_MAX 512

#define MOUNT_EXIT_OK 0
#define MOUNT_EXIT_USAGE 1
#define MOUNT_EXIT_FAILED 2

/* Attempts at looking up "fs.vfs". A util can be spawned from a boot script
 * while fs-manager is still registering, and one failed lookup is not evidence
 * the service is absent. */
#define MOUNT_LOOKUP_ATTEMPTS 8

/* Look up "fs.vfs", yielding between attempts. wasmos_svc_lookup does not retry
 * by contract, so the loop belongs to the caller. */
static int32_t fs_lookup(int32_t proc_endpoint, int32_t reply_endpoint, int32_t attempts) {
    for (int32_t i = 0; i < attempts; ++i) {
        int32_t ep = wasmos_svc_lookup(proc_endpoint, reply_endpoint, "fs.vfs", i + 1);
        if (ep >= 0) {
            return ep;
        }
        (void)wasmos_sched_yield();
    }
    return -1;
}

static void fail(const char* what, int32_t code) {
    printf("mount: %s: %s\n", what, wasmos_error_code_name((wasmos_error_code_t)code));
}

int main(void) {
    char args[64];
    char listing[MOUNT_LISTING_MAX];
    wasmos_ipc_message_t reply;
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
    int32_t reply_endpoint;
    int32_t fs_endpoint;
    int32_t bid;
    int32_t grant;
    int32_t rc;
    int32_t len;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    if (args[0] != '\0') {
        puts("usage: mount");
        return MOUNT_EXIT_USAGE;
    }

    reply_endpoint = wasmos_ipc_create_endpoint();
    if (reply_endpoint < 0) {
        fail("endpoint", reply_endpoint);
        return MOUNT_EXIT_FAILED;
    }
    fs_endpoint = fs_lookup(proc_endpoint, reply_endpoint, MOUNT_LOOKUP_ATTEMPTS);
    if (fs_endpoint < 0) {
        puts("mount: no filesystem service");
        return MOUNT_EXIT_FAILED;
    }

    bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(listing));
    if (bid < 0) {
        fail("buffer", bid);
        return MOUNT_EXIT_FAILED;
    }
    /* WRITE, not READ: fs-manager is the one filling this buffer. */
    grant = wasmos_xfer_buffer_borrow(fs_endpoint, bid, WASMOS_BUFFER_GRANT_WRITE);
    if (grant < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        fail("grant", grant);
        return MOUNT_EXIT_FAILED;
    }
    rc = wasmos_ipc_call(
        fs_endpoint, reply_endpoint, FSMGR_IPC_QUERY_MOUNTS_REQ, 1, 0, 0, bid, grant, &reply);
    /* The grant is torn down by releasing the buffer this tool owns; a client
     * never unborrows a grant it lent (the borrower would have to). */
    if (rc != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        fail("query", rc);
        return MOUNT_EXIT_FAILED;
    }
    if ((int32_t)reply.type != FSMGR_IPC_QUERY_MOUNTS_RESP) {
        (void)wasmos_xfer_buffer_release(bid);
        fail("query", reply.arg0 != 0 ? reply.arg0 : (int32_t)WASMOS_ERR_FS_BACKEND_IPC);
        return MOUNT_EXIT_FAILED;
    }
    len = reply.arg0;
    if (len <= 0 || len >= (int32_t)sizeof(listing)) {
        (void)wasmos_xfer_buffer_release(bid);
        puts("mount: empty listing");
        return MOUNT_EXIT_FAILED;
    }
    if (wasmos_xfer_buffer_read(bid, listing, len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        fail("read", WASMOS_ERR_FS_BUFFER);
        return MOUNT_EXIT_FAILED;
    }
    (void)wasmos_xfer_buffer_release(bid);
    listing[len] = '\0';
    /* Already newline-terminated per line by fs-manager; printed verbatim so the
     * shell shows the table exactly as the service reports it. */
    printf("%s", listing);
    return MOUNT_EXIT_OK;
}
