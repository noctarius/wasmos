/* stubs_xfer_buffer_platform.c - host stand-ins for the two platform services
 * the xfer-buffer object registry needs: the physical frame allocator and the
 * framebuffer geometry.
 *
 * pfa_alloc_pages is a bump allocator that never reuses a range and never
 * fails, and pfa_free_pages returns nothing to it. Every object therefore gets
 * a distinct, page-aligned backing address for as long as the process runs,
 * which is what the identity/distinctness checks in the suite rely on; the
 * allocator-exhaustion and address-reuse paths are out of reach here.
 *
 * The reported framebuffer is a fixed 1024x768x4 (3 MiB) at a page-aligned
 * base, so it is also the intrinsic capacity a BUFFER_KIND_FRAMEBUFFER
 * acquire is bounded by. */
#include <stdint.h>

#include "framebuffer.h"

static uint64_t g_next_phys = 0x00100000ULL;

uint64_t pfa_alloc_pages(uint64_t pages) {
    uint64_t base = g_next_phys;

    if (pages == 0) {
        return 0;
    }
    g_next_phys += pages * 4096u;
    return base;
}

void pfa_free_pages(uint64_t base, uint64_t pages) {
    (void)base;
    (void)pages;
}

int framebuffer_get_info(framebuffer_info_t* out) {
    if (!out) {
        return -1;
    }
    out->framebuffer_base = 0x04000000ULL;
    out->framebuffer_size = 1024u * 768u * 4u;
    out->framebuffer_width = 1024u;
    out->framebuffer_height = 768u;
    out->framebuffer_stride = 1024u * 4u;
    out->framebuffer_gop_pixel_format = 0u;
    return 0;
}
