/* console_claim_deny - an ordinary app must not be able to take the console.
 *
 * Registering a console ring (klog_register_ring) hands the registrant every
 * tty's output and the kernel log, and ownership of COM1 through
 * serial_owner_write; a second registration replaces the first. This app holds
 * only ipc.basic and builds a well-formed ring in a transfer buffer it owns --
 * every check except the console.owner capability is satisfied -- then tries.
 *
 * Prints "console-claim-deny: ok" and exits 0 when the call returns
 * WASMOS_ERR_KERNEL_NOT_AUTHORIZED. If the kernel accepts it, the vt stops
 * receiving console output, so this app's own report and the shell's next
 * prompt never reach the screen or serial: the failure shows as their absence. */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ringbuf.h"
#include "wasmos_cast.h"

#define CLAIM_RING_CAPACITY 4096u

static wasmos_ringbuf_t g_ring;

int main(void) {
    uint32_t region = wasmos_ringbuf_bytes_for(CLAIM_RING_CAPACITY);
    int32_t bid = wasmos_xfer_buffer_acquire((int32_t)region);
    if (bid <= 0) {
        puts("console-claim-deny: acquire failed");
        return 1;
    }
    int32_t off = wasmos_xfer_buffer_map(bid);
    if (off < 0) {
        puts("console-claim-deny: map failed");
        return 1;
    }
    if (wasmos_ringbuf_init(&g_ring, ptr_cast(void, (uint32_t)off), region, CLAIM_RING_CAPACITY) !=
        0) {
        puts("console-claim-deny: ring init failed");
        return 1;
    }
    int32_t rc = wasmos_klog_register_ring(bid, 0);
    if (rc != WASMOS_ERR_KERNEL_NOT_AUTHORIZED) {
        printf("console-claim-deny: expected deny, got %d\n", (int)rc);
        return 1;
    }
    puts("console-claim-deny: ok");
    return 0;
}
