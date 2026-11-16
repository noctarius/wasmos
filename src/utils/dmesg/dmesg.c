/* dmesg - print the kernel log.
 *
 * Reads the kernel log ring through kmsg_read, from the oldest retained byte to
 * the end, and writes it to the caller's console. The ring holds the newest
 * 64 KiB of kernel log only -- boot output, service and driver lines -- and
 * never a process's own console output, so this shows what the kernel logged
 * regardless of which tty the reader is on or where serial is bound.
 *
 * Paging is by sequence number, which never wraps: when the oldest bytes were
 * already overwritten at the first read, or are overwritten between two reads,
 * the call moves the reader forward and the difference is printed as a marker
 * instead of being passed over silently.
 *
 * The kernel keeps logging while this runs, so the read is bounded to twice the
 * ring size; a log growing faster than it can be printed still ends.
 */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"

#define DMESG_RING_BYTES (64u * 1024u)
#define DMESG_MAX_BYTES (2u * DMESG_RING_BYTES)

static char g_buf[1024];

int main(void) {
    uint64_t seq = 0;
    uint32_t total = 0;
    int first = 1;

    while (total < DMESG_MAX_BYTES) {
        uint64_t want = seq;
        int32_t n = wasmos_kmsg_read(g_buf, (int32_t)sizeof(g_buf), &seq);
        if (n < 0) {
            printf("dmesg: kmsg_read failed (%d)\n", (int)n);
            return 1;
        }
        if (seq != want) {
            if (first) {
                printf("[dmesg] %llu earlier bytes not retained\n", (unsigned long long)seq);
            } else {
                printf("\n[dmesg] %llu bytes lost while reading\n",
                       (unsigned long long)(seq - want));
            }
        }
        first = 0;
        if (n == 0) {
            break;
        }
        if (putsn(g_buf, (size_t)n) != 0) {
            return 1;
        }
        seq += (uint64_t)n;
        total += (uint32_t)n;
    }
    return 0;
}
