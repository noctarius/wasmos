/* xfer_map_alias - does a guest's xfer-buffer mapping alias the buffer's frames?
 *
 * `xfer_buffer_map` overlays an owned buffer's backing into linear memory and
 * promises the bytes are "directly addressable at that offset". Whether that
 * holds for a guest depends on the runtime's linear-memory model, and the two
 * models differ: under WARP the guest's linear memory IS the mapped frames,
 * while the wasm3 interpreter reads and writes linear memory through its own
 * kernel-side buffer, so a mapping that only rewrote the process page tables
 * would be invisible to it.
 *
 * Every graphics client depends on that promise: a surface is rendered through
 * this mapping and read by the compositor through its borrow, with no
 * write-back call between the two. This pins it as a fact rather than an
 * assumption. Nothing here races or depends on timing: the mapping either
 * aliases or it does not, and one comparison settles it.
 *
 * Both directions are checked, because the migration needs both:
 *   guest -> frames  write a pattern THROUGH the mapping, then read it back with
 *                    wasmos_xfer_buffer_read, which reaches the frames through
 *                    the kernel rather than through linear memory.
 *   frames -> guest  write a second pattern with wasmos_xfer_buffer_write, then
 *                    read it back THROUGH the mapping.
 *
 * A failure of the first direction is what would make a compositor see stale
 * pixels; a failure of the second would make an app see stale input. Each
 * reports its own marker so a log says which half broke.
 */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

#define PROBE_BYTES 256

/* Distinct, non-zero, and not each other's shift: a mapping that aliases nothing
 * reads back as zeros, and a half-written one does not match either pattern. */
#define PATTERN_GUEST 0xA5
#define PATTERN_KERNEL 0x5C

static void fill(uint8_t* p, uint8_t byte, int32_t len) {
    for (int32_t i = 0; i < len; ++i) {
        p[i] = byte;
    }
}

static int all_equal(const uint8_t* p, uint8_t byte, int32_t len) {
    for (int32_t i = 0; i < len; ++i) {
        if (p[i] != byte) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    int32_t bid = wasmos_xfer_buffer_acquire(PROBE_BYTES);
    if (bid <= 0) {
        printf("[test] xfer map alias acquire failed rc=%d\n", (int)bid);
        return 1;
    }

    int32_t off = wasmos_xfer_buffer_map(bid);
    if (off < 0) {
        printf("[test] xfer map alias map failed rc=%d\n", (int)off);
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    uint8_t* mapped = addr_cast(uint8_t*, off);

    /* guest -> frames. The read goes through the kernel, so it observes the
     * object's frames rather than whatever linear memory holds. */
    fill(mapped, PATTERN_GUEST, PROBE_BYTES);
    uint8_t seen[PROBE_BYTES];
    fill(seen, 0, PROBE_BYTES);
    int32_t rc = wasmos_xfer_buffer_read(bid, seen, PROBE_BYTES, 0);
    if (rc != 0) {
        printf("[test] xfer map alias read failed rc=%d\n", (int)rc);
    } else if (all_equal(seen, PATTERN_GUEST, PROBE_BYTES)) {
        putsn("[test] xfer map alias guest-to-frames ok\n", 41);
    } else {
        printf("[test] xfer map alias guest-to-frames MISSING first=%02x\n", (unsigned)seen[0]);
    }

    /* frames -> guest. The write goes through the kernel; the mapping must show
     * it without any refresh call. */
    uint8_t src[PROBE_BYTES];
    fill(src, PATTERN_KERNEL, PROBE_BYTES);
    rc = wasmos_xfer_buffer_write(bid, src, PROBE_BYTES, 0);
    if (rc != 0) {
        printf("[test] xfer map alias write failed rc=%d\n", (int)rc);
    } else if (all_equal(mapped, PATTERN_KERNEL, PROBE_BYTES)) {
        putsn("[test] xfer map alias frames-to-guest ok\n", 41);
    } else {
        printf("[test] xfer map alias frames-to-guest MISSING first=%02x\n", (unsigned)mapped[0]);
    }

    (void)wasmos_xfer_buffer_unmap(bid);
    (void)wasmos_xfer_buffer_release(bid);
    putsn("[test] xfer map alias done\n", 27);
    return 0;
}
