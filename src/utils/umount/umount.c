/* umount.c - remove a filesystem from the VFS namespace.
 *
 *   umount <path>
 *
 * The mount is named by the PATH it occupies, which is what `mount` reports and
 * the only handle a client has on it: which process serves a filesystem is not
 * something a caller knows or should need to. The path is resolved against the
 * working directory, so `umount .` names the mount you are standing in -- and is
 * refused for exactly that reason.
 *
 * fs-manager refuses while anything still stands in the mount
 * (WASMOS_ERR_FS_MOUNT_BUSY): a deeper mount inside it, an open file on it, or a
 * client whose working directory is under it. That is a statement about the
 * namespace rather than a shortage, so it is not retried here -- it clears when
 * whoever is standing there leaves. `/` is normally busy for this reason, since
 * every client starts there.
 *
 * On success the backend has been quiesced and dropped, and the mount POINT is
 * left behind: it is a directory in the covering filesystem, so whatever that
 * filesystem holds there becomes visible again.
 *
 * The path travels in a transfer buffer this tool OWNS and grants fs-manager
 * READ (docs/architecture/12-dma-transfers.md); a mount path is not bounded by
 * anything an IPC argument word can carry.
 *
 * Refusals print the error's SYMBOLIC name ("fs.MOUNT_BUSY"), not its
 * description: the descriptions in abi/errors.yaml are reference documentation
 * several sentences long, which is not a shell message.
 */
#include <stdint.h>

#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"

/* Longest path accepted. fs-manager refuses anything past its own cwd ceiling,
 * so a larger buffer here would only defer the refusal. */
#define UMOUNT_PATH_MAX 256

#define UMOUNT_EXIT_OK 0
#define UMOUNT_EXIT_USAGE 1
#define UMOUNT_EXIT_FAILED 2

#define UMOUNT_LOOKUP_ATTEMPTS 8

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

int main(void) {
    char path[UMOUNT_PATH_MAX];
    wasmos_ipc_message_t reply;
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
    int32_t reply_endpoint;
    int32_t fs_endpoint;
    int32_t bid;
    int32_t grant;
    int32_t rc;
    uint32_t len;

    path[0] = '\0';
    (void)wasmos_startup_args(path, sizeof(path));
    /* Trailing whitespace from the shell line would become part of the path and
     * name no mount at all. */
    len = (uint32_t)strlen(path);
    while (len > 0u && (path[len - 1u] == ' ' || path[len - 1u] == '\t')) {
        path[--len] = '\0';
    }
    if (len == 0u) {
        puts("usage: umount <path>");
        return UMOUNT_EXIT_USAGE;
    }

    reply_endpoint = wasmos_ipc_create_endpoint();
    if (reply_endpoint < 0) {
        printf("umount: %s\n", wasmos_error_code_name((wasmos_error_code_t)reply_endpoint));
        return UMOUNT_EXIT_FAILED;
    }
    fs_endpoint = fs_lookup(proc_endpoint, reply_endpoint, UMOUNT_LOOKUP_ATTEMPTS);
    if (fs_endpoint < 0) {
        puts("umount: no filesystem service");
        return UMOUNT_EXIT_FAILED;
    }

    bid = wasmos_xfer_buffer_acquire((int32_t)len);
    if (bid < 0) {
        printf("umount: %s\n", wasmos_error_code_name((wasmos_error_code_t)bid));
        return UMOUNT_EXIT_FAILED;
    }
    if (wasmos_xfer_buffer_write(bid, (const uint8_t*)path, (int32_t)len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        printf("umount: %s\n", wasmos_error_code_name(WASMOS_ERR_FS_BUFFER));
        return UMOUNT_EXIT_FAILED;
    }
    grant = wasmos_xfer_buffer_borrow(fs_endpoint, bid, WASMOS_BUFFER_GRANT_READ);
    if (grant < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        printf("umount: %s\n", wasmos_error_code_name((wasmos_error_code_t)grant));
        return UMOUNT_EXIT_FAILED;
    }
    rc = wasmos_ipc_call(
        fs_endpoint, reply_endpoint, FSMGR_IPC_UNMOUNT_REQ, 1, (int32_t)len, 0, bid, grant, &reply);
    (void)wasmos_xfer_buffer_release(bid);
    if (rc != 0) {
        printf("umount: %s: %s\n", path, wasmos_error_code_name((wasmos_error_code_t)rc));
        return UMOUNT_EXIT_FAILED;
    }
    if ((int32_t)reply.type != FSMGR_IPC_UNMOUNT_RESP || reply.arg0 != 0) {
        int32_t code = reply.arg0 != 0 ? reply.arg0 : (int32_t)WASMOS_ERR_FS_BACKEND_IPC;
        printf("umount: %s: %s\n", path, wasmos_error_code_name((wasmos_error_code_t)code));
        return UMOUNT_EXIT_FAILED;
    }
    printf("umount: %s unmounted\n", path);
    return UMOUNT_EXIT_OK;
}
