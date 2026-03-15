#ifndef WASMOS_FRAMEBUFFER_H
#define WASMOS_FRAMEBUFFER_H

#include <stdint.h>
#include "boot.h"

typedef struct framebuffer_info {
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;
    uint32_t framebuffer_reserved;
} framebuffer_info_t;

void framebuffer_init(const boot_info_t *info);
int framebuffer_get_info(framebuffer_info_t *out);
int framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color);

#endif
