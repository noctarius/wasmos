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

/* Next address the bump allocator hands out, in bytes, page-aligned and only
 * ever increasing. Starts at 1 MiB so a returned base is never 0, which is the
 * allocator's failure value. */
static uint64_t g_next_phys = 0x00100000ULL;

/* Returns the base of a fresh run of `pages` 4 KiB pages, or 0 for pages == 0 --
 * the same failure value the real allocator uses when it cannot satisfy a
 * request. That zero-page case is the ONLY failure reachable here: there is no
 * free pool to exhaust, so a caller's out-of-memory path stays uncovered. The
 * range is not zeroed and its contents are undefined; nothing maps it, so the
 * address is an identity token rather than memory a test may dereference. */
uint64_t pfa_alloc_pages(uint64_t pages) {
    uint64_t base = g_next_phys;

    if (pages == 0) {
        return 0;
    }
    g_next_phys += pages * 4096u;
    return base;
}

/* Accepts any (base, pages) and returns the frames nowhere. The real allocator
 * refcounts frames, releasing one only when its count reaches zero and panicking
 * on a free below zero; here a double free, a free of a range never allocated,
 * and a free of a pinned range are all indistinguishable no-ops. A leak on the
 * path under test is therefore invisible, and so is a double-free. */
void pfa_free_pages(uint64_t base, uint64_t pages) {
    (void)base;
    (void)pages;
}

/* Fills *out with the fixed geometry above and returns WASMOS_OK (0), or
 * WASMOS_INVAL (-1) for a NULL out. A framebuffer is always reported, so the
 * real function's WASMOS_ERR_FRAMEBUFFER_NOT_PRESENT arm -- and any caller
 * fallback behind it -- is unreachable in these suites. The base address names
 * no mapped memory: it bounds a framebuffer-kind acquire and must not be
 * dereferenced. */
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
