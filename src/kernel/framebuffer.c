#include "framebuffer.h"

#include <stdint.h>
#include <string.h>

static framebuffer_info_t g_framebuffer_info = {0};

void framebuffer_init(const boot_info_t *info)
{
    if (!info || !(info->flags & BOOT_INFO_FLAG_GOP_PRESENT) ||
        !info->framebuffer_base || info->framebuffer_size == 0 ||
        info->framebuffer_width == 0 || info->framebuffer_height == 0) {
        return;
    }
    g_framebuffer_info.framebuffer_base = (uint64_t)(uintptr_t)info->framebuffer_base;
    g_framebuffer_info.framebuffer_size = info->framebuffer_size;
    g_framebuffer_info.framebuffer_width = info->framebuffer_width;
    g_framebuffer_info.framebuffer_height = info->framebuffer_height;
    g_framebuffer_info.framebuffer_stride = info->framebuffer_pixels_per_scanline;
}

int framebuffer_get_info(framebuffer_info_t *out)
{
    if (!out || g_framebuffer_info.framebuffer_base == 0 ||
        g_framebuffer_info.framebuffer_size == 0 ||
        g_framebuffer_info.framebuffer_width == 0 ||
        g_framebuffer_info.framebuffer_height == 0 ||
        g_framebuffer_info.framebuffer_stride == 0) {
        return -1;
    }
    memcpy(out, &g_framebuffer_info, sizeof(framebuffer_info_t));
    return 0;
}

int framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (g_framebuffer_info.framebuffer_base == 0 ||
        g_framebuffer_info.framebuffer_size == 0 ||
        g_framebuffer_info.framebuffer_width == 0 ||
        g_framebuffer_info.framebuffer_height == 0) {
        return -1;
    }
    if (x >= g_framebuffer_info.framebuffer_width ||
        y >= g_framebuffer_info.framebuffer_height) {
        return -1;
    }
    uint64_t stride = g_framebuffer_info.framebuffer_stride;
    if (stride == 0) {
        return -1;
    }
    uint64_t index = (uint64_t)y * stride + x;
    uint64_t offset = index * 4;
    if (offset + 4 > g_framebuffer_info.framebuffer_size) {
        return -1;
    }
    uint32_t *pixel = (uint32_t *)(uintptr_t)(g_framebuffer_info.framebuffer_base + offset);
    *pixel = color;
    return 0;
}
