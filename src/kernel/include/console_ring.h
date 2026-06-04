/* console_ring.h - Lock-free single-producer/single-consumer ring buffer for console I/O.
 * Sized to fit in one 4 KB page with write_pos/read_pos as the producer/consumer cursors.
 * Used to share the kernel serial output ring with user-space console readers. */
#ifndef WASMOS_CONSOLE_RING_H
#define WASMOS_CONSOLE_RING_H

#include <stdint.h>

#define CONSOLE_RING_DATA_SIZE 4080u  /* data bytes; total struct is 4096 bytes */

typedef struct {
    volatile uint32_t write_pos;  /* producer cursor (kernel writes here) */
    volatile uint32_t read_pos;   /* consumer cursor (user-space reader) */
    uint32_t capacity;
    uint32_t _pad;
    uint8_t data[CONSOLE_RING_DATA_SIZE];
} console_ring_t;

#endif
