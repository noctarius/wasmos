#include "phys_map.h"
#include "sync/spinlock.h"

/* Shared by every process. Only mmio.map holders map windows, and a range inside
 * a held window reuses it; acpi-bus, which holds one or two per firmware table for
 * its lifetime, stays well inside this. A full table refuses the map rather than
 * evicting a window. */
#define PHYS_MAP_WINDOW_SLOTS 128u

static phys_map_window_t g_windows[PHYS_MAP_WINDOW_SLOTS];
static uint8_t g_valid[PHYS_MAP_WINDOW_SLOTS];
static ksync_spinlock_t g_phys_map_lock;

int phys_map_span(uint64_t phys, uint64_t size, uint64_t* out_page, uint32_t* out_pages) {
    if (phys == 0 || size == 0 || !out_page || !out_pages) {
        return -1;
    }
    uint64_t end = phys + size;
    if (end < phys || end > ~0xFFFULL) {
        return -1;
    }
    uint64_t page = phys & ~0xFFFULL;
    uint64_t pages = ((end + 0xFFFULL) & ~0xFFFULL) - page;
    pages >>= 12;
    if (pages > PHYS_MAP_MAX_PAGES) {
        return -1;
    }
    *out_page = page;
    *out_pages = (uint32_t)pages;
    return 0;
}

int phys_map_window_find(uint32_t pid, uint64_t phys, uint64_t size, uint32_t* out_off) {
    uint64_t end = phys + size;
    if (size == 0 || end < phys || !out_off) {
        return 0;
    }
    int found = 0;
    ksync_spinlock_lock(&g_phys_map_lock);
    for (uint32_t i = 0; i < PHYS_MAP_WINDOW_SLOTS; ++i) {
        const phys_map_window_t* w = &g_windows[i];
        if (!g_valid[i] || w->pid != pid) {
            continue;
        }
        uint64_t w_end = w->phys + (uint64_t)w->pages * 0x1000ULL;
        if (phys >= w->phys && end <= w_end) {
            *out_off = w->offset + (uint32_t)(phys - w->phys);
            found = 1;
            break;
        }
    }
    ksync_spinlock_unlock(&g_phys_map_lock);
    return found;
}

int phys_map_window_record(const phys_map_window_t* window) {
    if (!window) {
        return -1;
    }
    int rc = -1;
    ksync_spinlock_lock(&g_phys_map_lock);
    for (uint32_t i = 0; i < PHYS_MAP_WINDOW_SLOTS; ++i) {
        if (!g_valid[i]) {
            g_windows[i] = *window;
            g_valid[i] = 1;
            rc = 0;
            break;
        }
    }
    ksync_spinlock_unlock(&g_phys_map_lock);
    return rc;
}

void phys_map_window_release_pid(uint32_t pid, void (*unmap)(const phys_map_window_t* window)) {
    for (uint32_t i = 0; i < PHYS_MAP_WINDOW_SLOTS; ++i) {
        phys_map_window_t taken;
        int hit = 0;
        ksync_spinlock_lock(&g_phys_map_lock);
        if (g_valid[i] && g_windows[i].pid == pid) {
            taken = g_windows[i];
            g_valid[i] = 0;
            hit = 1;
        }
        ksync_spinlock_unlock(&g_phys_map_lock);
        /* Called outside the lock: unmapping takes paging locks of its own. */
        if (hit && unmap) {
            unmap(&taken);
        }
    }
}
