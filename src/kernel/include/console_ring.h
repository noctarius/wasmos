/* console_ring.h - Lock-free single-producer/single-consumer ring buffer for console I/O.
 * Sized to fit in one 4 KB page with write_pos/read_pos as the producer/consumer cursors.
 * Used to share the kernel serial output ring with user-space console readers. */
#ifndef WASMOS_CONSOLE_RING_H
#define WASMOS_CONSOLE_RING_H

#include <stdint.h>

#ifndef WASMOS_CONSOLE_RING_SHARED_H
#define WASMOS_CONSOLE_RING_SHARED_H
/* 4096 minus the 16-byte header, so the whole struct is exactly one page and can be
 * handed across as a single shared-memory frame. */
#define CONSOLE_RING_DATA_SIZE 4080u /* data bytes; total struct is 4096 bytes */

/* Both cursors are free-running byte counts; the position in `data` is the cursor modulo
 * capacity.  The ring is lossy by design: the producer never waits for the consumer, so a
 * reader that falls more than capacity bytes behind loses the oldest bytes.  Exactly one
 * producer and one consumer are supported — the cursors are plain volatile words with no
 * lock and no atomics, so a second writer or reader on either side corrupts the stream. */
typedef struct {
    volatile uint32_t write_pos; /* producer cursor (kernel writes here) */
    volatile uint32_t read_pos;  /* consumer cursor (user-space reader) */
    uint32_t capacity;           /* usable data bytes; CONSOLE_RING_DATA_SIZE */
    uint32_t _pad;               /* keeps `data` 16-byte aligned; not read */
    uint8_t data[CONSOLE_RING_DATA_SIZE];
} console_ring_t;
#endif

#endif
