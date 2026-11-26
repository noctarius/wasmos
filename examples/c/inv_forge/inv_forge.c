/* inv_forge - an unprivileged process forging a device-inventory record.
 *
 * device-manager's inventory endpoint ("devmgr.inv") is discoverable by name,
 * and the records published to it decide which drivers device-manager spawns
 * and what hardware it grants them. This app holds no inventory capability; it
 * publishes a block device that does not exist, canonical id "block:forged:0",
 * exactly as a block driver would. Device-manager must refuse it.
 *
 * Log lines go to the kernel log, "[inv-forge] ...". */
#include <stdint.h>

#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

static void forge_log(const char* what) {
    char line[96];
    (void)snprintf(line, sizeof(line), "[inv-forge] %s\n", what);
    (void)wasmos_console_write(addr_cast(int32_t, line), (int32_t)strlen(line));
}

int main(void) {
    int32_t proc_ep = wasmos_startup_proc_endpoint();
    int32_t self_ep = wasmos_ipc_create_endpoint();
    if (proc_ep < 0 || self_ep < 0) {
        forge_log("no endpoint");
        return 1;
    }
    int32_t inv_ep = wasmos_svc_lookup(proc_ep, self_ep, "devmgr.inv", 1);
    if (inv_ep < 0) {
        forge_log("devmgr.inv not found");
        return 1;
    }

    wasmos_block_descriptor_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.version = BLOCK_DESCRIPTOR_VERSION;
    desc.sector_bytes = 512u;
    desc.flags = BLOCK_DESCRIPTOR_FLAG_PRESENT;
    desc.lba_count = 8u;
    (void)snprintf(desc.canonical_id, sizeof(desc.canonical_id), "block:forged:0");

    int32_t bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(desc));
    if (bid < 0 || wasmos_xfer_buffer_write(bid, &desc, (int32_t)sizeof(desc), 0) != 0 ||
        wasmos_xfer_buffer_borrow(inv_ep, bid, WASMOS_BUFFER_GRANT_READ) < 0) {
        forge_log("buffer setup failed");
        return 1;
    }
    (void)wasmos_ipc_send(
        inv_ep, self_ep, DEVMGR_PUBLISH_BLOCK_DEVICE, 0, bid, 0, (int32_t)sizeof(desc), 0);
    forge_log("published block:forged:0");
    return 0;
}
