#ifndef WASMOS_CONSOLE_RING_H
#define WASMOS_CONSOLE_RING_H

#include <stdint.h>

#define CONSOLE_RING_DATA_SIZE 4080u

typedef struct {
    volatile uint32_t write_pos;
    volatile uint32_t read_pos;
    uint32_t capacity;
    uint32_t _pad;
    uint8_t data[CONSOLE_RING_DATA_SIZE];
} console_ring_t;

#endif
