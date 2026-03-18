#include "wasmos_native_driver.h"

/*
 * Native framebuffer driver.
 *
 * Probes the kernel framebuffer, maps it directly into this process's address
 * space via the driver API, and paints a gradient.  Because this runs as
 * compiled x86-64 code rather than through the wasm3 interpreter, the inner
 * pixel loop executes at full native speed.
 */

static int
str_len(const char *s)
{
    int n = 0;
    while (s[n]) { ++n; }
    return n;
}

static void
write_str(wasmos_driver_api_t *api, const char *s)
{
    api->console_write(s, str_len(s));
}

static void
paint_gradient(uint32_t *fb, int width, int height, int stride)
{
    int wh = width + height;
    for (int y = 0; y < height; ++y) {
        int g = (y * 255) / height;
        int by = (y * 128) / wh;
        uint32_t *row = fb + (uint32_t)y * (uint32_t)stride;
        for (int x = 0; x < width; ++x) {
            int r = (x * 255) / width;
            int b = ((x * 128) / wh + by) & 0xff;
            row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

int
initialize(wasmos_driver_api_t *api, int module_count, int arg2, int arg3)
{
    (void)module_count;
    (void)arg2;
    (void)arg3;

    write_str(api, "[framebuffer-native] probing\n");

    nd_framebuffer_info_t info;
    if (api->framebuffer_info(&info) != 0 ||
        info.framebuffer_base == 0 ||
        info.framebuffer_width == 0 ||
        info.framebuffer_height == 0 ||
        info.framebuffer_stride == 0) {
        write_str(api, "[framebuffer-native] not present\n");
        return 0;
    }

    /* Round size up to the next page boundary before mapping. */
    uint32_t size = (uint32_t)info.framebuffer_size;
    size = (size + 0xfffu) & ~0xfffu;

    write_str(api, "[framebuffer-native] mapping\n");
    void *fb = api->framebuffer_map(size);
    if (!fb) {
        write_str(api, "[framebuffer-native] map failed\n");
        return -1;
    }

    write_str(api, "[framebuffer-native] painting\n");
    paint_gradient((uint32_t *)fb,
                   (int)info.framebuffer_width,
                   (int)info.framebuffer_height,
                   (int)info.framebuffer_stride);

    write_str(api, "[framebuffer-native] done\n");
    return 0;
}